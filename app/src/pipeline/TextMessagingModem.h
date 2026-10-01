//=========================================================================
// Name:            TextMessagingModem.h
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

#ifndef AUDIO_PIPELINE__TEXT_MESSAGING_MODEM_H
#define AUDIO_PIPELINE__TEXT_MESSAGING_MODEM_H

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AnswerTempo.h"
#include "FrameCodec.h"
#include "TextMessagingTypes.h"
#include "GlissandoChord.h"
#include "GlissandoCw.h"
#include "GlissandoLink.h"
#include "GlissandoModem.h"
#include "GlissandoReceiver.h"

// Forward declaration of the struct implemented by Codec2.
extern "C"
{
    struct freedv;
}

// Text messaging rides on the codec2 raw data modes rather than on RADE:
// DATAC13 for the short signalling frames and DATAC4 for message text, both
// the most robust choice in their size class. Each frame goes out as its own
// burst (preamble, frame, postamble) inside a single keying of the
// transmitter, so the receiving modem can acquire every frame independently
// instead of having to hold sync across the whole message.
//
// Threading: open() and close() must run on the same thread (the GUI thread),
// because codec2's realtime allocator is thread local and memory taken from
// one thread's pool cannot be returned on another. modulate() is called from
// the GUI thread and demodulate() only from the audio tap thread; the two use
// separate modem instances and so never contend.
class TextMessagingModem
{
public:
    using FrameCallback = std::function<void(const TextMessaging::Frame& frame, float snr)>;

    TextMessagingModem();
    ~TextMessagingModem();

    TextMessagingModem(const TextMessagingModem&) = delete;
    TextMessagingModem& operator=(const TextMessagingModem&) = delete;

    bool open();
    void close();
    bool isOpen() const;

    // Called before audio starts. The callback runs on the tap thread.
    void setFrameCallback(FrameCallback callback);

    // Turns a keying's bursts into 8 kHz samples ready for the transmitter,
    // each burst in its own mode and complete with its own preamble, so a
    // keying may mix the two. Returns false if the modem is not open or a
    // frame is the wrong size for the mode it is to be sent in. frameEndsOut,
    // if given, gets where each modem frame ends in samplesOut: the only
    // places a keying too long for the time-out timer may be cut.
    bool modulate(const std::vector<TextMessaging::OutgoingBurst>& bursts,
                  std::vector<short>& samplesOut,
                  std::vector<size_t>* frameEndsOut = nullptr);

    // Feeds received 8 kHz audio to both demodulators, invoking the frame
    // callback for every frame that passes the modem's CRC.
    void demodulate(const short* samples, int numSamples);

    // True while either demodulator is locked onto a burst, or was within
    // CHANNEL_BUSY_HOLD_MILLISECONDS: somebody else has the channel. Safe to
    // call from any thread.
    bool isReceiving() const;

    // Puts both demodulators back to searching for a preamble and forgets
    // any sync they reported. Called at the end of our own transmission: the
    // receive path is not run while we are keyed, so a demodulator that was
    // locked onto a burst when we keyed is still "locked" when we unkey,
    // reports the channel busy on silence, and, expecting payload rather
    // than a preamble, misses the first burst that actually arrives. The
    // bench lost a frame that way. Safe to call from any thread.
    void resetReceivers();

    // Glissando: instead of the codec2 data modes, chat bursts go out as the
    // Glissando melodic chirp mode, cut into 77 bit segments (see
    // GlissandoLink.h), and the receive audio goes to a Glissando streaming
    // receiver. Safe to call from any thread, before or after open().
    struct GlissandoConfig
    {
        bool enabled = false;
        int gear = 3;                   // chosen by hand
        bool autoGear = true;           // shift from the last report
        Glissando::Scale scale = Glissando::Scale::Pentatonic;    // the scale we send in
        double tuningOffsetHz = 0.0;
        bool listenAllGears = true;
        bool listenAllScales = true;    // hear stations singing in any scale, not just ours
        bool chords = true;             // open each keying with the E4+D5 chord

        // How each keying ends: nothing, a bar of every note of the scale,
        // or cwText (callsign already filled in) in Morse at cwWpm, either
        // sung by the CW Glorifier or keyed straight on E4 and D5. The CW tail plays at most once every cwIdMinutes (every
        // keying at 0), the chord in between; it doubles as the station ID.
        // With no text, or a tail too long to fit (see GlissandoCw.h), the
        // chord closes the keying instead.
        enum class Tail
        {
            Off,
            Chord,
            Cw,             // glorified
            CwStraight,
        };
        Tail tail = Tail::Cw;
        std::string cwText;
        int cwWpm = Glissando::CW_DEFAULT_WPM;
        int cwIdMinutes = 10;
    };

    // The CW tail's text: format with every <MYCALL> replaced by callsign.
    // Empty when the format names the callsign and none is set.
    static std::string cwTailText(const std::string& format, const std::string& callsign);

    void setGlissando(const GlissandoConfig& config);
    GlissandoConfig glissandoConfig() const;

    struct GlissandoStatus
    {
        bool haveReport = false;        // a frame has been decoded
        Glissando::ChannelReport report;
        int heardGear = 0;
        Glissando::Scale heardScale = Glissando::Scale::Pentatonic;
        uint64_t heardAtMs = 0;         // steady clock
        int advisedGear = 0;            // recommendGear() of the report, 0 none
        int transmitGear = 3;           // the tempo modulate() will use now
    };

    GlissandoStatus glissandoStatus() const;

    // Every Glissando frame heard, one entry per voice decoded, so the
    // console can write what each one said on its waterfall as it arrives.
    // Kept until taken, at most the newest GLISSANDO_HEARD_LIMIT.
    struct GlissandoHeard
    {
        uint64_t startMs = 0;           // steady clock, when the frame began on the air
        int gear = 0;
        Glissando::Scale scale = Glissando::Scale::Pentatonic;
        int voice = 0;
        std::array<double, Glissando::NOTES> notesHz{};    // as heard: tuning and drift included
        std::array<int, Glissando::SYMBOLS_PER_FRAME> melody{};
        double snrDb = 0.0;
        Glissando::SegmentProgress segment;
    };

    static constexpr size_t GLISSANDO_HEARD_LIMIT = 64;

    std::vector<GlissandoHeard> takeGlissandoHeard();

    // Every Glissando frame modulate() has made, one entry per voice, in
    // the order they go on the air, so the console can draw our own tune as
    // it is sent. Kept until taken, at most the newest GLISSANDO_SENT_LIMIT.
    struct GlissandoSent
    {
        int gear = 0;
        Glissando::Scale scale = Glissando::Scale::Pentatonic;
        int voice = 0;
        std::array<double, Glissando::NOTES> notesHz{};    // tuning included
        std::array<int, Glissando::SYMBOLS_PER_FRAME> melody{};
        double leadSeconds = 0.0;       // the opening chord, sung before this frame
        double tailSeconds = 0.0;       // the closing chord, sung after it

        // A CW tail sung after this frame instead of the chord: one note
        // index per Morse unit (-1 for silence), each tailUnitSeconds long.
        // A straight tail's second note is in tailHarmony, empty otherwise.
        std::vector<int> tailMelody;
        std::vector<int> tailHarmony;
        double tailUnitSeconds = 0.0;
    };

    static constexpr size_t GLISSANDO_SENT_LIMIT = 256;

    std::vector<GlissandoSent> takeGlissandoSent();

    // The protocol timers suited to what is on the air now: the codec2
    // defaults, or ones sized to the Glissando tempo we transmit at.
    TextMessaging::AirTiming airTiming() const;

    // How long a new message of textBytes would be on the air, in seconds,
    // sent on its own at the tempo modulate() would pick now. Zero when chat
    // goes over the codec2 modes, whose messages are all well under a minute.
    double glissandoMessageSeconds(size_t textBytes) const;

private:
    struct Demodulator
    {
        struct freedv* modem = nullptr;
        const char* name = "";          // the mode, for FREEDV_TEXT_CHAT_RX_LOG
        std::vector<short> buffer;      // samples not yet consumed by the modem
        std::vector<uint8_t> bytes;     // one modem frame of decoded payload
        int payloadBytes = 0;
    };

    void closeLocked();
    void demodulateOne(Demodulator& demodulator, const short* samples, int numSamples);
    bool modulateFrame(struct freedv* modem, const std::vector<uint8_t>& frame,
                       std::vector<short>& samplesOut);

    Demodulator signallingRx_;
    Demodulator textRx_;
    struct freedv* signallingTx_;
    struct freedv* textTx_;
    std::atomic<bool> open_;

    // When a demodulator last reported sync, on the steady clock; zero for
    // never. Written by the tap thread, read by the protocol's.
    std::atomic<uint64_t> lastSyncMs_;

    // Held by demodulate() and by open()/close(), so the receive tap can never
    // be inside the modem while it is being torn down at shutdown.
    std::mutex rxMutex_;
    std::mutex callbackMutex_;
    FrameCallback frameCallback_;

    // Glissando. The receiver runs its own worker thread; its callback lands
    // in onGlissandoDecode(), which reassembles segments into chat frames.
    void onGlissandoDecode(const Glissando::StreamDecode& decode);
    void configureGlissandoReceiverLocked();
    int transmitGearLocked() const;
    // True when the next keying ends with the CW tail rather than the chord.
    bool cwTailDueLocked(uint64_t nowMs) const;
    // How long the next keying's tail lasts at this gear.
    double closingSecondsLocked(int gear, uint64_t nowMs) const;

    // How long a station heard in this tempo may go on sounding after its
    // last frame: its closing chord or CW tail.
    double farEndTailSecondsLocked(int gear) const;

    mutable std::mutex glissandoMutex_;
    GlissandoConfig glissando_;
    GlissandoStatus glissandoStatus_;
    Glissando::Reassembler reassembler_;
    std::vector<GlissandoHeard> glissandoHeard_;
    std::vector<GlissandoSent> glissandoSent_;
    TextMessaging::StationTempos stationGears_; // the gear each station was last heard in
    std::unique_ptr<Glissando::StreamingReceiver> glissandoRx_;

    // Hears the chord that opens a Glissando transmission, a frame and more
    // before the receiver can decode any of it: carrier sense for the chat
    // protocol. Pushed on the receive tap's thread and configured on the
    // GUI's, hence its own lock; isSounding() needs none.
    std::mutex chordMutex_;
    Glissando::ChordListener chordListener_;
    bool chordWasSounding_ = false;                 // under chordMutex_
    std::atomic<long long> chordStoppedAt_{-1};     // receiver samples, when it last stopped sounding
    uint64_t lastCwTailMs_ = 0;         // steady clock; zero before the first, under glissandoMutex_
    std::atomic<bool> glissandoOn_;

    // The end of the frame that completed the last burst heard, as the
    // receiver counts samples; -2 for none. While it is still the newest
    // frame heard, that burst is over and the channel is free.
    std::atomic<long long> completedFrameEnd_{-2};
};

// The application wide modem. Opened and closed by MainFrame on the GUI
// thread; the receive step and the text messaging session both talk to it.
TextMessagingModem& textMessagingModem();

#endif // AUDIO_PIPELINE__TEXT_MESSAGING_MODEM_H
