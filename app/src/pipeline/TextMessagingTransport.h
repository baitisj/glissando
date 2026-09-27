//=========================================================================
// Name:            TextMessagingTransport.h
// Purpose:         Puts text messaging bursts on the air.
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

#ifndef AUDIO_PIPELINE__TEXT_MESSAGING_TRANSPORT_H
#define AUDIO_PIPELINE__TEXT_MESSAGING_TRANSPORT_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "TextMessagingProtocol.h"

class TextMessagingModem;

// Modulates a burst, hands it to the transmit thread and keys the radio for
// exactly as long as the burst takes. The PTT function is supplied by
// MainFrame, which is the only thing that may touch the transmit controls; it
// is called from the session's tick thread, so MainFrame's implementation is
// responsible for getting onto the GUI thread.
class TextMessagingTransport : public TextMessaging::ITextMessagingTransport
{
public:
    using PttFunction = std::function<void(bool keyed)>;
    using VoiceTransmitCheck = std::function<bool()>;

    explicit TextMessagingTransport(TextMessagingModem* modem);
    virtual ~TextMessagingTransport() = default;

    void setPttFunction(PttFunction pttFunction);
    void setVoiceTransmitCheck(VoiceTransmitCheck voiceTransmitCheck);

    // Must return true for a burst to be sent at all: there is no point
    // keying the radio when the audio threads that would play the burst are
    // not running.
    void setTransmitAllowedCheck(VoiceTransmitCheck transmitAllowedCheck);

    // The longest the radio may stay keyed, in milliseconds, or 0 for no
    // limit: the transmit time-out timer. A keying that would run longer is
    // sent as several, letting up on the transmitter between whole frames.
    // Called once per keying.
    using KeyingLimitFunction = std::function<int()>;
    void setKeyingLimitFunction(KeyingLimitFunction keyingLimitFunction);

    virtual bool transmit(const std::vector<TextMessaging::OutgoingBurst>& bursts) override;
    virtual bool isTransmitting() const override;
    virtual bool isChannelBusy() const override;
    virtual void poll() override;

    // Drops anything queued and unkeys. Used when audio stops.
    void abort();

private:
    void unkey();

    // Keys the radio for the next part of a burst once the pause before it
    // is over.
    void resumeAfterPause(uint64_t nowMs, uint64_t resumeAtMs, uint64_t giveUpAtMs);

    // Starts sending the part of the burst up to keyingEnds_[keying_].
    void startKeyingLocked(uint64_t nowMs);

    // Moves as much of the current keying as the transmit queue has room
    // for into it.
    void feedQueueLocked();

    // Whether the main window has the radio keyed, for voice or for us.
    bool pttHeld() const;

    TextMessagingModem* modem_;
    PttFunction pttFunction_;
    VoiceTransmitCheck voiceTransmitCheck_;
    VoiceTransmitCheck transmitAllowedCheck_;
    KeyingLimitFunction keyingLimitFunction_;

    mutable std::mutex mutex_;
    std::vector<short> samples_;    // the burst being sent, reused per burst
    size_t queued_;                 // how much of samples_ is in the queue
    std::vector<size_t> frameEnds_; // where each modem frame of it ends

    // A burst too long for the time-out timer goes out as several keyings,
    // cut between frames. keyed_ stays set across the pauses between them,
    // so the protocol sees one transmission and nobody else's turn starts.
    std::vector<size_t> keyingEnds_;
    size_t keying_;                 // index into keyingEnds_ of the one on air
    bool pausing_;                  // radio let up between two keyings
    uint64_t resumeAtMs_;
    uint64_t giveUpAtMs_;           // the operator took the radio meanwhile

    std::atomic<bool> keyed_;
    uint64_t keyedAtMs_;            // of the current keying
    uint64_t keyDeadlineMs_;

    // Timing of the burst currently on the air, for FREEDV_TEXT_CHAT_TX_LOG.
    // The transmit queue's flags are read from the realtime thread and must
    // not be logged there, so poll() watches them from the session thread and
    // reports the transitions it sees.
    uint64_t burstMs_;
    bool sawTransmitting_;
    bool sawEmpty_;

    // Busy spells as last reported, for FREEDV_TEXT_CHAT_TX_LOG only.
    bool loggedChannelBusy_;
    uint64_t channelBusySinceMs_;
};

#endif // AUDIO_PIPELINE__TEXT_MESSAGING_TRANSPORT_H
