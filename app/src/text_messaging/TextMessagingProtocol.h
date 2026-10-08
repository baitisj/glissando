//=========================================================================
// Name:            TextMessagingProtocol.h
// Purpose:         Message, acknowledgement and ping state machine.
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

#ifndef TEXT_MESSAGING__TEXT_MESSAGING_PROTOCOL_H
#define TEXT_MESSAGING__TEXT_MESSAGING_PROTOCOL_H

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "FrameCodec.h"
#include "TextMessagingTypes.h"

namespace TextMessaging
{

class HeardStationList;
class MessageStore;

// Implemented by the audio pipeline. A transmission is one keying of the
// transmitter carrying every burst handed to transmit(), in order, each in
// its own mode; once transmit() returns true, isTransmitting() must keep
// returning true until the last sample of the last burst has been sent, and
// must also be true while the operator is transmitting voice. That contract
// is what lets the protocol know when its acknowledgement timer should start.
class ITextMessagingTransport
{
public:
    virtual ~ITextMessagingTransport() = default;

    virtual bool transmit(const std::vector<OutgoingBurst>& bursts) = 0;
    virtual bool isTransmitting() const = 0;

    // Called from the same loop that drives tick(), so a transport can finish
    // a burst (unkey the transmitter) without a timer of its own.
    virtual void poll() { /* nothing to do by default */ }

    // True while the receiver is locked onto somebody else's burst. The
    // protocol freezes on it: nothing starts, and no acknowledgement timer
    // runs down, until the channel is clear again.
    virtual bool isChannelBusy() const { return false; }

    // How many times longer a burst takes at the given Glissando tempo than
    // at the one set now, for a message the operator moved to another
    // tempo; see AirTiming, which is sized to the tempo set now.
    virtual double airTimeScale(int gear) const
    {
        (void)gear;
        return 1.0;
    }

    // A transport that takes its own turns on the channel, as data2g-host
    // does for the GLISS group: it waits for a clear channel and for any
    // session to end, so the protocol keeps none of its own pauses (no
    // turnarounds, reply windows, channel-busy holds or retry backoffs) and
    // hands over the next keying as soon as the last has gone out. Only
    // "Woah!" holds it. One keying at a time still, so what waits behind it
    // can be cancelled. Our own modem says no.
    virtual bool pacesItself() const { return false; }

    // Takes back the keying given to transmit() if none of it has started
    // to go out. False once it has, or with nothing to take back.
    virtual bool withdrawKeying() { return false; }

    // A link that acknowledges, retries and paces what it carries by itself,
    // such as a Data2G connected session. Nothing else has one: the defaults
    // say no, and the protocol then works exactly as it does without them.
    //
    // Whether a keying for this one station would go through such a link now.
    virtual bool deliversReliablyTo(const std::string& destination) const
    {
        (void)destination;
        return false;
    }

    // Hands a keying for one station to that link, without holding the
    // transmitter: the link sends it when it can, and says what became of it
    // in a report carrying keyingId. False if it cannot take it now.
    virtual bool transmitReliably(const std::vector<OutgoingBurst>& bursts, uint64_t keyingId)
    {
        (void)bursts;
        (void)keyingId;
        return false;
    }

    // The reports on keyings given to transmitReliably() since the last call,
    // in the order they were settled.
    virtual std::vector<KeyingReport> takeKeyingReports() { return {}; }

    // Takes back a keying given to transmitReliably() that has not yet gone
    // into the link. False once it has (it is the link's to finish and
    // report on), or for one it never had.
    virtual bool withdrawReliably(uint64_t keyingId)
    {
        (void)keyingId;
        return false;
    }

    // The operator has let go of this station. A transport that keeps
    // something open for one station, as Data2G keeps a session, ends it
    // at once, forgets every keying for the station it still holds without
    // reporting it, and says true, so the protocol drops what it has
    // outstanding for the station too. Nothing else does anything: false.
    virtual bool releaseStation(const std::string& destination)
    {
        (void)destination;
        return false;
    }
};

// Implemented by the dialog. Callbacks arrive on whichever thread drove the
// change and never with the protocol's lock held.
class ITextMessagingObserver
{
public:
    virtual ~ITextMessagingObserver() = default;

    virtual void onMessageAdded(const TextMessage& message) = 0;
    virtual void onMessageUpdated(const TextMessage& message) = 0;
    virtual void onStationsChanged() = 0;
};

// Drives chat over the air: fragments outgoing messages, retries the ones that
// are not acknowledged, reassembles incoming ones, answers pings, and keeps
// the message store and heard station list up to date.
//
// Only one transmission is outstanding at a time, which is all a half duplex
// channel can honestly support; anything else queued waits its turn.
class TextMessagingProtocol
{
public:
    TextMessagingProtocol(MessageStore& store, HeardStationList& stations);
    ~TextMessagingProtocol();

    TextMessagingProtocol(const TextMessagingProtocol&) = delete;
    TextMessagingProtocol& operator=(const TextMessagingProtocol&) = delete;

    void setTransport(ITextMessagingTransport* transport);
    void setObserver(ITextMessagingObserver* observer);

    void setMyCallsign(const std::string& callsign);
    std::string myCallsign() const;

    // Automatic acknowledgements and pongs. On by default; the dialog exposes
    // it so that an operator who must not transmit unattended can turn the
    // station into a receive only chat client.
    void setAutoReplyEnabled(bool enabled);
    bool autoReplyEnabled() const;

    // Whether a station acknowledges by itself, as far as we have heard: its
    // messages, broadcasts and pings say so, and an acknowledgement or pong
    // from it does too. Until it says otherwise, every station does. A
    // message to one that does not is sent once, without waiting for an
    // acknowledgement that will not come, and so without retries.
    bool stationAutoAcks(const std::string& callsign) const;

    // Our Maidenhead locator, as typed (FrameCodec::normalizeLocator), and
    // whether its grid square goes on the air. It rides as the last burst of
    // our directed messages to a station that understands locator frames,
    // once a contact and on every message to it until it says it has it;
    // and it fills the spare voice of a duet keying. Changing it sends it
    // again to everybody. An empty or unusable locator sends nothing.
    void setMyLocator(const std::string& locator, bool send);
    std::string myLocator() const;

    // A station's grid square as last heard from it, this session or an
    // earlier one, or empty.
    std::string stationLocator(const std::string& callsign) const;

    // Whether that grid square came from the station in this contact, rather
    // than being kept from an earlier one, when it may have moved since.
    bool stationLocatorIsCurrent(const std::string& callsign) const;

    // What earlier sessions learned of stations' locators, from the store.
    void restoreStationLocators(const std::vector<StationLocator>& stations);

    // The station the console's map shows. Until the operator picks one in
    // the station list, the last one we heard or keyed to whose locator we
    // know; a station whose locator we do not know leaves it as it was.
    // Empty until there is one.
    std::string mapStation() const;

    // The operator's pick in the station list, which the map then follows
    // instead of the traffic: the station picked, whether or not its
    // locator is known yet, or with the pick cleared, none, so the map
    // shows no path until another station is picked.
    void setMapSelection(const std::string& callsign);

    // Stops every chat transmission, for a station on a frequency where it
    // may not send data. Whatever is waiting to go out is discarded as not
    // sent, including a retry or a reply that becomes due while inhibited,
    // and sending is refused with this reason. A keying already on the air
    // finishes, and a message already sent can still be acknowledged, since
    // receiving carries on. An empty reason lifts it.
    void setTransmitInhibited(const std::string& reason);
    std::string transmitInhibitedReason() const;

    // The operator has stopped the transmitter. Everything outstanding is
    // dropped as aborted, so nothing keys again on its own: what was on the
    // air, what was waiting behind it, and what was sent and waiting for an
    // acknowledgement, which would otherwise retry. The transport stops the
    // keying itself.
    void abortTransmission();

    // "Woah!": the operator hears somebody the receiver has missed. Nothing
    // keys, replies included, until one more of the modem's frames could
    // have gone by, added to any such hold still running, so each press
    // buys more. A keying already on the air carries on. Returns how long the
    // hold now has to run.
    uint64_t holdTransmissions();

    // Replaces the clocks the protocol reads. Milliseconds must be monotonic
    // (timeouts) and the wall clock is what the chat window timestamps with.
    void setClocks(std::function<uint64_t()> monotonicMs, std::function<std::time_t()> wallClock);

    // Replaces every timer the protocol runs; see AirTiming. Takes effect
    // for waits started after the call.
    void setAirTiming(const AirTiming& timing);
    AirTiming airTiming() const;

    // Queues a message. Pass an empty destination to broadcast. Returns false
    // with a reason in errorOut if the message cannot be sent at all: no
    // callsign configured, empty or oversized text, or no transport.
    bool sendMessage(const std::string& text, const std::string& destination,
                     std::string& errorOut);

    // Queues a ping to a single station.
    bool sendPing(const std::string& destination, std::string& errorOut);

    // Called by the receive step for every frame the modem decodes.
    // viaReliableLink: it came through a link that acknowledges by itself
    // (see ITextMessagingTransport::transmitReliably), so it is answered by
    // that link, and says nothing about who has the channel.
    void onFrameReceived(const Frame& frame, float snr, bool viaReliableLink = false);

    // Drives transmission, retries and timeouts. Call it a few times a second
    // from the GUI timer; it does no work of its own when nothing is pending.
    void tick();

    // Number of transmissions waiting, including the one on the air. Used by
    // the dialog to show that something is still queued.
    size_t pendingCount() const;

    // What we are waiting to hear back, if anything. Reported for the whole
    // acknowledgement cycle including the retries, so the status line does not
    // flicker as the message goes back on the air.
    AckWait ackWait() const;

    // True while a burst is actually on the air, ours or the voice keyer's.
    bool isTransmitting() const;

    // True while anything of ours is waiting to go on the air: a message, a
    // retry, a ping or a reply. With the console disengaged there is no
    // transmitter to take it, and the operator is told so.
    bool hasQueuedTransmissions() const;

    // Whether somebody else has the channel, as of the last tick: the
    // receiver hears them, or a station said it has more bursts to come.
    // Nothing of ours keys meanwhile.
    bool channelHeld() const;

    // Whether the chat message with this id is one of them. A message left
    // queued when the app last closed is in the history but not the queue,
    // and engaging will not send it.
    bool isMessageQueued(int64_t messageId) const;

    // How long each chat message waiting for its first turn on the air has
    // still to wait, in queue order: the keying on the air and the far end's
    // turn after it, the turnarounds and holds now running, and the air time
    // and answer window of everything ahead of it. An estimate, which an
    // answer arriving early shortens and a busy channel holds still.
    std::vector<QueuedWait> queuedWaits() const;

    // Sends a chat message waiting for its first turn on the air at the
    // given Glissando tempo, whatever the console or Auto picks, or
    // with 0 at whichever tempo is set when it keys. Its retries keep it.
    // False if the message is not waiting for its first turn.
    bool setMessageTempo(int64_t messageId, int gear);

    // What the operator can do with a chat message or ping of ours that is
    // still outstanding. One waiting for its first turn on the air can be
    // removed from the queue, and is then never sent. One on the air,
    // waiting for its acknowledgement or pong, or waiting to be retried can
    // be aborted. None for anything else: delivered, failed, already
    // stopped, or not ours.
    enum class Cancel
    {
        None,
        Remove,
        Abort,
    };
    Cancel cancelFor(int64_t messageId) const;

    // Does it: a removed message ends NotSent and an aborted one Aborted,
    // and nothing of it goes on the air again. Returns what was done, and in
    // onAirOut whether it was on the air: the caller stops that keying,
    // which this cannot do. Whatever else was queued carries on.
    Cancel cancelMessage(int64_t messageId, bool* onAirOut = nullptr);

    // The operator has deselected the station. If the transport keeps a
    // link to it (a Data2G session), the link is ended at once and every
    // message and ping of ours for the station still outstanding is dropped
    // as cancelMessage() would drop it. With any other transport, nothing
    // changes. Returns how many were dropped.
    int releaseStation(const std::string& destination);

    // The chat messages and pings still outstanding, by message store id.
    std::vector<int64_t> outstandingMessageIds() const;

private:
    enum class TransmissionState
    {
        Queued,        // waiting for a clear transmitter
        Transmitting,  // handed to the transport, burst in progress
        AwaitingAck,   // burst finished, acknowledgement timer running
        Delivering,    // with a link that acknowledges by itself, waiting on its report
    };

    struct PendingTransmission
    {
        TextMessage message;                        // the chat line it belongs to

        // What it is sent as, unencoded: a frame per fragment for a message,
        // one frame for anything else. A keying is encoded as a whole, because
        // every burst has to say how many follow it in that keying, which
        // depends on what else is in it: a message leaves out the fragments
        // the far end has confirmed, and a reply may have a message behind it.
        std::vector<Frame> frames;
        uint32_t confirmed = 0;  // fragments a partial acknowledgement said arrived
        BurstMode mode = BurstMode::Text;
        bool expectsAck = false;
        bool isPing = false;
        bool reply = false;      // an acknowledgement or pong we owe somebody
        bool ack = false;        // an acknowledgement; message.airId says of what
        bool partialAck = false; // a partial acknowledgement; ditto
        std::string destination;
        int retries = 0;
        uint64_t deadlineMs = 0;
        uint64_t sentAtMs = 0;   // end of our burst, for the reply window
        uint64_t notBeforeMs = 0; // retry backoff; nothing to do with the far end
        int gear = 0;            // the tempo the operator chose; 0 for the one set now
        uint64_t keyingId = 0;   // while Delivering: the keying the link reports on
        bool locatorRode = false; // while Delivering: our locator went in the same keying
        TransmissionState state = TransmissionState::Queued;
    };

    // A station's locator, and where our exchange of locators with it
    // stands in the current contact; see LOCATOR_CONTACT_IDLE_MILLISECONDS.
    struct LocatorPeer
    {
        std::string gridSquare;
        LocatorSupport support = LocatorSupport::Unknown;
        uint64_t lastContactMs = 0;  // last heard from it or sent to it
        bool acknowledged = false;   // it said it has our locator
        bool sent = false;           // ours went to it
        bool heardTheirs = false;    // we heard its locator
    };

    struct Reassembly
    {
        std::vector<std::string> fragments;
        uint8_t fragmentCount = 0;
        uint32_t receivedMask = 0;
        uint64_t lastHeardMs = 0;   // see REASSEMBLY_TIMEOUT_MILLISECONDS
        float snr = 0.0f;
        bool broadcast = false;

        // When the sender's current keying should be over, from what the last
        // fragment heard said was still to come, and whether anything of this
        // message was heard in it. Once it is over, a message still missing
        // fragments asks for them.
        uint64_t keyingEndsMs = 0;
        bool heardThisKeying = false;
    };

    using ReassemblyKey = std::pair<std::string, uint16_t>;

    // A message we finished taking in, kept so that a retransmission of it
    // can be recognised: by its fragments, not just its ID, because a sender
    // that restarts reuses IDs.
    struct Completed
    {
        uint64_t atMs = 0;
        std::vector<std::string> fragments;
    };

    // Events queued while the lock is held and delivered once it is released,
    // so an observer is free to call back into the protocol.
    struct PendingEvent
    {
        enum class Type { MessageAdded, MessageUpdated, StationsChanged } type;
        TextMessage message;
    };

    // All of these assume mutex_ is held.
    bool queueMessageLocked(const std::string& text, const std::string& destination,
                            std::string& errorOut, std::vector<PendingEvent>& events);
    void queueAckLocked(const std::string& destination, uint16_t airId);
    void queuePartialAckLocked(const std::string& destination, uint16_t airId, uint32_t received);
    void requestMissingFragmentsLocked(uint64_t nowMs);
    void queuePongLocked(const std::string& destination, float snr);
    void reserveChannelForKeyingLocked(const Frame& frame, uint64_t nowMs);
    void handleIncomingFragmentLocked(const Frame& frame, float snr,
                                      std::vector<PendingEvent>& events, bool viaReliableLink);
    void handleAckLocked(const Frame& frame, std::vector<PendingEvent>& events);
    void handlePartialAckLocked(const Frame& frame, std::vector<PendingEvent>& events);
    bool retryOrFailLocked(size_t index, uint64_t nowMs, std::vector<PendingEvent>& events);
    PendingTransmission* riderLocked(size_t replyIndex, uint64_t nowMs);
    std::vector<OutgoingBurst> keyingBurstsLocked(
        const std::vector<const PendingTransmission*>& entries, uint64_t nowMs,
        std::string* locatorToOut);
    void handlePingLocked(const Frame& frame, float snr, std::vector<PendingEvent>& events,
                          bool viaReliableLink);
    void handlePongLocked(const Frame& frame, float snr, std::vector<PendingEvent>& events);
    void addSystemMessageLocked(const std::string& text, const std::string& destination,
                                std::vector<PendingEvent>& events);
    void updateStatusLocked(PendingTransmission& pending, MessageStatus status,
                            std::vector<PendingEvent>& events);
    void purgeStaleReassembliesLocked(uint64_t nowMs);
    void discardQueuedLocked(std::vector<PendingEvent>& events);
    void dropOutboxLocked(MessageStatus status, bool everything, std::vector<PendingEvent>& events);
    Cancel cancelForLocked(const PendingTransmission& pending) const;
    bool withdrawKeyingLocked();

    // Holds the transmitter off until the far end has had its turn. Never
    // shortens a wait that is already running.
    void deferTransmissionLocked(uint64_t fromMs, int baseMs, int jitterMs);
    uint64_t quietUntilLocked(bool forReply) const;
    uint64_t airTimeLocked(const PendingTransmission& pending) const;
    bool channelFrozenLocked(uint64_t nowMs);
    void holdTimersLocked(uint64_t pausedMs);
    uint32_t randomDelayLocked(int maxMs);
    void serviceOutboxLocked(uint64_t nowMs, bool frozen, std::vector<PendingEvent>& events);
    bool goesReliablyLocked(const PendingTransmission& pending) const;
    void sendReliablyLocked(uint64_t nowMs, std::vector<PendingEvent>& events);
    void settleReliableKeyingsLocked(std::vector<PendingEvent>& events);
    uint16_t nextAirIdLocked();
    Frame makeFrameLocked(FrameType type, const std::string& destination, uint16_t airId,
                          uint8_t fragmentIndex, uint8_t fragmentCount,
                          const std::vector<uint8_t>& payload) const;
    bool isAddressedToMeLocked(const Frame& frame) const;
    void noteStationAutoAckLocked(const std::string& station, bool autoAck,
                                  std::vector<PendingEvent>& events);
    bool expectsAckFromLocked(const std::string& destination) const;

    LocatorPeer& locatorPeerLocked(const std::string& callsign, uint64_t nowMs);
    bool locatorRidesToLocked(const std::string& destination, uint64_t nowMs);
    uint8_t featuresForLocked(const Frame& frame, const std::string& destination) const;
    Frame locatorFrameLocked(const std::string& destination) const;
    void noteLocatorFeaturesLocked(const Frame& frame, uint64_t nowMs);
    void handleLocatorLocked(const Frame& frame, uint64_t nowMs);
    void saveLocatorPeerLocked(const std::string& callsign, const LocatorPeer& peer);
    void noteMapStationLocked(const std::string& callsign);

    void deliver(const std::vector<PendingEvent>& events);

    mutable std::mutex mutex_;
    MessageStore& store_;
    HeardStationList& stations_;
    ITextMessagingTransport* transport_;
    ITextMessagingObserver* observer_;

    std::string myCallsign_;
    uint32_t myCallsignCrc_;
    bool autoReplyEnabled_;
    std::set<std::string> noAutoAckStations_;  // heard saying Auto acknowledge is off
    std::string myLocator_;       // normalized; empty for none
    bool sendLocator_;
    std::map<std::string, LocatorPeer> locatorPeers_;
    std::string mapStation_;      // the last station heard or keyed to whose locator we know
    bool mapPicked_;              // the operator has picked a station, or cleared the pick
    std::string mapPick_;         // that station; empty when cleared

    // Whether the keying now on the air ends in our locator. A station that
    // misses that burst holds its answer for a text burst's time after the
    // message, so the wait for that answer is that much longer.
    bool keyingCarriesLocator_;
    std::string keyingLocatorTo_;   // whose locator it carries
    std::string inhibitReason_;   // empty unless transmitting is inhibited
    uint16_t nextAirId_;

    uint64_t quietUntilMs_;           // turnarounds: nothing keys before this
    uint64_t ownTrafficQuietUntilMs_; // the answered station's turn: no keying of our own
    uint64_t operatorHoldUntilMs_;    // "Woah!": nothing keys before this
    uint32_t jitterState_;

    // How long listeners may go on treating the channel as ours after the
    // keying now on the air ends; zero if no longer than the keying itself.
    // A reply with something behind it tells them more follows, and one that
    // then loses what follows reserves for two fragments after the reply.
    uint64_t keyingHeldUntilMs_;

    // When the keying now on the air should end, from its air time; for
    // queuedWaits().
    uint64_t keyingEndsMs_;

    // Our own traffic waits for the station a reply with something behind
    // it answered, for as long as keyingHeldUntilMs_ says it may still be
    // holding the channel for us, plus its turn. Hearing that station again
    // shows it has let go, and ends the wait; see quietUntilLocked().
    uint64_t answeredHoldUntilMs_;
    std::string answeredStation_;

    // Carrier sense bookkeeping, updated every tick whether or not anything
    // is queued, so a busy spell is measured from when it really began.
    bool channelBusy_;
    bool channelHeld_ = false;        // channelFrozenLocked() at the last tick
    uint64_t nextKeyingId_ = 0;       // for keyings given to a link that acknowledges by itself
    uint64_t channelBusySinceMs_;
    uint64_t channelReservedUntilMs_; // a fragmented message still on the air
    std::string channelReservedBy_;   // whose, when it was a text frame's count
    uint64_t lastTickMs_;

    std::deque<PendingTransmission> outbox_;
    std::map<ReassemblyKey, Reassembly> inbox_;
    std::map<ReassemblyKey, Completed> recentlyCompleted_;

    std::function<uint64_t()> monotonicMs_;
    std::function<std::time_t()> wallClock_;
    AirTiming timing_;
};

} // namespace TextMessaging

#endif // TEXT_MESSAGING__TEXT_MESSAGING_PROTOCOL_H
