//=========================================================================
// Name:            TextMessagingProtocol.cpp
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

#include "TextMessagingProtocol.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

#include "HeardStationList.h"
#include "MessageStore.h"

namespace TextMessaging
{

namespace
{

// A message that completed within this window and arrives again is a
// retransmission whose acknowledgement we lost, not a new message.
constexpr uint64_t DUPLICATE_WINDOW_MS = 10 * 60 * 1000;

// Message IDs start somewhere random rather than at one. Receivers remember
// (sender, ID) for DUPLICATE_WINDOW_MS, so a station that restarted and began
// again at one would have its first messages taken for retransmissions of the
// last ones: acknowledged, and never shown.
uint16_t randomAirId()
{
    std::random_device device;
    return (uint16_t)(device() & MAX_AIR_ID);
}

std::string trim(const std::string& text)
{
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// SNR travels in one byte in half dB steps, which covers every SNR the modem
// can report with room to spare.
uint8_t encodeSnr(float snr)
{
    float scaled = snr * 2.0f;
    if (scaled > 127.0f) scaled = 127.0f;
    if (scaled < -128.0f) scaled = -128.0f;
    return (uint8_t)(int8_t)std::lround(scaled);
}

float decodeSnr(uint8_t encoded)
{
    return (float)(int8_t)encoded / 2.0f;
}

std::string formatSnr(float snr)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.1f", (double)snr);
    return buffer;
}

} // namespace

TextMessagingProtocol::TextMessagingProtocol(MessageStore& store, HeardStationList& stations)
    : store_(store)
    , stations_(stations)
    , transport_(nullptr)
    , observer_(nullptr)
    , myCallsignCrc_(0)
    , autoReplyEnabled_(true)
    , sendLocator_(false)
    , keyingCarriesLocator_(false)
    , nextAirId_(randomAirId())
    , quietUntilMs_(0)
    , ownTrafficQuietUntilMs_(0)
    , operatorHoldUntilMs_(0)
    , jitterState_(1)
    , keyingHeldUntilMs_(0)
    , keyingEndsMs_(0)
    , answeredHoldUntilMs_(0)
    , channelBusy_(false)
    , channelBusySinceMs_(0)
    , channelReservedUntilMs_(0)
    , lastTickMs_(0)
    , monotonicMs_([]() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    })
    , wallClock_([]() { return std::time(nullptr); })
{
    // empty
}

TextMessagingProtocol::~TextMessagingProtocol()
{
    // empty
}

void TextMessagingProtocol::setTransport(ITextMessagingTransport* transport)
{
    std::lock_guard<std::mutex> lock(mutex_);
    transport_ = transport;
}

void TextMessagingProtocol::setObserver(ITextMessagingObserver* observer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    observer_ = observer;
}

void TextMessagingProtocol::setMyCallsign(const std::string& callsign)
{
    std::lock_guard<std::mutex> lock(mutex_);
    myCallsign_ = FrameCodec::normalizeCallsign(callsign);
    myCallsignCrc_ = FrameCodec::callsignHash(myCallsign_);

    // Seeding the backoff from our own callsign keeps it reproducible for a
    // given station while making any two stations back off differently.
    jitterState_ = myCallsignCrc_ | 1u;
}

std::string TextMessagingProtocol::myCallsign() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return myCallsign_;
}

void TextMessagingProtocol::setAutoReplyEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    autoReplyEnabled_ = enabled;
}

bool TextMessagingProtocol::autoReplyEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return autoReplyEnabled_;
}

bool TextMessagingProtocol::channelHeld() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return channelHeld_;
}

bool TextMessagingProtocol::stationAutoAcks(const std::string& callsign) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return noAutoAckStations_.count(FrameCodec::normalizeCallsign(callsign)) == 0;
}

bool TextMessagingProtocol::expectsAckFromLocked(const std::string& destination) const
{
    return !destination.empty() && noAutoAckStations_.count(destination) == 0;
}

// What a station just said about its Auto acknowledge. A change is worth a
// line in the log, since it changes what our messages to it do, and messages
// to it that have not gone out yet follow it: they wait for an
// acknowledgement, or do not, from their next keying. One already waiting
// keeps waiting, and ends as sent rather than retrying if its wait runs out
// while the station says it will not answer.
void TextMessagingProtocol::noteStationAutoAckLocked(const std::string& station, bool autoAck,
                                                     std::vector<PendingEvent>& events)
{
    std::string callsign = FrameCodec::normalizeCallsign(station);
    if (callsign.empty()) return;

    bool wasAutoAck = noAutoAckStations_.count(callsign) == 0;
    if (wasAutoAck == autoAck) return;

    if (autoAck)
    {
        noAutoAckStations_.erase(callsign);
        addSystemMessageLocked(callsign + " : Auto ACK on, messages to it retry again",
                               callsign, events);
    }
    else
    {
        noAutoAckStations_.insert(callsign);
        addSystemMessageLocked(callsign + " : Auto ACK off, messages to it go once without retries",
                               callsign, events);
    }

    for (PendingTransmission& pending : outbox_)
    {
        if (pending.isPing || pending.reply || pending.mode != BurstMode::Text) continue;
        if (pending.destination != callsign || pending.state != TransmissionState::Queued) continue;
        pending.expectsAck = autoAck;
    }
}

void TextMessagingProtocol::setMyLocator(const std::string& locator, bool send)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string normalized = FrameCodec::normalizeLocator(locator);

    // Everybody who has the old square has the wrong one.
    if (FrameCodec::packGridSquare(normalized) != FrameCodec::packGridSquare(myLocator_))
    {
        for (auto& entry : locatorPeers_)
        {
            entry.second.acknowledged = false;
            entry.second.sent = false;
        }
    }

    myLocator_ = normalized;
    sendLocator_ = send;
}

std::string TextMessagingProtocol::myLocator() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return myLocator_;
}

std::string TextMessagingProtocol::stationLocator(const std::string& callsign) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = locatorPeers_.find(FrameCodec::normalizeCallsign(callsign));
    return found == locatorPeers_.end() ? std::string() : found->second.gridSquare;
}

std::string TextMessagingProtocol::mapStation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return mapStation_;
}

void TextMessagingProtocol::noteMapStationLocked(const std::string& callsign)
{
    auto found = locatorPeers_.find(callsign);
    if (found != locatorPeers_.end() && !found->second.gridSquare.empty()) mapStation_ = callsign;
}

void TextMessagingProtocol::restoreStationLocators(const std::vector<StationLocator>& stations)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const StationLocator& station : stations)
    {
        std::string callsign = FrameCodec::normalizeCallsign(station.callsign);
        if (callsign.empty()) continue;

        // Only what lasts: a contact is over by the time the program starts.
        LocatorPeer& peer = locatorPeers_[callsign];
        peer.gridSquare = FrameCodec::unpackGridSquare(FrameCodec::packGridSquare(station.gridSquare));
        peer.support = station.support;
    }
}

// The station's entry, its contact started over if it has been quiet too
// long, and marked as in contact now.
TextMessagingProtocol::LocatorPeer& TextMessagingProtocol::locatorPeerLocked(const std::string& callsign,
                                                                              uint64_t nowMs)
{
    LocatorPeer& peer = locatorPeers_[callsign];
    if (peer.lastContactMs != 0 && nowMs - peer.lastContactMs > LOCATOR_CONTACT_IDLE_MILLISECONDS)
    {
        peer.acknowledged = false;
        peer.sent = false;
        peer.heardTheirs = false;
    }
    peer.lastContactMs = nowMs;
    return peer;
}

// Whether our locator rides behind a message to this station. Only to one
// that has said it understands it: Glissando 0.5 books the burst after the
// last text frame as a whole text burst, six segments, and would answer
// that much late. A station that acknowledges gets it until it says it has
// it; one with Auto acknowledge off, once a contact.
bool TextMessagingProtocol::locatorRidesToLocked(const std::string& destination, uint64_t nowMs)
{
    if (myLocator_.empty() || !sendLocator_ || destination.empty()) return false;

    LocatorPeer& peer = locatorPeerLocked(destination, nowMs);
    if (peer.support != LocatorSupport::Yes) return false;
    return expectsAckFromLocked(destination) ? !peer.acknowledged : !peer.sent;
}

// A ping or an acknowledgement says we take in locator frames, and an
// acknowledgement whether we have its addressee's, this contact.
uint8_t TextMessagingProtocol::featuresForLocked(const Frame& frame, const std::string& destination) const
{
    uint8_t features = FEATURE_UNDERSTANDS_LOCATOR;
    if (frame.type != FrameType::MessageAck) return features;

    auto found = locatorPeers_.find(destination);
    if (found != locatorPeers_.end() && found->second.heardTheirs) features |= FEATURE_HEARD_YOUR_LOCATOR;
    return features;
}

Frame TextMessagingProtocol::locatorFrameLocked(const std::string& destination) const
{
    Frame frame = makeFrameLocked(FrameType::Locator, destination, 0, 0, 1, {});
    frame.locator = myLocator_;
    return frame;
}

// What a ping's or acknowledgement's feature byte says about its sender,
// whoever it was for; and an acknowledgement to us, whether it has our
// locator.
void TextMessagingProtocol::noteLocatorFeaturesLocked(const Frame& frame, uint64_t nowMs)
{
    if (!FrameCodec::carriesFeatures(frame.type)) return;

    LocatorPeer& peer = locatorPeerLocked(frame.originCallsign, nowMs);
    LocatorSupport support =
        (frame.features & FEATURE_UNDERSTANDS_LOCATOR) != 0 ? LocatorSupport::Yes : LocatorSupport::No;
    if (support != peer.support)
    {
        peer.support = support;
        saveLocatorPeerLocked(frame.originCallsign, peer);
    }

    if (frame.type == FrameType::MessageAck && isAddressedToMeLocked(frame) &&
        (frame.features & FEATURE_HEARD_YOUR_LOCATOR) != 0)
    {
        peer.acknowledged = true;
    }
}

// A station's locator, whoever it rode behind a message to. It is the last
// burst of the sender's keying, which a text frame said was still to come
// and booked as a whole text burst: hearing it ends the keying, and frees the
// channel and any request for missing fragments that was waiting it out.
void TextMessagingProtocol::handleLocatorLocked(const Frame& frame, uint64_t nowMs)
{
    LocatorPeer& peer = locatorPeerLocked(frame.originCallsign, nowMs);
    peer.heardTheirs = true;
    if (peer.gridSquare != frame.locator || peer.support != LocatorSupport::Yes)
    {
        peer.gridSquare = frame.locator;
        peer.support = LocatorSupport::Yes;
        saveLocatorPeerLocked(frame.originCallsign, peer);
    }

    if (frame.burstsFollowing != 0) return;
    if (channelReservedBy_ == frame.originCallsign)
    {
        channelReservedUntilMs_ = 0;
        channelReservedBy_.clear();
    }
    for (auto& entry : inbox_)
    {
        if (entry.first.first != frame.originCallsign) continue;
        if (entry.second.keyingEndsMs > nowMs) entry.second.keyingEndsMs = nowMs;
    }
}

void TextMessagingProtocol::saveLocatorPeerLocked(const std::string& callsign, const LocatorPeer& peer)
{
    StationLocator station;
    station.callsign = callsign;
    station.gridSquare = peer.gridSquare;
    station.support = peer.support;
    station.updated = wallClock_();
    store_.upsertStationLocator(station);
}

void TextMessagingProtocol::setTransmitInhibited(const std::string& reason)
{
    std::vector<PendingEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inhibitReason_ = reason;
        if (!inhibitReason_.empty()) discardQueuedLocked(events);
    }

    deliver(events);
}

std::string TextMessagingProtocol::transmitInhibitedReason() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return inhibitReason_;
}

void TextMessagingProtocol::abortTransmission()
{
    std::vector<PendingEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dropOutboxLocked(MessageStatus::Aborted, true, events);
    }

    deliver(events);
}

uint64_t TextMessagingProtocol::holdTransmissions()
{
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t nowMs = monotonicMs_();
    operatorHoldUntilMs_ = std::max(nowMs, operatorHoldUntilMs_) + (uint64_t)timing_.frameAirMs;
    return operatorHoldUntilMs_ - nowMs;
}

// Everything waiting for the transmitter, dropped. A chat line says it was
// not sent; an acknowledgement or pong has no line and simply goes.
void TextMessagingProtocol::discardQueuedLocked(std::vector<PendingEvent>& events)
{
    dropOutboxLocked(MessageStatus::NotSent, false, events);
}

// Drops what is waiting for the transmitter and, with everything, what is on
// the air and what is waiting for its acknowledgement too, leaving each chat
// line with the given status.
void TextMessagingProtocol::dropOutboxLocked(MessageStatus status, bool everything,
                                             std::vector<PendingEvent>& events)
{
    for (auto it = outbox_.begin(); it != outbox_.end();)
    {
        bool drop = everything || it->state == TransmissionState::Queued;
        if (!drop)
        {
            ++it;
            continue;
        }

        updateStatusLocked(*it, status, events);
        it = outbox_.erase(it);
    }
}

AirTiming AirTiming::forFrameSeconds(double frameSeconds, int bytesPerFrame,
                                     double decodeLatencySeconds, double replyFrameSeconds,
                                     double replyDecodeLatencySeconds, double chordSeconds,
                                     double replyChordSeconds, double closingChordSeconds,
                                     double answerSensedSeconds)
{
    AirTiming timing;
    if (frameSeconds <= 0.0 || bytesPerFrame <= 0) return timing;

    auto ms = [](double seconds) { return (int)std::lround(seconds * 1000.0); };
    auto frames = [bytesPerFrame](int bytes) { return (bytes + bytesPerFrame - 1) / bytesPerFrame; };

    // A Glissando keying opens and closes with a chord, which is air time
    // but no data.
    if (closingChordSeconds < 0.0) closingChordSeconds = chordSeconds;
    double chordsAir = chordSeconds + closingChordSeconds;
    double signallingAir = frames(SIGNALLING_FRAME_BYTES) * frameSeconds + chordsAir;
    double textAir = frames(TEXT_FRAME_BYTES) * frameSeconds + chordsAir;
    timing.frameAirMs = ms(frameSeconds);

    // The far end hears a burst of ours only once its first frame has been
    // decoded, a frame and a search after it began (after the opening
    // chord), and we hear its answer the same way; the codec2 waits stay as
    // they are on top of that.
    double seen = chordSeconds + frameSeconds + decodeLatencySeconds;

    // With the far end's tempo given, answers are waited for as it sends
    // them. It decodes our last frame, turns around, and having heard us on
    // the channel draws its jitter (TURNAROUND_JITTER plus half its own
    // frame) before keying. The first frame of its answer can be decoded here
    // one frame and one search after that, and nothing of it can be sensed
    // before then. On the air a ping at Presto duet gave up 29 s after it
    // ended, and the far end's Allegro answer was first heard 9 s later.
    bool farEndGiven = replyFrameSeconds > 0.0;
    auto farEndKeysAfter = [&](double farFrameSeconds) {
        return ms(decodeLatencySeconds + farFrameSeconds / 2.0) + TURNAROUND_AFTER_RX_MILLISECONDS +
               TURNAROUND_JITTER_MILLISECONDS;
    };
    int firstReplyFrame =
        farEndGiven ? farEndKeysAfter(replyFrameSeconds) +
                          ms(replyChordSeconds + replyFrameSeconds + replyDecodeLatencySeconds)
                    : 0;

    // Where the start of an answer can be heard, as the far end keys,
    // the window need only last until then.
    timing.replyWindowMs =
        farEndGiven && answerSensedSeconds > 0.0
            ? REPLY_WINDOW_MILLISECONDS + farEndKeysAfter(replyFrameSeconds) + ms(answerSensedSeconds)
            : REPLY_WINDOW_MILLISECONDS + std::max(ms(seen), firstReplyFrame);
    timing.turnaroundJitterMs = TURNAROUND_JITTER_MILLISECONDS + ms(frameSeconds / 2.0);
    timing.textFragmentAirMs = ms(textAir) + TEXT_FRAGMENT_AIR_MILLISECONDS;
    timing.signallingFollowedReservationMs = 2 * timing.textFragmentAirMs;
    timing.maxChannelBusyMs = std::max(MAX_CHANNEL_BUSY_MILLISECONDS,
                                       (MAX_FRAGMENTS_PER_MESSAGE + 1) * timing.textFragmentAirMs);

    // From the end of our burst: the far end decodes it, turns around, sends
    // a signalling burst, and we decode that.
    int answer = ms(2.0 * decodeLatencySeconds + signallingAir) + TURNAROUND_AFTER_RX_MILLISECONDS;
    if (farEndGiven)
    {
        // An answer at our own tempo, jitter and all; or the first frame of
        // one at the slowest, after which the channel is busy and the
        // protocol holds its timers until the rest has arrived.
        int ownTempo = farEndKeysAfter(frameSeconds) + ms(signallingAir + decodeLatencySeconds);
        answer = std::max(ownTempo, firstReplyFrame);
    }
    timing.ackTimeoutMs = ACK_TIMEOUT_MILLISECONDS + answer;
    timing.pingTimeoutMs = PING_TIMEOUT_MILLISECONDS + answer;
    timing.retryBackoffMs = RETRY_BACKOFF_MILLISECONDS + ms(frameSeconds / 2.0);
    timing.reassemblyTimeoutMs = std::max(
        REASSEMBLY_TIMEOUT_MILLISECONDS,
        3 * (MAX_FRAGMENTS_PER_MESSAGE + 1) * timing.textFragmentAirMs / 2 + timing.ackTimeoutMs);
    return timing;
}

void TextMessagingProtocol::setAirTiming(const AirTiming& timing)
{
    std::lock_guard<std::mutex> lock(mutex_);
    timing_ = timing;
}

AirTiming TextMessagingProtocol::airTiming() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return timing_;
}

void TextMessagingProtocol::setClocks(std::function<uint64_t()> monotonicMs,
                                      std::function<std::time_t()> wallClock)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (monotonicMs) monotonicMs_ = monotonicMs;
    if (wallClock) wallClock_ = wallClock;
}

size_t TextMessagingProtocol::pendingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return outbox_.size();
}

uint16_t TextMessagingProtocol::nextAirIdLocked()
{
    // Zero is reserved so that an all zero header cannot look like a valid ID.
    if (nextAirId_ == 0 || nextAirId_ > MAX_AIR_ID) nextAirId_ = 1;
    return nextAirId_++;
}

Frame TextMessagingProtocol::makeFrameLocked(FrameType type, const std::string& destination,
                                             uint16_t airId, uint8_t fragmentIndex,
                                             uint8_t fragmentCount,
                                             const std::vector<uint8_t>& payload) const
{
    Frame frame;
    frame.type = type;
    frame.destinationCrc = destination.empty() ? 0 : FrameCodec::callsignHash(destination);
    frame.originCallsign = myCallsign_;
    frame.airId = airId;
    frame.fragmentIndex = fragmentIndex;
    frame.fragmentCount = fragmentCount;
    frame.payload = payload;
    return frame;
}

bool TextMessagingProtocol::isAddressedToMeLocked(const Frame& frame) const
{
    // Addressing is by hash, so a collision could in principle hand us somebody
    // else's frame; the origin callsign in the frame is what gets displayed,
    // so the worst case is a stray line in the chat window.
    return !myCallsign_.empty() && frame.destinationCrc == myCallsignCrc_;
}

bool TextMessagingProtocol::sendMessage(const std::string& text, const std::string& destination,
                                        std::string& errorOut)
{
    std::vector<PendingEvent> events;
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ok = queueMessageLocked(text, destination, errorOut, events);
    }

    deliver(events);
    return ok;
}

bool TextMessagingProtocol::queueMessageLocked(const std::string& text,
                                               const std::string& destination,
                                               std::string& errorOut,
                                               std::vector<PendingEvent>& events)
{
    if (myCallsign_.empty())
    {
        errorOut = "Set your callsign in Tools/Options before sending messages.";
        return false;
    }

    if (transport_ == nullptr)
    {
        errorOut = "FreeDV is not running, so there is nothing to transmit with.";
        return false;
    }

    if (!inhibitReason_.empty())
    {
        errorOut = inhibitReason_;
        return false;
    }

    std::string body = trim(text);
    if (body.empty())
    {
        errorOut = "There is nothing to send.";
        return false;
    }

    if ((int)body.size() > MAX_MESSAGE_TEXT_BYTES)
    {
        errorOut = "Message is too long; the limit is " +
                   std::to_string(MAX_MESSAGE_TEXT_BYTES) + " characters.";
        return false;
    }

    std::string normalizedDestination = FrameCodec::normalizeCallsign(destination);
    bool broadcast = normalizedDestination.empty();
    uint16_t airId = nextAirIdLocked();

    // Encode before storing, so a message that cannot go on the air never
    // appears in the chat window as something that was sent.
    PendingTransmission pending;
    pending.mode = BurstMode::Text;
    pending.expectsAck = expectsAckFromLocked(normalizedDestination);
    pending.isPing = false;
    pending.destination = normalizedDestination;
    pending.state = TransmissionState::Queued;

    // Cut the text where each fragment fills: the header is the same for
    // all, and the text is Huffman coded, so how much fits depends on it.
    std::vector<std::string> chunks;
    for (size_t at = 0; at < body.size();)
    {
        size_t fits = FrameCodec::textThatFits(myCallsign_, body, at);
        if (fits == 0 || chunks.size() == MAX_FRAGMENTS_PER_MESSAGE)
        {
            errorOut = "Message is too long to send in one go; please shorten it.";
            return false;
        }
        chunks.push_back(body.substr(at, fits));
        at += fits;
    }

    size_t fragmentCount = chunks.size();
    for (size_t index = 0; index < fragmentCount; index++)
    {
        std::vector<uint8_t> payload(chunks[index].begin(), chunks[index].end());

        Frame frame = makeFrameLocked(broadcast ? FrameType::Broadcast : FrameType::Message,
                                      normalizedDestination, airId, (uint8_t)index,
                                      (uint8_t)fragmentCount, payload);

        // Checked now, as the whole message would be sent, so that a keying
        // built later from any subset of these fragments cannot fail to encode.
        frame.burstsFollowing = (uint8_t)(fragmentCount - 1 - index);
        if (FrameCodec::encode(frame, TEXT_FRAME_BYTES).empty())
        {
            errorOut = "Could not encode the message for transmission.";
            return false;
        }

        pending.frames.push_back(frame);
    }

    TextMessage message;
    message.kind = MessageKind::Chat;
    message.airId = airId;
    message.originCallsign = myCallsign_;
    message.destCallsign = normalizedDestination;
    message.broadcast = broadcast;
    message.text = body;
    message.timestamp = wallClock_();
    message.direction = MessageDirection::Sent;
    message.status = MessageStatus::Queued;

    if (!store_.addMessage(message))
    {
        errorOut = "Could not save the message: " + store_.lastError();
        return false;
    }

    pending.message = message;
    outbox_.push_back(pending);

    PendingEvent event;
    event.type = PendingEvent::Type::MessageAdded;
    event.message = message;
    events.push_back(event);

    return true;
}

bool TextMessagingProtocol::sendPing(const std::string& destination, std::string& errorOut)
{
    std::vector<PendingEvent> events;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (myCallsign_.empty())
        {
            errorOut = "Set your callsign in Tools/Options before pinging.";
            return false;
        }

        if (transport_ == nullptr)
        {
            errorOut = "FreeDV is not running, so there is nothing to transmit with.";
            return false;
        }

        if (!inhibitReason_.empty())
        {
            errorOut = inhibitReason_;
            return false;
        }

        std::string normalizedDestination = FrameCodec::normalizeCallsign(destination);
        if (normalizedDestination.empty())
        {
            errorOut = "Select a station to ping.";
            return false;
        }

        uint16_t airId = nextAirIdLocked();
        Frame frame = makeFrameLocked(FrameType::Ping, normalizedDestination, airId, 0, 1, {});
        std::vector<uint8_t> encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
        if (encoded.empty())
        {
            errorOut = "Could not encode the ping for transmission.";
            return false;
        }

        TextMessage message;
        message.kind = MessageKind::System;
        message.airId = airId;
        message.originCallsign = myCallsign_;
        message.destCallsign = normalizedDestination;
        message.text = myCallsign_ + " >> " + normalizedDestination + " : PING!";
        message.timestamp = wallClock_();
        message.direction = MessageDirection::Sent;
        message.status = MessageStatus::Queued;

        if (!store_.addMessage(message))
        {
            errorOut = "Could not save the ping: " + store_.lastError();
            return false;
        }

        PendingTransmission pending;
        pending.message = message;
        pending.frames.push_back(frame);
        pending.mode = BurstMode::Signalling;
        pending.expectsAck = true;
        pending.isPing = true;
        pending.destination = normalizedDestination;
        pending.state = TransmissionState::Queued;
        outbox_.push_back(pending);

        PendingEvent event;
        event.type = PendingEvent::Type::MessageAdded;
        event.message = message;
        events.push_back(event);
    }

    deliver(events);
    return true;
}

void TextMessagingProtocol::queueAckLocked(const std::string& destination, uint16_t airId)
{
    // The whole message is in, so a request for part of it still waiting to
    // go out has been overtaken.
    for (auto it = outbox_.begin(); it != outbox_.end();)
    {
        bool overtaken = it->partialAck && it->state == TransmissionState::Queued &&
                         it->destination == destination && it->message.airId == airId;
        it = overtaken ? outbox_.erase(it) : std::next(it);
    }

    // A retransmission of a message we already have arrives one fragment at a
    // time, and each duplicate fragment asks for the acknowledgement again.
    // One queued acknowledgement answers all of them; eight would hold the
    // channel for eight bursts saying the same thing.
    for (const PendingTransmission& queued : outbox_)
    {
        if (queued.ack && queued.state == TransmissionState::Queued &&
            queued.destination == destination && queued.message.airId == airId)
        {
            return;
        }
    }

    Frame frame = makeFrameLocked(FrameType::MessageAck, destination, airId, 0, 1, {});
    std::vector<uint8_t> encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
    if (encoded.empty()) return;

    PendingTransmission pending;
    pending.frames.push_back(frame);
    pending.mode = BurstMode::Signalling;
    pending.expectsAck = false;
    pending.reply = true;
    pending.ack = true;
    pending.message.airId = airId;
    pending.destination = destination;
    pending.state = TransmissionState::Queued;

    // Acknowledgements go to the front: the sender is sitting on a timer.
    outbox_.push_front(pending);
}

// Tells the sender which fragments arrived, so that it resends only the rest.
// A newer report for the same message replaces one still waiting to go out.
void TextMessagingProtocol::queuePartialAckLocked(const std::string& destination, uint16_t airId,
                                                  uint32_t received)
{
    std::vector<uint8_t> payload{(uint8_t)received};
    Frame frame = makeFrameLocked(FrameType::MessagePartialAck, destination, airId, 0, 1, payload);
    std::vector<uint8_t> encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
    if (encoded.empty()) return;

    for (PendingTransmission& queued : outbox_)
    {
        if (queued.partialAck && queued.state == TransmissionState::Queued &&
            queued.destination == destination && queued.message.airId == airId)
        {
            queued.frames.assign(1, frame);
            return;
        }
    }

    PendingTransmission pending;
    pending.frames.push_back(frame);
    pending.mode = BurstMode::Signalling;
    pending.expectsAck = false;
    pending.reply = true;
    pending.partialAck = true;
    pending.message.airId = airId;
    pending.destination = destination;
    pending.state = TransmissionState::Queued;

    // Like an acknowledgement: the sender is sitting on a timer.
    outbox_.push_front(pending);
}

// A message addressed to us, heard in part during a keying that is now over,
// asks the sender for the fragments still missing. One heard nothing in the
// keying says nothing: the sender's timer covers that, as it always has.
void TextMessagingProtocol::requestMissingFragmentsLocked(uint64_t nowMs)
{
    for (auto& entry : inbox_)
    {
        Reassembly& reassembly = entry.second;
        if (reassembly.broadcast || !reassembly.heardThisKeying) continue;
        if (nowMs < reassembly.keyingEndsMs) continue;

        reassembly.heardThisKeying = false;
        if (autoReplyEnabled_)
        {
            queuePartialAckLocked(entry.first.first, entry.first.second, reassembly.receivedMask);
        }
    }
}

void TextMessagingProtocol::queuePongLocked(const std::string& destination, float snr)
{
    std::vector<uint8_t> payload{encodeSnr(snr)};
    Frame frame = makeFrameLocked(FrameType::PingAck, destination, nextAirIdLocked(), 0, 1, payload);
    std::vector<uint8_t> encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
    if (encoded.empty()) return;

    PendingTransmission pending;
    pending.frames.push_back(frame);
    pending.mode = BurstMode::Signalling;
    pending.expectsAck = false;
    pending.reply = true;
    pending.destination = destination;
    pending.state = TransmissionState::Queued;
    outbox_.push_front(pending);
}

void TextMessagingProtocol::addSystemMessageLocked(const std::string& text,
                                                   const std::string& destination,
                                                   std::vector<PendingEvent>& events)
{
    TextMessage message;
    message.kind = MessageKind::System;
    message.originCallsign = myCallsign_;
    message.destCallsign = destination;
    message.text = text;
    message.timestamp = wallClock_();
    message.direction = MessageDirection::Received;
    message.status = MessageStatus::Received;

    if (!store_.addMessage(message)) return;

    PendingEvent event;
    event.type = PendingEvent::Type::MessageAdded;
    event.message = message;
    events.push_back(event);
}

void TextMessagingProtocol::updateStatusLocked(PendingTransmission& pending, MessageStatus status,
                                               std::vector<PendingEvent>& events)
{
    // Acknowledgements and pongs have no chat line of their own.
    if (pending.message.id == 0) return;

    pending.message.status = status;
    pending.message.retryCount = pending.retries;
    pending.message.fragmentCount =
        pending.mode == BurstMode::Text ? (int)pending.frames.size() : 0;
    pending.message.fragmentsConfirmed = std::popcount(pending.confirmed);
    store_.updateMessageStatus(pending.message.id, status, pending.retries);

    PendingEvent event;
    event.type = PendingEvent::Type::MessageUpdated;
    event.message = pending.message;
    events.push_back(event);
}

void TextMessagingProtocol::onFrameReceived(const Frame& frame, float snr)
{
    std::vector<PendingEvent> events;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Our own transmission looped back through the radio's monitor path.
        if (!myCallsign_.empty() && frame.originCallsign == myCallsign_) return;

        // The station we just heard is turning its receiver back on; keying
        // straight away talks over it.
        deferTransmissionLocked(monotonicMs_(), timing_.turnaroundAfterRxMs, 0);

        // A station we answered is sending, so it is not holding the channel
        // for the rest of our keying, however much of it it lost.
        if (frame.originCallsign == answeredStation_)
        {
            answeredHoldUntilMs_ = 0;
            answeredStation_.clear();
        }

        std::time_t now = wallClock_();
        stations_.heard(frame.originCallsign, snr, now);

        HeardStation station;
        station.callsign = frame.originCallsign;
        station.snr = snr;
        station.lastHeard = now;
        store_.upsertHeardStation(station);

        PendingEvent stationsChanged;
        stationsChanged.type = PendingEvent::Type::StationsChanged;
        events.push_back(stationsChanged);

        // Whoever the frame is for, its sender holds the channel for the rest
        // of its keying.
        uint64_t nowMs = monotonicMs_();
        reserveChannelForKeyingLocked(frame, nowMs);

        // Whoever it is for, a frame keeps its sender's contact going, and a
        // ping or acknowledgement says whether it takes in locators.
        locatorPeerLocked(frame.originCallsign, nowMs);
        noteLocatorFeaturesLocked(frame, nowMs);

        // Messages, broadcasts and pings say whether their sender acknowledges
        // by itself. An acknowledgement or a pong to us is that station doing so.
        switch (frame.type)
        {
            case FrameType::Message:
            case FrameType::Broadcast:
            case FrameType::Ping:
                noteStationAutoAckLocked(frame.originCallsign, frame.senderAutoAck, events);
                break;
            case FrameType::MessageAck:
            case FrameType::MessagePartialAck:
            case FrameType::PingAck:
                if (isAddressedToMeLocked(frame))
                {
                    noteStationAutoAckLocked(frame.originCallsign, true, events);
                }
                break;
            case FrameType::Locator:
                break;
        }

        switch (frame.type)
        {
            case FrameType::Broadcast:
                handleIncomingFragmentLocked(frame, snr, events);
                break;
            case FrameType::Message:
                if (isAddressedToMeLocked(frame)) handleIncomingFragmentLocked(frame, snr, events);
                break;
            case FrameType::MessageAck:
                if (isAddressedToMeLocked(frame)) handleAckLocked(frame, events);
                break;
            case FrameType::MessagePartialAck:
                if (isAddressedToMeLocked(frame)) handlePartialAckLocked(frame, events);
                break;
            case FrameType::Ping:
                if (isAddressedToMeLocked(frame)) handlePingLocked(frame, snr, events);
                break;
            case FrameType::PingAck:
                if (isAddressedToMeLocked(frame)) handlePongLocked(frame, snr, events);
                break;
            case FrameType::Locator:
                handleLocatorLocked(frame, nowMs);
                break;
        }

        noteMapStationLocked(frame.originCallsign);
    }

    deliver(events);
}

// Every frame says how many bursts its sender still has to send in this
// keying; see TEXT_FRAGMENT_AIR_MILLISECONDS. A text frame gives the count,
// so it sets the reservation exactly, and the last burst of a keying releases
// it. That also covers a keying that is not a whole message, such as a resend
// of the fragments a receiver was missing, which "fragment k of n" could not.
void TextMessagingProtocol::reserveChannelForKeyingLocked(const Frame& frame, uint64_t nowMs)
{
    if (FrameCodec::isSignallingFrameType(frame.type))
    {
        // Only "more follows", and only ever lengthens a reservation.
        if (frame.burstsFollowing == 0) return;

        uint64_t until = nowMs + (uint64_t)timing_.signallingFollowedReservationMs;
        if (until > channelReservedUntilMs_) channelReservedUntilMs_ = until;
        return;
    }

    channelReservedUntilMs_ =
        frame.burstsFollowing == 0
            ? 0
            : nowMs + (uint64_t)frame.burstsFollowing * (uint64_t)timing_.textFragmentAirMs;
    channelReservedBy_ = frame.burstsFollowing == 0 ? std::string() : frame.originCallsign;
}

void TextMessagingProtocol::handleIncomingFragmentLocked(const Frame& frame, float snr,
                                                         std::vector<PendingEvent>& events)
{
    bool broadcast = frame.type == FrameType::Broadcast;
    ReassemblyKey key(frame.originCallsign, frame.airId);
    uint64_t nowMs = monotonicMs_();

    // The sender is retransmitting a message we already have, which means our
    // acknowledgement did not reach them. Send it again rather than showing
    // the message twice. A retransmission carries the same fragments; a
    // fragment that differs under the same ID is a new message from a sender
    // whose IDs started over, and is taken in as one.
    auto completed = recentlyCompleted_.find(key);
    if (completed != recentlyCompleted_.end())
    {
        const std::vector<std::string>& fragments = completed->second.fragments;
        bool sameMessage =
            fragments.size() == frame.fragmentCount &&
            fragments[frame.fragmentIndex] ==
                std::string(frame.payload.begin(), frame.payload.end());

        if (sameMessage)
        {
            if (!broadcast && autoReplyEnabled_) queueAckLocked(frame.originCallsign, frame.airId);
            return;
        }

        recentlyCompleted_.erase(completed);
    }

    Reassembly& reassembly = inbox_[key];
    if (reassembly.fragmentCount != frame.fragmentCount)
    {
        reassembly.fragments.assign(frame.fragmentCount, "");
        reassembly.fragmentCount = frame.fragmentCount;
        reassembly.receivedMask = 0;
        reassembly.snr = snr;
        reassembly.broadcast = broadcast;
    }

    reassembly.fragments[frame.fragmentIndex].assign(frame.payload.begin(), frame.payload.end());
    reassembly.receivedMask |= (1u << frame.fragmentIndex);
    reassembly.lastHeardMs = nowMs;
    reassembly.heardThisKeying = true;
    reassembly.keyingEndsMs =
        nowMs + (uint64_t)frame.burstsFollowing * (uint64_t)timing_.textFragmentAirMs;
    reassembly.snr = (reassembly.snr + snr) / 2.0f;

    uint32_t completeMask = (1u << frame.fragmentCount) - 1u;
    if (reassembly.receivedMask != completeMask) return;

    TextMessage message;
    message.kind = MessageKind::Chat;
    message.airId = frame.airId;
    message.originCallsign = frame.originCallsign;
    message.destCallsign = broadcast ? "" : myCallsign_;
    message.broadcast = broadcast;
    for (const std::string& fragment : reassembly.fragments) message.text += fragment;
    message.timestamp = wallClock_();
    message.direction = MessageDirection::Received;
    message.status = MessageStatus::Received;
    message.snr = reassembly.snr;

    Completed& done = recentlyCompleted_[key];
    done.atMs = nowMs;
    done.fragments = reassembly.fragments;
    inbox_.erase(key);

    if (store_.addMessage(message))
    {
        PendingEvent event;
        event.type = PendingEvent::Type::MessageAdded;
        event.message = message;
        events.push_back(event);
    }

    if (!broadcast && autoReplyEnabled_) queueAckLocked(frame.originCallsign, frame.airId);
}

void TextMessagingProtocol::handleAckLocked(const Frame& frame, std::vector<PendingEvent>& events)
{
    // The acknowledged message is not necessarily at the head: an incoming
    // message's own acknowledgement jumps the queue ahead of it.
    for (auto it = outbox_.begin(); it != outbox_.end(); ++it)
    {
        if (it->isPing || !it->expectsAck) continue;
        if (it->message.airId != frame.airId) continue;
        if (it->destination != frame.originCallsign) continue;

        // The whole message is there now, however much of it a partial
        // acknowledgement had confirmed; the window read "OK 7/8" without this.
        it->confirmed = (1u << it->frames.size()) - 1u;
        updateStatusLocked(*it, MessageStatus::Acknowledged, events);
        outbox_.erase(it);
        return;
    }
}

// The far end has part of a message of ours. Whatever it confirms is never
// sent again. A report that brings news resends the rest at once and costs no
// retry: a message getting through piece by piece is worth finishing. One
// that brings none means the last keying delivered nothing new, and counts
// as a retry just as a timeout does.
void TextMessagingProtocol::handlePartialAckLocked(const Frame& frame,
                                                   std::vector<PendingEvent>& events)
{
    if (frame.payload.size() != 1) return;

    for (size_t i = 0; i < outbox_.size(); i++)
    {
        PendingTransmission& pending = outbox_[i];
        if (pending.isPing || !pending.expectsAck || pending.mode != BurstMode::Text) continue;
        if (pending.message.airId != frame.airId) continue;
        if (pending.destination != frame.originCallsign) continue;

        uint32_t all = (1u << pending.frames.size()) - 1u;
        uint32_t received = frame.payload[0] & all;
        bool progress = (received & ~pending.confirmed) != 0;
        pending.confirmed |= received;

        if (pending.confirmed == all)
        {
            updateStatusLocked(pending, MessageStatus::Acknowledged, events);
            outbox_.erase(outbox_.begin() + (std::ptrdiff_t)i);
            return;
        }

        // Mid keying the report can only have been meant for an earlier one;
        // what it confirms is recorded and the keying carries on.
        if (pending.state == TransmissionState::Transmitting) return;

        if (progress)
        {
            pending.state = TransmissionState::Queued;
            pending.notBeforeMs = 0;

            // No new status: the message is still waiting on the far end. But
            // the window shows how far it got, and that has changed.
            updateStatusLocked(pending, pending.message.status, events);
            return;
        }

        if (pending.state == TransmissionState::AwaitingAck)
        {
            retryOrFailLocked(i, monotonicMs_(), events);
        }
        return;
    }
}

void TextMessagingProtocol::handlePingLocked(const Frame& frame, float snr,
                                             std::vector<PendingEvent>& events)
{
    addSystemMessageLocked(frame.originCallsign + " >> " + myCallsign_ + " : PING!",
                           frame.originCallsign, events);

    if (autoReplyEnabled_) queuePongLocked(frame.originCallsign, snr);
}

void TextMessagingProtocol::handlePongLocked(const Frame& frame, float snr,
                                             std::vector<PendingEvent>& events)
{
    std::string heardBy;
    if (!frame.payload.empty())
    {
        heardBy = ", heard you at " + formatSnr(decodeSnr(frame.payload[0])) + " dB";
    }

    addSystemMessageLocked(frame.originCallsign + " >> " + myCallsign_ + " : PONG! (" +
                               formatSnr(snr) + " dB" + heardBy + ")",
                           frame.originCallsign, events);

    for (auto it = outbox_.begin(); it != outbox_.end(); ++it)
    {
        if (!it->isPing) continue;
        if (it->destination != frame.originCallsign) continue;

        updateStatusLocked(*it, MessageStatus::Acknowledged, events);
        outbox_.erase(it);
        return;
    }
}

void TextMessagingProtocol::purgeStaleReassembliesLocked(uint64_t nowMs)
{
    for (auto it = inbox_.begin(); it != inbox_.end();)
    {
        if (nowMs - it->second.lastHeardMs > (uint64_t)timing_.reassemblyTimeoutMs)
        {
            it = inbox_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = recentlyCompleted_.begin(); it != recentlyCompleted_.end();)
    {
        if (nowMs - it->second.atMs > DUPLICATE_WINDOW_MS)
        {
            it = recentlyCompleted_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

AckWait TextMessagingProtocol::ackWait() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (const PendingTransmission& pending : outbox_)
    {
        if (!pending.expectsAck) continue;

        // Nothing is outstanding until it has actually been sent once. A
        // message still waiting its turn is queued, not awaited, and saying
        // otherwise would overwrite the notice that says so.
        if (pending.state != TransmissionState::AwaitingAck && pending.retries == 0 &&
            pending.confirmed == 0)
        {
            continue;
        }

        return pending.isPing ? AckWait::Ping : AckWait::Message;
    }

    return AckWait::Nothing;
}

bool TextMessagingProtocol::isTransmitting() const
{
    ITextMessagingTransport* transport = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_;
    }

    // Asked outside the lock: the transport reaches into the audio pipeline,
    // which has no business waiting on the protocol's mutex.
    return transport != nullptr && transport->isTransmitting();
}

bool TextMessagingProtocol::hasQueuedTransmissions() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(outbox_.begin(), outbox_.end(), [](const PendingTransmission& pending)
                       { return pending.state == TransmissionState::Queued; });
}

TextMessagingProtocol::Cancel TextMessagingProtocol::cancelForLocked(const PendingTransmission& pending) const
{
    // Replies have no line of their own in the chat to cancel from; a
    // message or a ping of ours does.
    if (pending.reply || pending.message.id == 0) return Cancel::None;
    if (!pending.isPing && pending.message.kind != MessageKind::Chat) return Cancel::None;

    // Not yet on the air at all: no retry, and no fragment confirmed.
    bool untouched = pending.state == TransmissionState::Queued && pending.retries == 0 && pending.confirmed == 0;
    return untouched ? Cancel::Remove : Cancel::Abort;
}

TextMessagingProtocol::Cancel TextMessagingProtocol::cancelFor(int64_t messageId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const PendingTransmission& pending : outbox_)
    {
        if (pending.message.id == messageId) return cancelForLocked(pending);
    }
    return Cancel::None;
}

TextMessagingProtocol::Cancel TextMessagingProtocol::cancelMessage(int64_t messageId, bool* onAirOut)
{
    if (onAirOut != nullptr) *onAirOut = false;

    Cancel done = Cancel::None;
    std::vector<PendingEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = outbox_.begin(); it != outbox_.end(); ++it)
        {
            if (it->message.id != messageId) continue;

            done = cancelForLocked(*it);
            if (done == Cancel::None) break;

            if (onAirOut != nullptr) *onAirOut = it->state == TransmissionState::Transmitting;
            updateStatusLocked(*it, done == Cancel::Remove ? MessageStatus::NotSent : MessageStatus::Aborted, events);
            outbox_.erase(it);
            break;
        }
    }

    deliver(events);
    return done;
}

std::vector<int64_t> TextMessagingProtocol::outstandingMessageIds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int64_t> ids;
    for (const PendingTransmission& pending : outbox_)
    {
        if (cancelForLocked(pending) != Cancel::None) ids.push_back(pending.message.id);
    }
    return ids;
}

bool TextMessagingProtocol::isMessageQueued(int64_t messageId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(outbox_.begin(), outbox_.end(), [&](const PendingTransmission& pending)
                       { return pending.state == TransmissionState::Queued && pending.message.id == messageId; });
}

// The air time of an entry's next keying: a burst per frame not yet
// confirmed. Each is counted as long as a text fragment, which a reply is
// not, and a keying's chords are counted once per burst; a little long,
// which is the side to err on for a countdown.
uint64_t TextMessagingProtocol::airTimeLocked(const PendingTransmission& pending) const
{
    uint64_t bursts = 0;
    for (size_t index = 0; index < pending.frames.size(); index++)
    {
        if ((pending.confirmed & (1u << index)) == 0) bursts++;
    }
    uint64_t airMs = bursts * (uint64_t)timing_.textFragmentAirMs;
    if (pending.gear != 0 && transport_ != nullptr)
    {
        airMs = (uint64_t)std::llround((double)airMs * transport_->airTimeScale(pending.gear));
    }
    return airMs;
}

std::vector<QueuedWait> TextMessagingProtocol::queuedWaits() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<QueuedWait> waits;
    uint64_t nowMs = monotonicMs_();
    uint64_t turnaround = (uint64_t)timing_.turnaroundAfterTxMs;
    uint64_t window = (uint64_t)timing_.replyWindowMs;

    // The keying on the air, and after it the turnaround and, if it asked
    // anything or answered anybody, the far end's turn.
    uint64_t at = nowMs;
    bool onAir = false;
    bool owesTurn = false;
    for (const PendingTransmission& pending : outbox_)
    {
        if (pending.state != TransmissionState::Transmitting) continue;
        onAir = true;
        if (pending.expectsAck || pending.reply) owesTurn = true;
    }
    if (onAir)
    {
        uint64_t end = std::max(nowMs, keyingEndsMs_);
        at = end + turnaround + (owesTurn ? window : 0);
    }
    at = std::max(at, quietUntilLocked(false));

    // Then the queue in order, each entry its air time and the far end's
    // turn after it. A reply goes ahead of our own traffic, which then gives
    // the answered station its turn.
    for (const PendingTransmission& pending : outbox_)
    {
        if (pending.state != TransmissionState::Queued) continue;

        uint64_t start = std::max(at, pending.notBeforeMs);
        if (!pending.reply && !pending.isPing && pending.message.kind == MessageKind::Chat)
        {
            QueuedWait wait;
            wait.messageId = pending.message.id;
            wait.waitMs = (int64_t)(start - nowMs);
            wait.channelBusy = channelBusy_;
            wait.gear = pending.gear;
            waits.push_back(wait);
        }

        at = start + airTimeLocked(pending) + turnaround +
             (pending.expectsAck || pending.reply ? window : 0);
    }

    return waits;
}

bool TextMessagingProtocol::setMessageTempo(int64_t messageId, int gear)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (PendingTransmission& pending : outbox_)
    {
        if (pending.message.id != messageId) continue;
        if (pending.isPing || pending.message.kind != MessageKind::Chat) return false;
        if (cancelForLocked(pending) != Cancel::Remove) return false;

        pending.gear = gear;
        return true;
    }
    return false;
}

// When the transmitter may next be used. Beyond the plain turnaround, a
// message we have sent and not yet had answered buys the far end room to
// answer it: it waits out its own turnaround first and then sends a whole
// burst, none of which we can hear while keyed. The window disappears by
// itself when the acknowledgement arrives, because the entry goes with it.
//
// A reply we owe -- an acknowledgement or a pong -- waits out the turnarounds
// but not that window. The station we are answering has just finished a burst
// and is waiting on a window of exactly the same length before it keys again,
// so a reply held for our own window goes out at the very moment that
// station's window expires, and the two collide. The bench showed it: a pong
// held five seconds keyed one second before the far end's next message. The
// cost is that a reply to one station can key while a slow acknowledgement
// from another is just starting; carrier sense covers that once the other
// burst is more than a second old. Nor does a reply wait out the turn we give
// a station we have just answered, or the wait for it to let go of the
// channel: those are for keyings of our own.
uint64_t TextMessagingProtocol::quietUntilLocked(bool forReply) const
{
    // The operator's "Woah!" holds everything, replies too: whoever they
    // can hear would be stomped on by an acknowledgement just the same.
    uint64_t quietUntil = std::max(quietUntilMs_, operatorHoldUntilMs_);
    if (forReply) return quietUntil;

    if (ownTrafficQuietUntilMs_ > quietUntil) quietUntil = ownTrafficQuietUntilMs_;
    if (answeredHoldUntilMs_ > quietUntil) quietUntil = answeredHoldUntilMs_;

    for (const PendingTransmission& pending : outbox_)
    {
        if (pending.state != TransmissionState::AwaitingAck) continue;

        uint64_t replyBy = pending.sentAtMs + (uint64_t)timing_.replyWindowMs;
        if (replyBy > quietUntil) quietUntil = replyBy;
    }

    return quietUntil;
}

// Whether the queue is frozen because somebody else has the channel: the
// receiver says so, or a fragmented message we are part way through hearing
// has more bursts to come. A spell that outlasts any real transmission is a
// receiver false triggering, not traffic, and is ignored until it clears so
// it cannot silence us for good.
bool TextMessagingProtocol::channelFrozenLocked(uint64_t nowMs)
{
    bool busy = (transport_ != nullptr && transport_->isChannelBusy()) ||
                nowMs < channelReservedUntilMs_;

    if (!busy)
    {
        // Everybody who heard that burst comes unfrozen at the same instant,
        // and two of them keying together cannot sense each other. A random
        // moment's pause spreads them out.
        if (channelBusy_) deferTransmissionLocked(nowMs, 0, timing_.turnaroundJitterMs);
        channelBusy_ = false;
        return false;
    }

    if (!channelBusy_)
    {
        channelBusy_ = true;
        channelBusySinceMs_ = nowMs;
    }

    return nowMs - channelBusySinceMs_ < (uint64_t)timing_.maxChannelBusyMs;
}

// Time spent frozen does not count against anything outstanding. The
// acknowledgement deadline moves back so nothing retries or gives up while the
// reply could not have reached us, and so does the start of the reply window:
// when a third station's burst ends, both ends of our exchange come unfrozen
// together, and the far end is owed its turn to answer before we key again.
void TextMessagingProtocol::holdTimersLocked(uint64_t pausedMs)
{
    for (PendingTransmission& pending : outbox_)
    {
        if (pending.state != TransmissionState::AwaitingAck) continue;

        pending.deadlineMs += pausedMs;
        pending.sentAtMs += pausedMs;
    }
}

void TextMessagingProtocol::deferTransmissionLocked(uint64_t fromMs, int baseMs, int jitterMs)
{
    uint64_t until = fromMs + (uint64_t)baseMs + randomDelayLocked(jitterMs);
    if (until > quietUntilMs_) quietUntilMs_ = until;
}

// A delay in [0, maxMs] from the station's own sequence, so any two stations
// draw differently and no draw repeats the last.
uint32_t TextMessagingProtocol::randomDelayLocked(int maxMs)
{
    if (maxMs <= 0) return 0;

    jitterState_ = jitterState_ * 1103515245u + 12345u;
    return (jitterState_ >> 16) % (uint32_t)(maxMs + 1);
}

void TextMessagingProtocol::tick()
{
    std::vector<PendingEvent> events;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        uint64_t nowMs = monotonicMs_();
        purgeStaleReassembliesLocked(nowMs);
        requestMissingFragmentsLocked(nowMs);

        // Replies queued as frames arrive, retries whose timers ran out and
        // resends that a partial acknowledgement asked for all land in the
        // queue; while inhibited none of them may go out.
        if (!inhibitReason_.empty()) discardQueuedLocked(events);

        // While the channel is busy everything waits, timers included: the far
        // end cannot answer us through somebody else's burst, and it may be the
        // answer itself that is coming in. Nor through one of ours: it hears
        // us keyed and holds its answer until we stop. At Adagio an
        // acknowledgement we owed a third station keyed for two minutes
        // while a ping waited for its pong, and the ping gave up before the
        // far end could have answered it.
        bool frozen = channelFrozenLocked(nowMs);
        channelHeld_ = frozen;
        bool keyed = transport_ != nullptr && transport_->isTransmitting();
        if ((frozen || keyed) && lastTickMs_ != 0 && nowMs > lastTickMs_) holdTimersLocked(nowMs - lastTickMs_);
        lastTickMs_ = nowMs;

        if (transport_ != nullptr && !outbox_.empty()) serviceOutboxLocked(nowMs, frozen, events);
    }

    deliver(events);
}

void TextMessagingProtocol::serviceOutboxLocked(uint64_t nowMs, bool frozen,
                                                std::vector<PendingEvent>& events)
{
    // Voice always wins the transmitter, and only one burst is on the air at a
    // time. A transmission parked on its acknowledgement timer does not count:
    // the transmitter is idle for the whole of that wait, so the traffic queued
    // behind it keeps moving, and handleAckLocked matches an acknowledgement to
    // any entry rather than just the first.
    if (!transport_->isTransmitting())
    {
        // The keying we handed to the transport has finished, with everything
        // that was in it: a reply and the message that rode behind it both.
        // Acknowledgements push to the front, so neither is necessarily first.
        for (size_t i = 0; i < outbox_.size();)
        {
            PendingTransmission& sent = outbox_[i];
            if (sent.state != TransmissionState::Transmitting)
            {
                i++;
                continue;
            }

            deferTransmissionLocked(nowMs, timing_.turnaroundAfterTxMs,
                                    timing_.turnaroundJitterMs);

            // Having answered somebody, give them the channel before keying
            // anything of our own: see the note on REPLY_WINDOW_MILLISECONDS.
            // Their turn starts when they stop holding the channel for us,
            // which can be later than our unkeying: if they lost what rode
            // behind the reply, not until their reservation runs out. That
            // is a wait apart, because it is over as soon as they are heard
            // again, as they nearly always are: answering the message that
            // rode along. Counted from the unkeying alone, at Presto it held
            // every message of ours back for over two minutes after a pong
            // with a message behind it, and at Adagio for over ten minutes.
            if (sent.reply)
            {
                uint64_t jitter = randomDelayLocked(timing_.turnaroundJitterMs);
                uint64_t turn = nowMs + (uint64_t)timing_.replyWindowMs + jitter;
                if (turn > ownTrafficQuietUntilMs_) ownTrafficQuietUntilMs_ = turn;

                if (keyingHeldUntilMs_ > nowMs)
                {
                    answeredHoldUntilMs_ = keyingHeldUntilMs_ + (uint64_t)timing_.replyWindowMs + jitter;
                    answeredStation_ = sent.destination;
                }
            }

            sent.sentAtMs = nowMs;

            if (sent.expectsAck)
            {
                sent.state = TransmissionState::AwaitingAck;
                sent.deadlineMs = nowMs + (uint64_t)(sent.isPing ? timing_.pingTimeoutMs
                                                                 : timing_.ackTimeoutMs);
                if (keyingCarriesLocator_) sent.deadlineMs += (uint64_t)timing_.textFragmentAirMs;
                updateStatusLocked(sent, MessageStatus::AwaitingAck, events);
                i++;
            }
            else
            {
                updateStatusLocked(sent, MessageStatus::Sent, events);
                outbox_.erase(outbox_.begin() + (std::ptrdiff_t)i);
            }
        }

        // The turnaround is the far end's turn; anything queued waits it out.
        // Only the start of a burst is held back -- the acknowledgement timers
        // below keep running, so a retry is never late because of it. An entry
        // still backing off from a retry steps aside for whatever is behind it.
        if (!frozen)
        {
            for (size_t i = 0; i < outbox_.size(); i++)
            {
                PendingTransmission& next = outbox_[i];
                if (next.state != TransmissionState::Queued) continue;
                if (nowMs < next.notBeforeMs) continue;
                if (nowMs < quietUntilLocked(next.reply)) continue;

                std::vector<PendingTransmission*> entries{&next};
                if (next.reply)
                {
                    PendingTransmission* rider = riderLocked(i, nowMs);
                    if (rider != nullptr) entries.push_back(rider);
                }

                std::string locatorTo;
                std::vector<OutgoingBurst> keying = keyingBurstsLocked(
                    std::vector<const PendingTransmission*>(entries.begin(), entries.end()), nowMs,
                    &locatorTo);
                if (!keying.empty() && transport_->transmit(keying))
                {
                    keyingCarriesLocator_ = !locatorTo.empty();
                    if (keyingCarriesLocator_) locatorPeers_[locatorTo].sent = true;
                    for (const OutgoingBurst& burst : keying)
                    {
                        if (burst.destination.empty()) continue;
                        noteMapStationLocked(burst.destination);
                        break;
                    }

                    // A reply with something behind it says more follows, and a
                    // listener that then loses what follows holds the channel
                    // for two fragments after the reply, however short the
                    // keying turns out. The reply burst is shorter than a text
                    // fragment, so a fragment's air time bounds it.
                    keyingHeldUntilMs_ =
                        entries.size() > 1 && entries[0]->mode == BurstMode::Signalling
                            ? nowMs + (uint64_t)timing_.textFragmentAirMs +
                                  (uint64_t)timing_.signallingFollowedReservationMs
                            : 0;

                    keyingEndsMs_ = nowMs;
                    for (PendingTransmission* entry : entries) keyingEndsMs_ += airTimeLocked(*entry);
                    if (keyingCarriesLocator_) keyingEndsMs_ += (uint64_t)timing_.textFragmentAirMs;

                    for (PendingTransmission* entry : entries)
                    {
                        entry->state = TransmissionState::Transmitting;
                        updateStatusLocked(*entry, MessageStatus::Transmitting, events);
                    }
                }
                break;
            }
        }
    }

    // Expired acknowledgement timers. They stand still while anybody, us
    // included, has the channel (see tick()), so one runs out only after
    // time the far end could have answered in.
    for (size_t i = 0; i < outbox_.size();)
    {
        PendingTransmission& waiting = outbox_[i];
        if (waiting.state != TransmissionState::AwaitingAck || nowMs < waiting.deadlineMs)
        {
            i++;
            continue;
        }

        if (!retryOrFailLocked(i, nowMs, events)) i++;
    }
}

// An attempt that got nothing through. A ping gets one chance; a message gets
// the retries the operator can see counting up in the chat window, and goes
// back to the queue to wait its turn like any other transmission. Returns true
// if the entry was removed from the outbox.
bool TextMessagingProtocol::retryOrFailLocked(size_t index, uint64_t nowMs,
                                              std::vector<PendingEvent>& events)
{
    PendingTransmission& waiting = outbox_[index];

    // It went out; the station has since said it will not answer, so another
    // try would only get the same silence.
    if (!waiting.isPing && !expectsAckFromLocked(waiting.destination))
    {
        updateStatusLocked(waiting, MessageStatus::Sent, events);
        outbox_.erase(outbox_.begin() + (std::ptrdiff_t)index);
        return true;
    }

    if (!waiting.isPing && waiting.retries < MAX_MESSAGE_RETRIES)
    {
        waiting.retries++;
        waiting.state = TransmissionState::Queued;
        waiting.notBeforeMs = nowMs + randomDelayLocked(timing_.retryBackoffMs * waiting.retries);
        updateStatusLocked(waiting, MessageStatus::Retrying, events);
        return false;
    }

    if (waiting.isPing)
    {
        addSystemMessageLocked(waiting.destination +
                                   (expectsAckFromLocked(waiting.destination)
                                        ? " : no response to PING"
                                        : " : no response to PING (its Auto ACK is off)"),
                               waiting.destination, events);
    }

    updateStatusLocked(waiting, MessageStatus::Failed, events);
    outbox_.erase(outbox_.begin() + (std::ptrdiff_t)index);
    return true;
}

// A transmission of our own to send behind the reply at replyIndex, in the
// same keying, or null. Without this a station with a message queued only
// ever acknowledged while the other side had traffic, because every reply
// gives the answered station the channel: it was starved for as long as the
// other station kept typing. With it the two alternate a message each. One
// rider at most, or a station with a long queue would hold the channel as
// long as it liked. It need not wait out our own quiet time: that is there
// to keep us from starting a keying, and this one has already started, with
// every listener told how long it will last. A retry still backing off
// waits, and so does anything already on its way or waiting on an answer.
TextMessagingProtocol::PendingTransmission* TextMessagingProtocol::riderLocked(size_t replyIndex,
                                                                              uint64_t nowMs)
{
    for (size_t i = 0; i < outbox_.size(); i++)
    {
        if (i == replyIndex) continue;

        PendingTransmission& candidate = outbox_[i];
        if (candidate.reply || candidate.state != TransmissionState::Queued) continue;
        if (nowMs < candidate.notBeforeMs) continue;
        // A keying has one tempo, the reply's, so a message moved to a
        // tempo of its own keys on its own, and nothing jumps ahead of it.
        if (candidate.gear != 0) return nullptr;

        return &candidate;
    }

    return nullptr;
}

// The bursts for one keying: every frame each entry still has to send, in
// order, each saying how many bursts follow it in the keying as a whole. A
// message leaves out the fragments the far end has confirmed. Our locator
// rides last behind a message to a station that is due it, and that
// station's callsign goes in locatorToOut; see locatorRidesToLocked().
std::vector<OutgoingBurst> TextMessagingProtocol::keyingBurstsLocked(
    const std::vector<const PendingTransmission*>& entries, uint64_t nowMs, std::string* locatorToOut)
{
    std::vector<std::pair<const PendingTransmission*, const Frame*>> order;
    int gear = 0;
    std::string locatorTo;
    for (const PendingTransmission* entry : entries)
    {
        if (gear == 0) gear = entry->gear;
        for (size_t index = 0; index < entry->frames.size(); index++)
        {
            if ((entry->confirmed & (1u << index)) != 0) continue;
            order.push_back({entry, &entry->frames[index]});
        }

        bool directedMessage = !entry->reply && !entry->isPing && entry->mode == BurstMode::Text;
        if (directedMessage && locatorTo.empty() && locatorRidesToLocked(entry->destination, nowMs))
        {
            locatorTo = entry->destination;
        }
    }
    if (locatorToOut != nullptr) *locatorToOut = locatorTo;

    size_t bursts = order.size() + (locatorTo.empty() ? 0 : 1);
    std::vector<OutgoingBurst> keying;
    for (size_t position = 0; position < order.size(); position++)
    {
        const PendingTransmission* entry = order[position].first;
        BurstMode mode = entry->mode;
        Frame frame = *order[position].second;
        frame.burstsFollowing = (uint8_t)(bursts - 1 - position);
        frame.senderAutoAck = autoReplyEnabled_;
        if (FrameCodec::carriesFeatures(frame.type)) frame.features = featuresForLocked(frame, entry->destination);

        // Every frame encoded when it was queued, and a keying holds at most a
        // reply, one message and a locator, so the count after any fragment
        // still fits: this does not fail.
        std::vector<uint8_t> encoded = FrameCodec::encode(
            frame, mode == BurstMode::Signalling ? SIGNALLING_FRAME_BYTES : TEXT_FRAME_BYTES);
        if (encoded.empty()) return {};

        keying.push_back({mode, encoded, gear, entry->destination, {}});
    }

    if (!locatorTo.empty())
    {
        std::vector<uint8_t> encoded = FrameCodec::encode(locatorFrameLocked(locatorTo), SIGNALLING_FRAME_BYTES);
        if (!encoded.empty()) keying.push_back({BurstMode::Signalling, encoded, gear, locatorTo, {}});
    }

    // Whatever the keying, a duet with a voice to spare sings our locator in
    // it: it costs no air time, and builds that do not know it skip a filler
    // unread. One segment holds it only from a standard callsign.
    if (!keying.empty() && !myLocator_.empty() && sendLocator_ && FrameCodec::isStandardCallsign(myCallsign_))
    {
        keying.back().duetFiller = FrameCodec::encode(locatorFrameLocked(entries.front()->destination),
                                                      SIGNALLING_FRAME_BYTES);
    }

    return keying;
}

void TextMessagingProtocol::deliver(const std::vector<PendingEvent>& events)
{
    ITextMessagingObserver* observer = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        observer = observer_;
    }

    if (observer == nullptr) return;

    for (const PendingEvent& event : events)
    {
        switch (event.type)
        {
            case PendingEvent::Type::MessageAdded:
                observer->onMessageAdded(event.message);
                break;
            case PendingEvent::Type::MessageUpdated:
                observer->onMessageUpdated(event.message);
                break;
            case PendingEvent::Type::StationsChanged:
                observer->onStationsChanged();
                break;
        }
    }
}

} // namespace TextMessaging
