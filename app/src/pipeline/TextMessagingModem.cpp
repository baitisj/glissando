//=========================================================================
// Name:            TextMessagingModem.cpp
// Purpose:         Carries text messaging frames over codec2 data modes.
//
// Authors:         FreeDV text messaging contributors
// License:
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//=========================================================================

#include "TextMessagingModem.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "freedv_api.h"
#include "util/logging/ulog.h"

using namespace TextMessaging;

namespace
{

// Gap left between the bursts that make up one message, so the receiving
// modem has time to drop sync and look for the next preamble.
constexpr int INTER_BURST_GAP_MS = 100;
constexpr int MODEM_SAMPLE_RATE = 8000;

// Enough audio to hold several bursts if the tap thread falls behind. Beyond
// this the oldest samples are dropped: they are older than any burst we could
// still decode.
constexpr int MAX_BUFFERED_SAMPLES = MODEM_SAMPLE_RATE * 4;

// Glissando is constant envelope, so its peak is its level: -6 dBFS leaves
// the transmit level control room either way.
constexpr float GLISSANDO_PEAK = 16384.0f;

// Allowance for a receiver search to finish once its audio is in.
constexpr double GLISSANDO_SEARCH_SECONDS = 1.0;
constexpr uint64_t CHORUS_ANNOUNCE_INTERVAL_MS = 60000;
constexpr uint64_t CHORUS_STATION_TIMEOUT_MS = 300000;

// Automatic gear shifting follows the last frame heard for this long, then
// falls back to the tempo chosen by hand: an old report says nothing about
// the band now.
constexpr int GLISSANDO_REPORT_LIFETIME_MS = 15 * 60 * 1000;

uint64_t steadyMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Set FREEDV_TEXT_CHAT_RX_LOG to have every burst the receivers finish decoding
// reported: what each frame was, from whom, which fragment of which message,
// and every burst that was heard but failed its CRC. A field report of a
// message "partly received" cannot be analysed without it.
bool rxLogEnabled()
{
    static const bool enabled = std::getenv("FREEDV_TEXT_CHAT_RX_LOG") != nullptr;
    return enabled;
}

const char* frameTypeName(FrameType type)
{
    switch (type)
    {
        case FrameType::Ping: return "ping";
        case FrameType::PingAck: return "pong";
        case FrameType::Message: return "message";
        case FrameType::MessageAck: return "ack";
        case FrameType::Broadcast: return "broadcast";
        case FrameType::MessagePartialAck: return "partial ack";
    }
    return "unknown";
}

float modemSnr(struct freedv* modem)
{
    int sync = 0;
    float snr = 0.0f;
    freedv_get_modem_stats(modem, &sync, &snr);
    return snr;
}

uint8_t packChorusChannel(int gear, Glissando::Scale scale, int degree)
{
    int degreeIndex = 0;
    for (; degreeIndex < Glissando::SCALE_DEGREE_COUNT; ++degreeIndex)
        if (Glissando::SCALE_DEGREES[degreeIndex] == degree) break;
    if (gear < Glissando::MIN_GEAR || gear > Glissando::MAX_GEAR ||
        degreeIndex >= Glissando::SCALE_DEGREE_COUNT)
        return 0;
    return (uint8_t)(((gear - 1) << 5) | ((int)scale << 3) | (degreeIndex << 1) | 1);
}

bool unpackChorusChannel(uint8_t packed, int& gear, Glissando::Scale& scale, int& degree);

Glissando::Payload makeChorusBeacon(const std::string& callsign, int gear,
                                    Glissando::Scale scale, int degree)
{
    Glissando::Payload payload{};
    uint8_t packedCallsign[6]{};
    FrameCodec::packCallsign(callsign, packedCallsign);
    uint8_t body[Glissando::SEGMENT_DATA_BYTES]{};
    body[0] = 0xC7;
    std::memcpy(body + 1, packedCallsign, sizeof(packedCallsign));
    body[7] = packChorusChannel(gear, scale, degree);
    body[8] = 0xA5;
    payload[0] = 0; // signalling
    payload[1] = payload[2] = payload[3] = 1; // reserved segment index 7
    payload[4] = 1; // final segment
    int bit = Glissando::SEGMENT_HEADER_BITS;
    for (uint8_t byte : body)
        for (int shift = 7; shift >= 0; --shift)
            payload[bit++] = (byte >> shift) & 1;
    return payload;
}

bool decodeChorusBeacon(const Glissando::Payload& payload, std::string& callsign,
                        int& gear, Glissando::Scale& scale, int& degree)
{
    if (payload[0] != 0 || payload[1] != 1 || payload[2] != 1 ||
        payload[3] != 1 || payload[4] != 1) return false;
    uint8_t body[Glissando::SEGMENT_DATA_BYTES]{};
    int bit = Glissando::SEGMENT_HEADER_BITS;
    for (uint8_t& byte : body)
        for (int shift = 7; shift >= 0; --shift)
            byte |= (payload[bit++] & 1) << shift;
    if (body[0] != 0xC7 || body[8] != 0xA5 ||
        !unpackChorusChannel(body[7], gear, scale, degree)) return false;
    callsign = FrameCodec::unpackCallsign(body + 1);
    return !callsign.empty();
}

bool unpackChorusChannel(uint8_t packed, int& gear, Glissando::Scale& scale, int& degree)
{
    if ((packed & 1) == 0) return false; // version 1
    int gearCode = (packed >> 5) & 7;
    int scaleCode = (packed >> 3) & 3;
    int degreeIndex = (packed >> 1) & 3;
    if (gearCode >= Glissando::MAX_GEAR || degreeIndex >= Glissando::SCALE_DEGREE_COUNT) return false;
    gear = gearCode + Glissando::MIN_GEAR;
    scale = (Glissando::Scale)scaleCode;
    degree = Glissando::SCALE_DEGREES[degreeIndex];
    return true;
}

} // namespace

TextMessagingModem::TextMessagingModem()
    : signallingTx_(nullptr)
    , textTx_(nullptr)
    , open_(false)
    , lastSyncMs_(0)
    , reassembler_(SIGNALLING_FRAME_BYTES, TEXT_FRAME_BYTES)
    , glissandoRx_(new Glissando::StreamingReceiver())
    , glissandoOn_(false)
{
    glissandoRx_->setDecodeCallback([this](const Glissando::StreamDecode& decode) {
        onGlissandoDecode(decode);
    });
}

TextMessagingModem::~TextMessagingModem()
{
    close();
    glissandoRx_->stop();
}

bool TextMessagingModem::open()
{
    std::lock_guard<std::mutex> lock(rxMutex_);

    if (open_) return true;

    signallingRx_.modem = freedv_open(FREEDV_MODE_DATAC13);
    signallingRx_.name = "DATAC13";
    textRx_.modem = freedv_open(FREEDV_MODE_DATAC4);
    textRx_.name = "DATAC4";
    signallingTx_ = freedv_open(FREEDV_MODE_DATAC13);
    textTx_ = freedv_open(FREEDV_MODE_DATAC4);

    if (signallingRx_.modem == nullptr || textRx_.modem == nullptr ||
        signallingTx_ == nullptr || textTx_ == nullptr)
    {
        log_warn("Could not open the text messaging data modems");
        closeLocked();
        return false;
    }

    for (Demodulator* demodulator : {&signallingRx_, &textRx_})
    {
        // One frame per burst: each frame is preceded by its own preamble, so
        // the modem should look for a new burst after every one.
        freedv_set_frames_per_burst(demodulator->modem, 1);
        freedv_set_verbose(demodulator->modem, 0);

        assert(freedv_get_modem_sample_rate(demodulator->modem) == MODEM_SAMPLE_RATE);

        int bytesPerModemFrame = freedv_get_bits_per_modem_frame(demodulator->modem) / 8;
        demodulator->payloadBytes = bytesPerModemFrame - 2; // the modem's own CRC
        demodulator->bytes.resize(bytesPerModemFrame);
        demodulator->buffer.clear();
        demodulator->buffer.reserve(freedv_get_n_max_modem_samples(demodulator->modem) * 2);
    }

    // The payload sizes the frame codec was written against have to match what
    // this build of codec2 actually gives us, or frames would silently be
    // truncated on the air.
    if (signallingRx_.payloadBytes != SIGNALLING_FRAME_BYTES ||
        textRx_.payloadBytes != TEXT_FRAME_BYTES)
    {
        log_warn("Unexpected codec2 data mode payload sizes (%d/%d, expected %d/%d)",
                 signallingRx_.payloadBytes, textRx_.payloadBytes, SIGNALLING_FRAME_BYTES,
                 TEXT_FRAME_BYTES);
        closeLocked();
        return false;
    }

    // The protocol reserves the channel for the fragments it has not heard
    // yet using TEXT_FRAGMENT_AIR_MILLISECONDS; a fragment that ran longer
    // than that on the air would leave a gap it could key into.
    int textFrameSamples = freedv_get_n_tx_preamble_modem_samples(textTx_) +
                           freedv_get_n_tx_modem_samples(textTx_) +
                           freedv_get_n_tx_postamble_modem_samples(textTx_) +
                           MODEM_SAMPLE_RATE * INTER_BURST_GAP_MS / 1000;
    int textFrameMs = textFrameSamples * 1000 / MODEM_SAMPLE_RATE;
    if (textFrameMs > TEXT_FRAGMENT_AIR_MILLISECONDS)
    {
        log_warn("A text fragment takes %d ms on the air, more than the %d ms the protocol "
                 "reserves for one", textFrameMs, TEXT_FRAGMENT_AIR_MILLISECONDS);
        closeLocked();
        return false;
    }

    freedv_set_verbose(signallingTx_, 0);
    freedv_set_verbose(textTx_, 0);

    {
        std::lock_guard<std::mutex> glissandoLock(glissandoMutex_);
        configureGlissandoReceiverLocked();
        reassembler_.reset();
    }
    glissandoRx_->start();

    open_ = true;
    return true;
}

void TextMessagingModem::close()
{
    std::lock_guard<std::mutex> lock(rxMutex_);
    closeLocked();
}

void TextMessagingModem::closeLocked()
{
    open_ = false;
    glissandoRx_->stop();

    for (struct freedv** modem : {&signallingRx_.modem, &textRx_.modem, &signallingTx_, &textTx_})
    {
        if (*modem != nullptr)
        {
            freedv_close(*modem);
            *modem = nullptr;
        }
    }

    signallingRx_.buffer.clear();
    textRx_.buffer.clear();
    lastSyncMs_.store(0, std::memory_order_release);
}

bool TextMessagingModem::isOpen() const
{
    return open_;
}

void TextMessagingModem::setFrameCallback(FrameCallback callback)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    frameCallback_ = std::move(callback);
}

bool TextMessagingModem::modulateFrame(struct freedv* modem, const std::vector<uint8_t>& frame,
                                       std::vector<short>& samplesOut)
{
    int bytesPerModemFrame = freedv_get_bits_per_modem_frame(modem) / 8;
    int payloadBytes = bytesPerModemFrame - 2;
    if ((int)frame.size() != payloadBytes) return false;

    std::vector<uint8_t> bytes(bytesPerModemFrame, 0);
    std::memcpy(bytes.data(), frame.data(), frame.size());

    // The raw data modes require the CRC in the last two bytes.
    uint16_t crc = freedv_gen_crc16(bytes.data(), payloadBytes);
    bytes[bytesPerModemFrame - 2] = (uint8_t)(crc >> 8);
    bytes[bytesPerModemFrame - 1] = (uint8_t)(crc & 0xFF);

    int preambleSamples = freedv_get_n_tx_preamble_modem_samples(modem);
    int frameSamples = freedv_get_n_tx_modem_samples(modem);
    int postambleSamples = freedv_get_n_tx_postamble_modem_samples(modem);
    int gapSamples = MODEM_SAMPLE_RATE * INTER_BURST_GAP_MS / 1000;

    // The preamble and postamble calls report how many samples they actually
    // produced, which can be fewer than the maximum the getters report, so
    // each section is trimmed to its real length before the next one starts.
    size_t offset = samplesOut.size();
    samplesOut.resize(offset + preambleSamples);
    samplesOut.resize(offset + freedv_rawdatapreambletx(modem, &samplesOut[offset]));

    offset = samplesOut.size();
    samplesOut.resize(offset + frameSamples);
    freedv_rawdatatx(modem, &samplesOut[offset], bytes.data());

    offset = samplesOut.size();
    samplesOut.resize(offset + postambleSamples);
    samplesOut.resize(offset + freedv_rawdatapostambletx(modem, &samplesOut[offset]));

    // Silence between bursts, so the far end can drop sync and start looking
    // for the next preamble.
    samplesOut.resize(samplesOut.size() + gapSamples, 0);

    return true;
}

bool TextMessagingModem::modulate(const std::vector<OutgoingBurst>& bursts,
                                  std::vector<short>& samplesOut,
                                  std::vector<size_t>* frameEndsOut)
{
    if (!open_) return false;

    samplesOut.clear();
    if (frameEndsOut != nullptr) frameEndsOut->clear();

    if (glissandoOn_.load(std::memory_order_acquire))
    {
        Glissando::ModemSettings settings;
        bool announceChorus = false;
        std::string announceCallsign;
        bool chordPreambleEnabled = false;
        bool chordTailEnabled = false;
        double chordPreambleSeconds = 1.0;
        double chordTailSeconds = 1.0;
        {
            std::lock_guard<std::mutex> lock(glissandoMutex_);
            settings.gear = transmitGearLocked();
            settings.scale = glissando_.scale;
            settings.scaleDegree = transmitScaleDegreeLocked();
            settings.tuningOffsetHz = glissando_.tuningOffsetHz;

            chordPreambleEnabled = glissando_.chordPreambleEnabled;
            chordTailEnabled = glissando_.chordTailEnabled;
            chordPreambleSeconds = glissando_.chordPreambleSeconds;
            chordTailSeconds = glissando_.chordTailSeconds;
            settings.chordPreambleSeconds = chordPreambleSeconds;
            settings.chordTailSeconds = chordTailSeconds;

            uint64_t now = steadyMs();
            bool changed = settings.gear != lastAnnouncedGear_ || settings.scale != lastAnnouncedScale_ ||
                           settings.scaleDegree != lastAnnouncedDegree_ ||
                           glissando_.callsign != lastAnnouncedCallsign_;
            if (glissando_.chorus && !glissando_.callsign.empty() &&
                (changed || now - lastChorusAnnouncementMs_ >= CHORUS_ANNOUNCE_INTERVAL_MS))
            {
                uint8_t channel = packChorusChannel(settings.gear, settings.scale, settings.scaleDegree);
                if (channel != 0)
                {
                    announceChorus = true;
                    announceCallsign = glissando_.callsign;
                    lastChorusAnnouncementMs_ = now;
                    lastAnnouncedGear_ = settings.gear;
                    lastAnnouncedScale_ = settings.scale;
                    lastAnnouncedDegree_ = settings.scaleDegree;
                    lastAnnouncedCallsign_ = glissando_.callsign;
                }
            }

        }
        const Glissando::GearInfo& gear = Glissando::gearInfo(settings.gear);

        if (announceChorus)
        {
            Glissando::ModemSettings control;
            control.gear = 4;
            control.scale = Glissando::Scale::Pentatonic;
            control.scaleDegree = 0;
            control.tuningOffsetHz = settings.tuningOffsetHz;
            std::vector<float> audio = Glissando::modulate(
                {makeChorusBeacon(announceCallsign, settings.gear, settings.scale, settings.scaleDegree)}, control);
            for (float sample : audio) samplesOut.push_back((short)std::lround(sample * GLISSANDO_PEAK));
            if (frameEndsOut != nullptr) frameEndsOut->push_back(samplesOut.size());
            samplesOut.resize(samplesOut.size() + (size_t)(MODEM_SAMPLE_RATE * INTER_BURST_GAP_MS / 1000), 0);
        }

        std::vector<Glissando::LinkBurst> linkBursts;
        for (const OutgoingBurst& burst : bursts)
        {
            linkBursts.push_back({burst.mode == BurstMode::Text, burst.frame});
        }
        std::vector<Glissando::Payload> payloads = Glissando::segmentBursts(linkBursts, gear.voices);

        for (size_t first = 0; first + gear.voices <= payloads.size(); first += gear.voices)
        {
            Glissando::ModemSettings frameSettings = settings;
            frameSettings.chordPreambleEnabled = chordPreambleEnabled && first == 0;
            frameSettings.chordTailEnabled = chordTailEnabled && first + gear.voices >= payloads.size();
            std::vector<Glissando::Payload> voices(payloads.begin() + first,
                                                   payloads.begin() + first + gear.voices);
            std::vector<float> audio = Glissando::modulate(voices, frameSettings);
            {
                std::lock_guard<std::mutex> lock(glissandoMutex_);
                for (int v = 0; v < gear.voices; v++)
                {
                    GlissandoSent sent;
                    sent.gear = settings.gear;
                    sent.scale = settings.scale;
                    sent.voice = v;
                    sent.notesHz = Glissando::scaleDegreeNotes(
                      settings.scale, v, settings.scaleDegree
                    );
                    for (double& hz : sent.notesHz) hz += settings.tuningOffsetHz;
                    sent.melody = Glissando::payloadMelody(voices[v]);
                    glissandoSent_.push_back(sent);
                }
                if (glissandoSent_.size() > GLISSANDO_SENT_LIMIT)
                {
                    glissandoSent_.erase(glissandoSent_.begin(),
                                         glissandoSent_.end() - GLISSANDO_SENT_LIMIT);
                }
            }
            for (float sample : audio)
            {
                samplesOut.push_back((short)std::lround(sample * GLISSANDO_PEAK));
            }
            if (frameEndsOut != nullptr) frameEndsOut->push_back(samplesOut.size());
        }

        if (rxLogEnabled())
        {
            log_info("TX: %d Glissando frame(s) at %s, %.1f s", (int)(payloads.size() / gear.voices),
                     gear.tempo, samplesOut.size() / (double)MODEM_SAMPLE_RATE);
        }
        return !samplesOut.empty();
    }

    for (const OutgoingBurst& burst : bursts)
    {
        struct freedv* modem = burst.mode == BurstMode::Signalling ? signallingTx_ : textTx_;
        if (!modulateFrame(modem, burst.frame, samplesOut))
        {
            samplesOut.clear();
            if (frameEndsOut != nullptr) frameEndsOut->clear();
            return false;
        }
        if (frameEndsOut != nullptr) frameEndsOut->push_back(samplesOut.size());
    }

    return !samplesOut.empty();
}

void TextMessagingModem::demodulateOne(Demodulator& demodulator, const short* samples,
                                       int numSamples)
{
    std::vector<short>& buffer = demodulator.buffer;
    buffer.insert(buffer.end(), samples, samples + numSamples);

    if ((int)buffer.size() > MAX_BUFFERED_SAMPLES)
    {
        buffer.erase(buffer.begin(), buffer.begin() + (buffer.size() - MAX_BUFFERED_SAMPLES));
    }

    int nin = freedv_nin(demodulator.modem);
    while ((int)buffer.size() >= nin)
    {
        int bytesOut = freedv_rawdatarx(demodulator.modem, demodulator.bytes.data(), buffer.data());
        buffer.erase(buffer.begin(), buffer.begin() + nin);
        nin = freedv_nin(demodulator.modem);

        // FREEDV_RX_SYNC covers trial sync as well as full sync, so this holds
        // from the moment a preamble is correlated until the packet is in:
        // the whole time a burst is on the channel, not just the instant a
        // frame decodes.
        int status = freedv_get_rx_status(demodulator.modem);
        if (status & FREEDV_RX_SYNC)
        {
            lastSyncMs_.store(steadyMs(), std::memory_order_release);
        }

        // codec2 reports a packet that was demodulated but failed its CRC and
        // then drops it; this is the only trace a faded burst leaves.
        if (bytesOut <= 0)
        {
            if ((status & FREEDV_RX_BIT_ERRORS) && rxLogEnabled())
            {
                log_info("RX: %s burst failed its CRC (%.1f dB)", demodulator.name,
                         (double)modemSnr(demodulator.modem));
            }
            continue;
        }

        float snr = modemSnr(demodulator.modem);

        Frame frame;
        if (!FrameCodec::decode(demodulator.bytes.data(), bytesOut, frame))
        {
            if (rxLogEnabled())
            {
                log_info("RX: %s frame passed its CRC but is not a chat frame (%.1f dB)",
                         demodulator.name, (double)snr);
            }
            continue;
        }

        if (rxLogEnabled())
        {
            log_info("RX: %s %s from %s id %u fragment %d/%d (+%d to come) to %06X, %.1f dB",
                     demodulator.name, frameTypeName(frame.type),
                     frame.originCallsign.c_str(), (unsigned)frame.airId,
                     frame.fragmentIndex + 1, frame.fragmentCount, frame.burstsFollowing,
                     (unsigned)frame.destinationCrc, (double)snr);

            // Which fragments the far end holds is the point of the frame.
            if (frame.type == FrameType::MessagePartialAck && !frame.payload.empty())
            {
                log_info("RX: partial ack reports fragments %02X received",
                         (unsigned)frame.payload[0]);
            }
        }

        FrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = frameCallback_;
        }

        if (callback) callback(frame, snr);
    }
}

void TextMessagingModem::demodulate(const short* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0) return;

    if (glissandoOn_.load(std::memory_order_acquire))
    {
        // The receiver copies and returns; the search runs on its own thread.
        glissandoRx_->push(samples, numSamples);
        return;
    }

    std::lock_guard<std::mutex> lock(rxMutex_);
    if (!open_) return;

    demodulateOne(signallingRx_, samples, numSamples);
    demodulateOne(textRx_, samples, numSamples);
}

void TextMessagingModem::resetReceivers()
{
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        reassembler_.reset();
    }
    glissandoRx_->reset();

    std::lock_guard<std::mutex> lock(rxMutex_);
    if (!open_) return;

    for (Demodulator* demodulator : {&signallingRx_, &textRx_})
    {
        // FREEDV_SYNC_UNSYNC returns the state machine to search and clears
        // the modem's own sample history; the samples we were holding for it
        // are just as stale.
        freedv_set_sync(demodulator->modem, FREEDV_SYNC_UNSYNC);
        demodulator->buffer.clear();
    }

    lastSyncMs_.store(0, std::memory_order_release);
}

bool TextMessagingModem::isReceiving() const
{
    if (!open_) return false;

    if (glissandoOn_.load(std::memory_order_acquire))
    {
        // A burst whose last frame has been heard is over: the far end is
        // waiting for an answer, and holding the channel for another frame
        // and a half kept it waiting 68 s at Adagio before it could have one.
        // A message with more bursts to come holds the channel in the
        // protocol, from what its frames say.
        if (glissandoRx_->lastFrameEnd() == completedFrameEnd_.load(std::memory_order_acquire)) return false;

        // A frame of a burst decodes one frame length after the one before
        // it, plus the search; hold the channel across that gap.
        int gear = 0;
        {
            std::lock_guard<std::mutex> lock(glissandoMutex_);
            gear = glissandoStatus_.heardGear != 0 ? glissandoStatus_.heardGear : transmitGearLocked();
        }
        long long hold = (long long)(Glissando::gearInfo(gear).frameSamples() * 1.5);
        return glissandoRx_->isBusy(hold);
    }

    uint64_t last = lastSyncMs_.load(std::memory_order_acquire);
    return last != 0 && steadyMs() - last < (uint64_t)CHANNEL_BUSY_HOLD_MILLISECONDS;
}

void TextMessagingModem::setGlissando(const GlissandoConfig& config)
{
    GlissandoConfig normalized = config;
    normalized.scaleDegree = Glissando::normalizeScaleDegree(config.scaleDegree);
    normalized.callsign = FrameCodec::normalizeCallsign(config.callsign);
    normalized.chordPreambleSeconds = std::isfinite(config.chordPreambleSeconds)
        ? std::min(10.0, std::max(0.1, config.chordPreambleSeconds)) : 1.0;
    normalized.chordTailSeconds = std::isfinite(config.chordTailSeconds)
        ? std::min(10.0, std::max(0.1, config.chordTailSeconds)) : 1.0;
    bool wasOn = false;
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        wasOn = glissando_.enabled;
        if (normalized.chorus && !glissando_.chorus) lastAnnouncedDegree_ = -1;
        // A frame half heard in the old scale or tuning will not be
        // finished in the new one. Listening for every scale, our own scale
        // only changes what we send.
        bool rescaled = normalized.scale != glissando_.scale &&
                        !(normalized.listenAllScales && glissando_.listenAllScales);
        bool retuned = rescaled || normalized.listenAllScales != glissando_.listenAllScales ||
                       normalized.tuningOffsetHz != glissando_.tuningOffsetHz ||
                       normalized.chorus != glissando_.chorus;
        glissando_ = normalized;
        glissandoStatus_.transmitGear = transmitGearLocked();
        configureGlissandoReceiverLocked();
        if (retuned || wasOn != normalized.enabled) reassembler_.reset();
    }
    if (wasOn != normalized.enabled) glissandoRx_->reset();
    glissandoOn_.store(normalized.enabled, std::memory_order_release);
}

TextMessagingModem::GlissandoConfig TextMessagingModem::glissandoConfig() const
{
    std::lock_guard<std::mutex> lock(glissandoMutex_);
    return glissando_;
}

TextMessagingModem::GlissandoStatus TextMessagingModem::glissandoStatus() const
{
    std::lock_guard<std::mutex> lock(glissandoMutex_);
    GlissandoStatus status = glissandoStatus_;
    status.transmitGear = transmitGearLocked();
    status.transmitScaleDegree = transmitScaleDegreeLocked();
    uint64_t now = steadyMs();
    status.chorusParticipants = 0;
    for (const auto& entry : chorusStations_)
        if (now - entry.second.heardAtMs <= CHORUS_STATION_TIMEOUT_MS) ++status.chorusParticipants;
    return status;
}

AirTiming TextMessagingModem::airTiming() const
{
    if (!glissandoOn_.load(std::memory_order_acquire)) return AirTiming();

    int gear = 0;
    double slowestFrameSeconds = 0.0;
    bool preambleEnabled = false;
    double preambleSeconds = 0.0;
    double bookendSeconds = 0.0;
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        gear = transmitGearLocked();
        preambleEnabled = glissando_.chordPreambleEnabled;
        preambleSeconds = glissando_.chordPreambleSeconds;
        bookendSeconds = (glissando_.chordPreambleEnabled ? glissando_.chordPreambleSeconds : 0.0) +
                         (glissando_.chordTailEnabled ? glissando_.chordTailSeconds : 0.0);
        std::vector<int> listening;
        if (glissando_.listenAllGears)
            for (int g = Glissando::MIN_GEAR; g <= Glissando::MAX_GEAR; ++g) listening.push_back(g);
        else listening = {glissando_.gear, gear};
        for (int g : listening)
            slowestFrameSeconds = std::max(slowestFrameSeconds, Glissando::gearInfo(g).frameSeconds());
    }
    const Glissando::GearInfo& info = Glissando::gearInfo(gear);
    auto decodeLatency = [](double frameSeconds) { return frameSeconds / 4.0 + GLISSANDO_SEARCH_SECONDS; };
    return AirTiming::forFrameSeconds(info.frameSeconds(), Glissando::SEGMENT_DATA_BYTES * info.voices,
                                      decodeLatency(info.frameSeconds()), slowestFrameSeconds,
                                      decodeLatency(slowestFrameSeconds),
                                      preambleEnabled ? preambleSeconds : 0.0, bookendSeconds);
}

double TextMessagingModem::glissandoMessageSeconds(size_t textBytes) const
{
    if (!glissandoOn_.load(std::memory_order_acquire) || textBytes == 0) return 0.0;

    int gear = 0;
    bool chorus = false;
    double bookendSeconds = 0.0;
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        gear = transmitGearLocked();
        chorus = glissando_.chorus;
        bookendSeconds = (glissando_.chordPreambleEnabled ? glissando_.chordPreambleSeconds : 0.0) +
                         (glissando_.chordTailEnabled ? glissando_.chordTailSeconds : 0.0);
    }
    const Glissando::GearInfo& info = Glissando::gearInfo(gear);

    textBytes = std::min(textBytes, (size_t)MAX_MESSAGE_TEXT_BYTES);
    std::vector<Glissando::LinkBurst> bursts;
    for (size_t offset = 0; offset < textBytes; offset += TEXT_BYTES_PER_FRAGMENT)
    {
        size_t chunk = std::min(textBytes - offset, (size_t)TEXT_BYTES_PER_FRAGMENT);
        bursts.push_back({true, std::vector<uint8_t>(TEXT_HEADER_BYTES + chunk, 0xFF)});
    }

    size_t frames = Glissando::framesForBursts(bursts, info.voices);
    double seconds = frames * info.frameSeconds() + (frames != 0 ? bookendSeconds : 0.0);
    if (chorus) seconds += Glissando::gearInfo(4).frameSeconds() + INTER_BURST_GAP_MS / 1000.0;
    return seconds;
}

int TextMessagingModem::transmitGearLocked() const
{
    int gear = glissando_.gear;
    if (glissando_.autoGear && glissandoStatus_.haveReport && glissandoStatus_.advisedGear != 0 &&
        steadyMs() - glissandoStatus_.heardAtMs < (uint64_t)GLISSANDO_REPORT_LIFETIME_MS)
    {
        gear = glissandoStatus_.advisedGear;
    }
    // The +3/+4 step positions put the duet's high voice above a 4 kHz
    // channel. Keep those choices on a single-voice gear even in auto mode.
    if (transmitScaleDegreeLocked() >= 3) gear = std::min(gear, 4);
    return gear;
}

int TextMessagingModem::transmitScaleDegreeLocked() const
{
    if (!glissando_.chorus || glissando_.callsign.empty()) return glissando_.scaleDegree;

    std::vector<std::string> callsigns{glissando_.callsign};
    uint64_t now = steadyMs();
    for (const auto& entry : chorusStations_)
    {
        if (entry.first != glissando_.callsign && entry.second.scale == glissando_.scale &&
            now - entry.second.heardAtMs <= CHORUS_STATION_TIMEOUT_MS)
            callsigns.push_back(entry.first);
    }
    std::sort(callsigns.begin(), callsigns.end());
    auto own = std::find(callsigns.begin(), callsigns.end(), glissando_.callsign);
    size_t slot = (size_t)std::distance(callsigns.begin(), own) % Glissando::SCALE_DEGREE_COUNT;
    return Glissando::SCALE_DEGREES[slot];
}

void TextMessagingModem::updateChorusReceiverLocked()
{
    const uint64_t now = steadyMs();
    for (auto it = chorusStations_.begin(); it != chorusStations_.end();)
    {
        if (it->first == glissando_.callsign || now - it->second.heardAtMs > CHORUS_STATION_TIMEOUT_MS)
            it = chorusStations_.erase(it);
        else
            ++it;
    }

    std::vector<int> gears;
    std::vector<Glissando::ReceiveHypothesis> hypotheses;
    if (glissando_.chorus)
    {
        // Discovery must see any gear. Once participants announce themselves,
        // StreamingReceiver uses only their tuples outside scheduled scans.
        for (int gear = Glissando::MIN_GEAR; gear <= Glissando::MAX_GEAR; ++gear) gears.push_back(gear);
        for (const auto& entry : chorusStations_)
            hypotheses.push_back({entry.second.gear, entry.second.scale, entry.second.scaleDegree});
        // All Chorus stations listen for profile announcements on this shared control lane.
        hypotheses.push_back({4, Glissando::Scale::Pentatonic, 0});
    }
    else
    {
        if (glissando_.listenAllGears)
        {
            for (int gear = Glissando::MIN_GEAR; gear <= Glissando::MAX_GEAR; ++gear) gears.push_back(gear);
        }
        else
        {
            gears.push_back(glissando_.gear);
            int current = transmitGearLocked();
            if (current != glissando_.gear) gears.push_back(current);
        }
    }
    glissandoRx_->configure(gears, glissando_.scale, glissando_.tuningOffsetHz,
                            glissando_.listenAllScales, glissando_.chorus, hypotheses);
}

void TextMessagingModem::configureGlissandoReceiverLocked()
{
    updateChorusReceiverLocked();
}

void TextMessagingModem::onGlissandoDecode(const Glissando::StreamDecode& decode)
{
    const Glissando::Decode& d = decode.decode;
    Glissando::LinkBurst burst;
    bool complete = false;
    bool chorusBeacon = false;
    int announcedGear = 0, announcedDegree = 0;
    Glissando::Scale announcedScale = Glissando::Scale::Pentatonic;
    std::string announcedCallsign;
    float snr = (float)d.report.snrDb;
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        chorusBeacon = glissando_.chorus &&
            decodeChorusBeacon(d.payload, announcedCallsign, announcedGear, announcedScale, announcedDegree);
        // The fixed G4 lane carries control data, not a peer's chosen data
        // gear. Feeding its report to Auto Shift makes a clean loopback choose
        // G5 repeatedly and can mask the station's normal channel conditions.
        if (!chorusBeacon)
        {
            glissandoStatus_.haveReport = true;
            glissandoStatus_.report = d.report;
            glissandoStatus_.heardGear = decode.gear;
            glissandoStatus_.heardScale = d.scale;
            glissandoStatus_.heardScaleDegree = d.scaleDegree;
            glissandoStatus_.heardAtMs = steadyMs();
            glissandoStatus_.advisedGear = Glissando::recommendGear(d.report.snrDb, d.report.dopplerHz);
        }

        // The same waveform can be decoded as more than one tempo (Presto
        // and the duet's low voice), or by two overlapping searches.
        long long duplicate = Glissando::gearInfo(decode.gear).samplesPerSymbol();
        GlissandoHeard heard;
        complete = reassembler_.add(d.payload, d.startSample, duplicate, burst, &heard.segment);

        if (!heard.segment.duplicate)
        {
            long long age = glissandoRx_->samplesReceived() - d.startSample;
            uint64_t now = steadyMs();
            uint64_t ageMs = (uint64_t)std::max(0LL, age * 1000 / Glissando::SAMPLE_RATE_HZ);
            heard.startMs = now > ageMs ? now - ageMs : 0;
            heard.gear = decode.gear;
            heard.scale = d.scale;
            heard.voice = d.voice;
            heard.notesHz = Glissando::scaleDegreeNotes(d.scale, d.voice, d.scaleDegree);
            for (double& hz : heard.notesHz) hz += glissando_.tuningOffsetHz + d.frequencyOffsetHz;
            heard.melody = Glissando::payloadMelody(d.payload);
            heard.snrDb = d.report.snrDb;
            if (glissandoHeard_.size() >= GLISSANDO_HEARD_LIMIT) glissandoHeard_.erase(glissandoHeard_.begin());
            glissandoHeard_.push_back(std::move(heard));
        }
        if (chorusBeacon)
        {
            if (announcedCallsign != glissando_.callsign)
            {
                chorusStations_[announcedCallsign] =
                    {announcedGear, announcedScale, announcedDegree, steadyMs()};
                updateChorusReceiverLocked();
            }
        }
    }
    if (complete) completedFrameEnd_.store(glissandoRx_->lastFrameEnd(), std::memory_order_release);
    lastSyncMs_.store(steadyMs(), std::memory_order_release);
    if (chorusBeacon)
    {
        if (rxLogEnabled()) log_info("RX: Chorus participant %s on G%d %s degree %d",
            announcedCallsign.c_str(), announcedGear, Glissando::scaleName(announcedScale),
            Glissando::scaleDegreeNumber(announcedDegree));
        return;
    }
    if (rxLogEnabled())
    {
        log_info("RX: Glissando %s %s scale degree %d voice %d, %.1f dB, %.2f Hz Doppler, offset %+.1f Hz%s",
                 Glissando::gearInfo(decode.gear).tempo, Glissando::scaleName(d.scale),
                 Glissando::scaleDegreeNumber(d.scaleDegree), d.voice, d.report.snrDb,
                 d.report.dopplerHz, d.frequencyOffsetHz, complete ? ", burst complete" : "");
    }
    if (!complete) return;

    Frame frame;
    if (!FrameCodec::decode(burst.bytes.data(), (int)burst.bytes.size(), frame))
    {
        if (rxLogEnabled()) log_info("RX: Glissando burst is not a chat frame");
        return;
    }

    FrameCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = frameCallback_;
    }
    if (callback) callback(frame, snr);
}

std::vector<TextMessagingModem::GlissandoHeard> TextMessagingModem::takeGlissandoHeard()
{
    std::lock_guard<std::mutex> lock(glissandoMutex_);
    std::vector<GlissandoHeard> heard;
    heard.swap(glissandoHeard_);
    return heard;
}

std::vector<TextMessagingModem::GlissandoSent> TextMessagingModem::takeGlissandoSent()
{
    std::lock_guard<std::mutex> lock(glissandoMutex_);
    std::vector<GlissandoSent> sent;
    sent.swap(glissandoSent_);
    return sent;
}

TextMessagingModem& textMessagingModem()
{
    static TextMessagingModem modem;
    return modem;
}
