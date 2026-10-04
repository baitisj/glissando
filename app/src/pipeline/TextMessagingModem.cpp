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

// Allowance for an opening chord to be heard once it has ended: the chord
// listener's next hop (0.1 s), the audio path and the protocol's tick, with
// room to spare.
constexpr double CHORD_HEARD_SECONDS = 1.0;

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

} // namespace

TextMessagingModem::TextMessagingModem()
    : signallingTx_(nullptr)
    , textTx_(nullptr)
    , open_(false)
    , lastSyncMs_(0)
    , reassembler_(SIGNALLING_FRAME_BYTES, TEXT_FRAME_BYTES)
    , stationGears_(GLISSANDO_REPORT_LIFETIME_MS)
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
        bool chords = false;
        bool cwTail = false;
        GlissandoConfig::Tail tail = GlissandoConfig::Tail::Off;
        std::string cwText;
        int cwWpm = Glissando::CW_DEFAULT_WPM;
        {
            std::lock_guard<std::mutex> lock(glissandoMutex_);
            // Replies too go in our own tempo, the one chosen for this
            // station's power and path, not the asker's; see AnswerTempo.h.
            // A message the operator moved to a tempo of its own goes in
            // that one; it keys alone, so the keying has only the one.
            settings.gear = transmitGearLocked();
            for (const OutgoingBurst& burst : bursts)
            {
                if (burst.gear >= Glissando::MIN_GEAR && burst.gear <= Glissando::MAX_GEAR)
                {
                    settings.gear = burst.gear;
                    break;
                }
            }
            settings.scale = glissando_.scale;
            settings.tuningOffsetHz = glissando_.tuningOffsetHz;
            chords = glissando_.chords;
            cwTail = cwTailDueLocked(steadyMs());
            tail = glissando_.tail;
            cwText = glissando_.cwText;
            cwWpm = glissando_.cwWpm;
        }
        const Glissando::GearInfo& gear = Glissando::gearInfo(settings.gear);

        std::vector<Glissando::LinkBurst> linkBursts;
        for (const OutgoingBurst& burst : bursts)
        {
            linkBursts.push_back({burst.mode == BurstMode::Text, burst.frame});
        }
        {
            std::lock_guard<std::mutex> lock(glissandoMutex_);
            noteKeyingLocked(bursts, steadyMs());
        }
        std::vector<Glissando::Payload> payloads = Glissando::segmentBursts(linkBursts, gear.voices);

        // The keying opens with E4 and D5 for 0.6 s at every tempo and in
        // every scale, which listeners hear as the channel being taken (see
        // GlissandoChord.h), and closes with every note of the scale at once
        // (Tyler Carr's idea, PR #22) or the CW tail (GlissandoCw.h). None
        // of them carries anything the receiver uses: the frames are found
        // by their motifs, as without them.
        std::vector<float> opening;
        std::vector<float> closing;
        std::vector<int> tailMelody;
        std::vector<int> tailHarmony;
        const double cwUnitSeconds = 1.2 / std::min(std::max(cwWpm, Glissando::CW_MIN_WPM), Glissando::CW_MAX_WPM);
        if (chords && !payloads.empty()) opening = Glissando::openingChord(settings);
        if (!payloads.empty() && cwTail)
        {
            const bool straight = tail == GlissandoConfig::Tail::CwStraight;
            closing = Glissando::cwTail(cwText, cwWpm, settings,
                                        straight ? Glissando::CwStyle::Straight : Glissando::CwStyle::Glorified);
            // For the visi-scope: a straight tail is drawn on the scale's
            // notes nearest E4 and D5, which every scale has.
            auto nearest = [&settings](double hz) {
                std::array<double, Glissando::NOTES> notes = Glissando::scaleNotes(settings.scale, 0);
                int best = 0;
                for (int i = 1; i < Glissando::NOTES; i++)
                {
                    if (std::fabs(notes[(size_t)i] - hz) < std::fabs(notes[(size_t)best] - hz)) best = i;
                }
                return best;
            };
            for (const Glissando::CwElement& element : Glissando::cwTune(cwText, settings.scale))
            {
                int low = element.note;
                int high = -1;
                if (straight && element.note >= 0)
                {
                    low = nearest(Glissando::CW_STRAIGHT_LOW_HZ);
                    high = nearest(Glissando::CW_STRAIGHT_HIGH_HZ);
                }
                tailMelody.insert(tailMelody.end(), (size_t)element.units, low);
                if (straight) tailHarmony.insert(tailHarmony.end(), (size_t)element.units, high);
            }
            std::lock_guard<std::mutex> lock(glissandoMutex_);
            lastCwTailMs_ = steadyMs();
        }
        else if (!payloads.empty() && tail != GlissandoConfig::Tail::Off)
        {
            closing = Glissando::chord(settings);
        }
        const double openingSeconds = opening.size() / (double)Glissando::SAMPLE_RATE_HZ;
        const double closingSeconds = closing.size() / (double)Glissando::SAMPLE_RATE_HZ;
        for (float sample : opening) samplesOut.push_back((short)std::lround(sample * GLISSANDO_PEAK));

        for (size_t first = 0; first + gear.voices <= payloads.size(); first += gear.voices)
        {
            std::vector<Glissando::Payload> voices(payloads.begin() + first,
                                                   payloads.begin() + first + gear.voices);
            std::vector<float> audio = Glissando::modulate(voices, settings);
            {
                std::lock_guard<std::mutex> lock(glissandoMutex_);
                for (int v = 0; v < gear.voices; v++)
                {
                    GlissandoSent sent;
                    sent.gear = settings.gear;
                    sent.scale = settings.scale;
                    sent.voice = v;
                    sent.notesHz = Glissando::scaleNotes(settings.scale, v);
                    for (double& hz : sent.notesHz) hz += settings.tuningOffsetHz;
                    sent.melody = Glissando::payloadMelody(voices[v]);
                    if (first == 0) sent.leadSeconds = openingSeconds;
                    if (first + gear.voices >= payloads.size() && tailMelody.empty())
                    {
                        sent.tailSeconds = closingSeconds;
                    }
                    if (first + gear.voices >= payloads.size() && v == gear.voices - 1 && !tailMelody.empty())
                    {
                        sent.tailMelody = tailMelody;
                        sent.tailHarmony = tailHarmony;
                        sent.tailUnitSeconds = cwUnitSeconds;
                    }
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

        // The tail belongs to the last frame's keying.
        if (!closing.empty() && samplesOut.size() > opening.size())
        {
            for (float sample : closing) samplesOut.push_back((short)std::lround(sample * GLISSANDO_PEAK));
            if (frameEndsOut != nullptr && !frameEndsOut->empty()) frameEndsOut->back() = samplesOut.size();
        }

        if (rxLogEnabled())
        {
            log_info("TX: %d Glissando frame(s) at %s, %.1f s%s", (int)(payloads.size() / gear.voices),
                     gear.tempo, samplesOut.size() / (double)MODEM_SAMPLE_RATE,
                     tailMelody.empty() ? "" : ", with the CW tail");
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
        std::lock_guard<std::mutex> lock(chordMutex_);
        chordListener_.push(samples, numSamples);

        // Where the listener last heard the notes stop, on the receiver's
        // clock, for isReceiving().
        bool sounding = chordListener_.isSounding();
        if (chordWasSounding_ && !sounding) chordStoppedAt_.store(glissandoRx_->samplesReceived(), std::memory_order_release);
        chordWasSounding_ = sounding;
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
    {
        std::lock_guard<std::mutex> lock(chordMutex_);
        chordListener_.reset();
        chordWasSounding_ = false;
        chordStoppedAt_.store(-1, std::memory_order_release);
    }

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
    return busyReason() != Busy::No;
}

TextMessagingModem::CarrierSense TextMessagingModem::carrierSense() const
{
    CarrierSense sense;
    Busy why = busyReason();
    sense.busy = why != Busy::No;
    if (why == Busy::Chord)
    {
        std::lock_guard<std::mutex> lock(chordMutex_);
        sense.notesHz = chordListener_.soundingNotesHz();
    }
    else if (why == Busy::Melody)
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        sense.notesHz = lastHeardNotesHz_;
    }
    return sense;
}

TextMessagingModem::Busy TextMessagingModem::busyReason() const
{
    if (!open_) return Busy::No;

    if (glissandoOn_.load(std::memory_order_acquire))
    {
        // An opening chord heard, and the notes of its scale still sounding:
        // somebody has the channel, well before any frame of theirs decodes.
        if (chordListener_.isSounding()) return Busy::Chord;

        int gear = 0;
        double tailSeconds = 0.0;
        {
            std::lock_guard<std::mutex> lock(glissandoMutex_);
            gear = glissandoStatus_.heardGear != 0 ? glissandoStatus_.heardGear : transmitGearLocked();
            tailSeconds = farEndTailSecondsLocked(gear);
        }

        // A burst whose last frame has been heard is over: the far end is
        // waiting for an answer, and holding the channel for another frame
        // and a half kept it waiting 68 s at Adagio before it could have one.
        // A message with more bursts to come holds the channel in the
        // protocol, from what its frames say. But the far end may still be
        // singing its closing chord or CW tail, and is not listening until
        // it stops: a reply keyed over the tail loses its opening, and with
        // it the acknowledgement or pong. The chord listener hears the tail
        // stop when it followed the keying; at weaker signals it does not
        // (two stations at -12 dB lost an acknowledgement this way, and the
        // message waited minutes for its retry), and the channel is held for
        // as long as a tail can be.
        long long completed = completedFrameEnd_.load(std::memory_order_acquire);
        if (glissandoRx_->lastFrameEnd() == completed)
        {
            if (chordStoppedAt_.load(std::memory_order_acquire) >= completed) return Busy::No;
            bool tail = glissandoRx_->samplesReceived() < completed + (long long)(tailSeconds * Glissando::SAMPLE_RATE_HZ);
            return tail ? Busy::Melody : Busy::No;
        }

        // A frame of a burst decodes one frame length after the one before
        // it, plus the search; hold the channel across that gap.
        long long hold = (long long)(Glissando::gearInfo(gear).frameSamples() * 1.5);
        return glissandoRx_->isBusy(hold) ? Busy::Melody : Busy::No;
    }

    uint64_t last = lastSyncMs_.load(std::memory_order_acquire);
    bool synced = last != 0 && steadyMs() - last < (uint64_t)CHANNEL_BUSY_HOLD_MILLISECONDS;
    return synced ? Busy::Codec2 : Busy::No;
}

void TextMessagingModem::setGlissando(const GlissandoConfig& config)
{
    bool wasOn = false;
    bool tuningChanged = false;
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        wasOn = glissando_.enabled;
        tuningChanged = config.tuningOffsetHz != glissando_.tuningOffsetHz;
        // A frame half heard in the old scale or tuning will not be
        // finished in the new one. Listening for every scale, our own scale
        // only changes what we send.
        bool rescaled = config.scale != glissando_.scale && !(config.listenAllScales && glissando_.listenAllScales);
        bool retuned = rescaled || config.listenAllScales != glissando_.listenAllScales ||
                       config.tuningOffsetHz != glissando_.tuningOffsetHz;
        glissando_ = config;
        glissandoStatus_.transmitGear = transmitGearLocked();
        configureGlissandoReceiverLocked();
        if (retuned || wasOn != config.enabled) reassembler_.reset();
    }
    if (wasOn != config.enabled) glissandoRx_->reset();
    if (tuningChanged || wasOn != config.enabled)
    {
        std::lock_guard<std::mutex> lock(chordMutex_);
        chordListener_.configure(config.tuningOffsetHz);
    }
    glissandoOn_.store(config.enabled, std::memory_order_release);
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
    return status;
}

AirTiming TextMessagingModem::airTiming() const
{
    if (!glissandoOn_.load(std::memory_order_acquire)) return AirTiming();

    int gear = 0;
    double slowestFrameSeconds = 0.0;
    bool chords = false;
    double closingSeconds = 0.0;
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        gear = transmitGearLocked();
        chords = glissando_.chords;
        closingSeconds = closingSecondsLocked(gear, steadyMs());

        // An answer comes back in whatever tempo its station sends at (see
        // AnswerTempo.h), so the waits cover our own tempo and every tempo
        // heard lately. They used to cover every tempo the receiver listens
        // for, Adagio included, and at Presto an unanswered ping held the
        // queue for 111 s and gave up after 120 s. A station not heard
        // lately that answers slower than that is covered by its opening
        // chord: hearing it freezes the timers until its keying ends.
        std::vector<int> answering = stationGears_.recentTempos(steadyMs());
        answering.push_back(gear);
        for (int g : answering)
        {
            slowestFrameSeconds = std::max(slowestFrameSeconds, Glissando::gearInfo(g).frameSeconds());
        }
    }
    const Glissando::GearInfo& info = Glissando::gearInfo(gear);
    // The receiver searches every quarter frame, and a search takes a
    // moment on top of that.
    auto decodeLatency = [](double frameSeconds) { return frameSeconds / 4.0 + GLISSANDO_SEARCH_SECONDS; };
    // Our own chords as set: 0.6 s to open, and a bar of four symbols or
    // the CW tail to close.
    // The far end's opening chord whether or not it plays one, which only
    // waits a little longer for an answer, and as long as a bar at the
    // slowest tempo, which is what a station on an older build opens with.
    auto chordFor = [](double frameSeconds) { return frameSeconds * 4.0 / Glissando::SYMBOLS_PER_FRAME; };
    // With chords on, an answer is heard by its opening chord, a moment
    // after the chord ends, so the reply window need not wait for its first
    // frame (Jeff's call, 2026-10-01: a second ping held 2 minutes at
    // Adagio behind one nobody answered). The far end's chord is taken to
    // be as long as an older build's, as above.
    const double replyChord = std::max(Glissando::OPENING_CHORD_SECONDS, chordFor(slowestFrameSeconds));
    return AirTiming::forFrameSeconds(info.frameSeconds(),
                                      Glissando::SEGMENT_DATA_BYTES * info.voices,
                                      decodeLatency(info.frameSeconds()), slowestFrameSeconds,
                                      decodeLatency(slowestFrameSeconds),
                                      chords ? Glissando::OPENING_CHORD_SECONDS : 0.0,
                                      replyChord, closingSeconds,
                                      chords ? replyChord + CHORD_HEARD_SECONDS : 0.0);
}

double TextMessagingModem::airTimeScale(int gear) const
{
    if (!glissandoOn_.load(std::memory_order_acquire)) return 1.0;

    int current = 0;
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        current = transmitGearLocked();
    }

    // A duet carries two payloads per frame.
    auto secondsPerPayload = [](int g) {
        const Glissando::GearInfo& info = Glissando::gearInfo(g);
        return info.frameSeconds() / info.voices;
    };
    return secondsPerPayload(gear) / secondsPerPayload(current);
}

double TextMessagingModem::glissandoMessageSeconds(const std::string& text, const std::string& callsign) const
{
    if (!glissandoOn_.load(std::memory_order_acquire) || text.empty()) return 0.0;

    int gear = 0;
    bool chords = false;
    double closingSeconds = 0.0;
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        gear = transmitGearLocked();
        chords = glissando_.chords;
        closingSeconds = closingSecondsLocked(gear, steadyMs());
    }
    const Glissando::GearInfo& info = Glissando::gearInfo(gear);

    // The link trims a frame's zero padding off before cutting it into
    // segments, so a fragment costs its header and its coded text, cut as
    // the protocol cuts the message in sendMessage().
    std::string sender = FrameCodec::normalizeCallsign(callsign).empty() ? std::string("N0CALL") : callsign;
    std::vector<Glissando::LinkBurst> bursts;
    for (size_t at = 0; at < text.size() && bursts.size() < (size_t)MAX_FRAGMENTS_PER_MESSAGE;)
    {
        size_t fits = FrameCodec::textThatFits(sender, text, at);
        if (fits == 0) break;
        Frame frame;
        frame.type = FrameType::Message;
        frame.originCallsign = sender;
        frame.fragmentCount = MAX_FRAGMENTS_PER_MESSAGE;
        frame.payload.assign(text.begin() + (long)at, text.begin() + (long)(at + fits));
        bursts.push_back({true, FrameCodec::encode(frame, TEXT_FRAME_BYTES)});
        at += fits;
    }

    return Glissando::framesForBursts(bursts, info.voices) * info.frameSeconds() +
           (chords ? Glissando::OPENING_CHORD_SECONDS : 0.0) + closingSeconds;
}

bool TextMessagingModem::cwTailDueLocked(uint64_t nowMs) const
{
    if (glissando_.tail != GlissandoConfig::Tail::Cw && glissando_.tail != GlissandoConfig::Tail::CwStraight)
    {
        return false;
    }
    if (Glissando::cwSendable(glissando_.cwText).empty()) return false;
    if (!Glissando::cwTailFits(glissando_.cwText, glissando_.cwWpm)) return false;
    return lastCwTailMs_ == 0 || nowMs - lastCwTailMs_ >= (uint64_t)std::max(glissando_.cwIdMinutes, 0) * 60000;
}

double TextMessagingModem::farEndTailSecondsLocked(int gear) const
{
    // Nothing on the air says how a station closes, or whether its CW
    // identification is due, so this allows for it every time: the bar of
    // the tempo heard, or a CW tail as long as ours at the usual speed. At
    // Presto that keeps a weak station's answer back about three seconds
    // more; at Andante and slower the decode takes longer than any tail.
    double seconds = Glissando::chordSeconds(gear);
    int wpm = std::min(glissando_.cwWpm, Glissando::CW_DEFAULT_WPM);
    if (Glissando::cwTailFits(glissando_.cwText, wpm))
    {
        seconds = std::max(seconds, Glissando::cwTailSeconds(glissando_.cwText, wpm));
    }
    return seconds;
}

double TextMessagingModem::closingSecondsLocked(int gear, uint64_t nowMs) const
{
    if (cwTailDueLocked(nowMs)) return Glissando::cwTailSeconds(glissando_.cwText, glissando_.cwWpm);
    return glissando_.tail == GlissandoConfig::Tail::Off ? 0.0 : Glissando::chordSeconds(gear);
}

// A station we sent to in the last quarter hour is one we expect to hear
// from; the few most recent are worth a guess each.
static constexpr uint64_t WORKING_MILLISECONDS = 15 * 60 * 1000;
static constexpr size_t WORKING_GUESSES = 3;

std::vector<Glissando::KnownBits> TextMessagingModem::expectedFrames(const std::string& ownCallsign,
                                                                     const std::vector<std::string>& workingWith)
{
    std::vector<Glissando::KnownBits> known;
    if (FrameCodec::normalizeCallsign(ownCallsign).empty()) return known;

    uint8_t bytes[FrameCodec::EXPECTED_START_BYTES];
    uint8_t masks[FrameCodec::EXPECTED_START_BYTES];
    for (const std::string& station : workingWith)
    {
        if (FrameCodec::normalizeCallsign(station).empty()) continue;
        FrameCodec::expectedFrameStart(ownCallsign, station, bytes, masks);
        known.push_back(Glissando::firstSegmentKnownBits(bytes, masks, FrameCodec::EXPECTED_START_BYTES));
    }
    FrameCodec::expectedFrameStart(ownCallsign, std::string(), bytes, masks);
    known.push_back(Glissando::firstSegmentKnownBits(bytes, masks, FrameCodec::EXPECTED_START_BYTES));
    return known;
}

void TextMessagingModem::noteKeyingLocked(const std::vector<OutgoingBurst>& bursts, uint64_t nowMs)
{
    for (const OutgoingBurst& burst : bursts)
    {
        Frame frame;
        if (FrameCodec::decode(burst.frame.data(), (int)burst.frame.size(), frame))
            ownCallsign_ = frame.originCallsign;
        std::string station = FrameCodec::normalizeCallsign(burst.destination);
        if (!station.empty()) working_[station] = nowMs;
    }

    std::vector<std::pair<uint64_t, std::string>> recent;
    for (auto it = working_.begin(); it != working_.end();)
    {
        if (nowMs - it->second > WORKING_MILLISECONDS)
        {
            it = working_.erase(it);
            continue;
        }
        recent.push_back({it->second, it->first});
        ++it;
    }
    std::sort(recent.begin(), recent.end(), std::greater<std::pair<uint64_t, std::string>>());
    std::vector<std::string> workingWith;
    for (size_t i = 0; i < recent.size() && i < WORKING_GUESSES; i++) workingWith.push_back(recent[i].second);
    glissandoRx_->setKnownBits(expectedFrames(ownCallsign_, workingWith));
}

std::string TextMessagingModem::cwTailText(const std::string& format, const std::string& callsign)
{
    static const std::string TOKEN = "<MYCALL>";
    std::string text;
    size_t at = 0;
    for (size_t found; (found = format.find(TOKEN, at)) != std::string::npos; at = found + TOKEN.size())
    {
        if (callsign.empty()) return std::string();
        text += format.substr(at, found - at) + callsign;
    }
    return text + format.substr(at);
}

int TextMessagingModem::transmitGearLocked() const
{
    if (glissando_.autoGear && glissandoStatus_.haveReport && glissandoStatus_.advisedGear != 0 &&
        steadyMs() - glissandoStatus_.heardAtMs < (uint64_t)GLISSANDO_REPORT_LIFETIME_MS)
    {
        return glissandoStatus_.advisedGear;
    }
    return glissando_.gear;
}

void TextMessagingModem::configureGlissandoReceiverLocked()
{
    std::vector<int> gears;
    if (glissando_.listenAllGears)
    {
        for (int gear = Glissando::MIN_GEAR; gear <= Glissando::MAX_GEAR; gear++) gears.push_back(gear);
    }
    else
    {
        gears.push_back(glissando_.gear);
        int current = transmitGearLocked();
        if (current != glissando_.gear) gears.push_back(current);
    }
    glissandoRx_->configure(gears, glissando_.scale, glissando_.tuningOffsetHz, glissando_.listenAllScales);
}

void TextMessagingModem::onGlissandoDecode(const Glissando::StreamDecode& decode)
{
    const Glissando::Decode& d = decode.decode;
    Glissando::LinkBurst burst;
    bool complete = false;
    float snr = (float)d.report.snrDb;
    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        glissandoStatus_.haveReport = true;
        glissandoStatus_.report = d.report;
        glissandoStatus_.heardGear = decode.gear;
        glissandoStatus_.heardScale = d.scale;
        glissandoStatus_.heardAtMs = steadyMs();
        glissandoStatus_.advisedGear = Glissando::recommendGear(d.report.snrDb, d.report.dopplerHz);

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
            heard.notesHz = Glissando::scaleNotes(d.scale, d.voice);
            for (double& hz : heard.notesHz) hz += glissando_.tuningOffsetHz + d.frequencyOffsetHz;

            // Both voices of a duet frame start together; a new frame
            // replaces the notes of the last.
            uint64_t sinceLast = heard.startMs > lastHeardStartMs_ ? heard.startMs - lastHeardStartMs_
                                                                   : lastHeardStartMs_ - heard.startMs;
            if (sinceLast > 500) lastHeardNotesHz_.clear();
            lastHeardStartMs_ = heard.startMs;
            lastHeardNotesHz_.insert(lastHeardNotesHz_.end(), heard.notesHz.begin(), heard.notesHz.end());
            heard.melody = Glissando::payloadMelody(d.payload);
            heard.snrDb = d.report.snrDb;
            if (glissandoHeard_.size() >= GLISSANDO_HEARD_LIMIT) glissandoHeard_.erase(glissandoHeard_.begin());
            glissandoHeard_.push_back(std::move(heard));
        }
    }
    if (complete) completedFrameEnd_.store(glissandoRx_->lastFrameEnd(), std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(chordMutex_);
        chordListener_.heardFrame();
    }
    lastSyncMs_.store(steadyMs(), std::memory_order_release);

    if (rxLogEnabled())
    {
        log_info("RX: Glissando %s %s voice %d, %.1f dB, %.2f Hz Doppler, offset %+.1f Hz%s",
                 Glissando::gearInfo(decode.gear).tempo, Glissando::scaleName(d.scale), d.voice, d.report.snrDb,
                 d.report.dopplerHz, d.frequencyOffsetHz, complete ? ", burst complete" : "");
    }
    if (!complete) return;

    Frame frame;
    if (!FrameCodec::decode(burst.bytes.data(), (int)burst.bytes.size(), frame))
    {
        if (rxLogEnabled()) log_info("RX: Glissando burst is not a chat frame");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(glissandoMutex_);
        stationGears_.heard(frame.originCallsign, decode.gear, steadyMs());
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
