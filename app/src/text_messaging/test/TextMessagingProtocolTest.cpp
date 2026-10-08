//=========================================================================
// Name:            TextMessagingProtocolTest.cpp
// Purpose:         Exercises the chat state machine against a fake radio.
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

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "../DeliveryChip.h"
#include "../FrameCodec.h"
#include "../HeardStationList.h"
#include "../MessageStore.h"
#include "../TextMessagingProtocol.h"

using namespace TextMessaging;

namespace
{

int failures = 0;

// How many of character c fill a text fragment from a standard callsign.
size_t perFragment(char c)
{
    return FrameCodec::textThatFits("W1AW", std::string(400, c), 0);
}

void check(bool condition, const char* what, int line)
{
    if (!condition)
    {
        failures++;
        fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// A radio that records what it was asked to send and finishes transmitting
// only when the test says so.
class FakeTransport : public ITextMessagingTransport
{
public:
    bool transmit(const std::vector<OutgoingBurst>& bursts) override
    {
        if (refuse) return false;

        transmissions.emplace_back();
        modes.emplace_back();
        gears.emplace_back();
        fillers.push_back(bursts.empty() ? std::vector<uint8_t>() : bursts.back().duetFiller);
        for (const OutgoingBurst& burst : bursts)
        {
            transmissions.back().push_back(burst.frame);
            modes.back().push_back(burst.mode);
            gears.back().push_back(burst.gear);
        }
        transmitting = true;
        return true;
    }

    bool isTransmitting() const override { return transmitting || voiceActive; }
    bool isChannelBusy() const override { return channelBusy; }
    double airTimeScale(int gear) const override { return gear == 1 ? 8.0 : 1.0; }

    // A link that acknowledges by itself, such as a Data2G session, to the
    // stations in reliableTo; nothing by default, as for our own modem.
    bool deliversReliablyTo(const std::string& destination) const override
    {
        return reliableTo.count(destination) != 0;
    }

    bool transmitReliably(const std::vector<OutgoingBurst>& bursts, uint64_t keyingId) override
    {
        if (bursts.empty() || !deliversReliablyTo(bursts.front().destination)) return false;
        reliableKeyings.push_back(keyingId);
        reliable.emplace_back();
        for (const OutgoingBurst& burst : bursts) reliable.back().push_back(burst.frame);
        return true;
    }

    std::vector<KeyingReport> takeKeyingReports() override
    {
        std::vector<KeyingReport> taken;
        taken.swap(reports);
        return taken;
    }

    // Lets go of stations only when releases is set, as Data2G does; our own
    // modem keeps the default, which does nothing.
    bool releaseStation(const std::string& destination) override
    {
        if (!releases) return ITextMessagingTransport::releaseStation(destination);
        released.push_back(destination);
        return true;
    }
    bool releases = false;
    std::vector<std::string> released;

    std::vector<std::vector<std::vector<uint8_t>>> transmissions; // frames, per keying
    std::vector<std::vector<BurstMode>> modes;                    // and their modes
    std::vector<std::vector<int>> gears;                          // and tempos
    std::vector<std::vector<uint8_t>> fillers;                    // and duet fillers
    bool transmitting = false;
    bool voiceActive = false;
    bool channelBusy = false;
    bool refuse = false;

    std::set<std::string> reliableTo;
    std::vector<uint64_t> reliableKeyings;                    // given to transmitReliably(), in order
    std::vector<std::vector<std::vector<uint8_t>>> reliable;  // and their frames
    std::vector<KeyingReport> reports;                        // for takeKeyingReports() to hand over
};

class RecordingObserver : public ITextMessagingObserver
{
public:
    void onMessageAdded(const TextMessage& message) override { added.push_back(message); }
    void onMessageUpdated(const TextMessage& message) override { updated.push_back(message); }
    void onStationsChanged() override { stationsChanged++; }

    const TextMessage* lastUpdateFor(int64_t id) const
    {
        for (auto it = updated.rbegin(); it != updated.rend(); ++it)
        {
            if (it->id == id) return &(*it);
        }
        return nullptr;
    }

    std::vector<TextMessage> added;
    std::vector<TextMessage> updated;
    int stationsChanged = 0;
};

// Wraps everything a test station needs, with clocks the test drives.
struct Station
{
    explicit Station(const std::string& callsign)
        : protocol(store, stations)
    {
        check(store.open(":memory:"), "store.open", __LINE__);
        protocol.setTransport(&transport);
        protocol.setObserver(&observer);
        protocol.setMyCallsign(callsign);
        protocol.setClocks([this]() { return nowMs; }, [this]() { return (std::time_t)1750000000; });
    }

    // Runs the protocol far enough to put a queued transmission on the air and
    // see it through to the end of the over. The clock skips past any
    // turnaround or retry backoff first, since a real station would simply
    // have waited.
    void completeOneTransmission()
    {
        nowMs += std::max(MAX_TURNAROUND_MILLISECONDS, MAX_RETRY_BACKOFF_MILLISECONDS) + 1;
        protocol.tick(); // hands the burst to the transport
        transport.transmitting = false;
        protocol.tick(); // the over has finished
    }

    // Decodes every frame of the most recent transmission into this station.
    void receiveFrom(FakeTransport& other, float snr = 5.0f)
    {
        CHECK(!other.transmissions.empty());
        for (const std::vector<uint8_t>& raw : other.transmissions.back())
        {
            Frame frame;
            CHECK(FrameCodec::decode(raw.data(), (int)raw.size(), frame));
            protocol.onFrameReceived(frame, snr);
        }
    }

    MessageStore store;
    HeardStationList stations;
    FakeTransport transport;
    RecordingObserver observer;
    TextMessagingProtocol protocol;
    uint64_t nowMs = 1000;
};

Frame decodeOne(const std::vector<uint8_t>& raw)
{
    Frame frame;
    CHECK(FrameCodec::decode(raw.data(), (int)raw.size(), frame));
    return frame;
}

bool everUpdatedTo(const RecordingObserver& observer, int64_t id, MessageStatus status)
{
    for (const TextMessage& update : observer.updated)
    {
        if (update.id == id && update.status == status) return true;
    }
    return false;
}

// Whether the log has a line of the protocol's own saying this.
bool hasSystemLine(const RecordingObserver& observer, const std::string& text)
{
    for (const TextMessage& message : observer.added)
    {
        if (message.kind == MessageKind::System && message.text.find(text) != std::string::npos)
            return true;
    }
    return false;
}

void testAddressedMessageIsAcknowledged()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("Hello over there", "VK3ABC", error));
    CHECK(error.empty());
    CHECK(sender.observer.added.size() == 1);
    CHECK(sender.observer.added[0].status == MessageStatus::Queued);
    CHECK(sender.observer.added[0].destCallsign == "VK3ABC");
    CHECK(!sender.observer.added[0].broadcast);

    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == 1);
    CHECK(sender.transport.transmissions[0].size() == 1); // short message, one fragment
    CHECK(sender.transport.modes[0][0] == BurstMode::Text);          // text goes in the wider mode

    int64_t messageId = sender.observer.added[0].id;
    const TextMessage* update = sender.observer.lastUpdateFor(messageId);
    CHECK(update != nullptr && update->status == MessageStatus::AwaitingAck);

    // The receiver decodes it, shows it, and queues an acknowledgement.
    receiver.receiveFrom(sender.transport, 6.5f);
    CHECK(receiver.observer.added.size() == 1);
    CHECK(receiver.observer.added[0].text == "Hello over there");
    CHECK(receiver.observer.added[0].originCallsign == "W1AW");
    CHECK(receiver.observer.added[0].direction == MessageDirection::Received);
    CHECK(receiver.observer.stationsChanged > 0);
    CHECK(receiver.stations.contains("W1AW"));
    CHECK(receiver.protocol.pendingCount() == 1);

    receiver.completeOneTransmission();
    CHECK(receiver.transport.modes[0][0] == BurstMode::Signalling); // acknowledgements are signalling
    CHECK(receiver.protocol.pendingCount() == 0);

    sender.receiveFrom(receiver.transport);
    update = sender.observer.lastUpdateFor(messageId);
    CHECK(update != nullptr && update->status == MessageStatus::Acknowledged);
    CHECK(sender.protocol.pendingCount() == 0);
}

void testRetriesThenFails()
{
    Station sender("W1AW");

    std::string error;
    CHECK(sender.protocol.sendMessage("Anybody there", "VK3ABC", error));
    int64_t messageId = sender.observer.added[0].id;

    for (int attempt = 0; attempt <= MAX_MESSAGE_RETRIES; attempt++)
    {
        sender.completeOneTransmission();
        CHECK((int)sender.transport.transmissions.size() == attempt + 1);

        // Nothing comes back before the timer expires.
        sender.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
        sender.protocol.tick();

        const TextMessage* update = sender.observer.lastUpdateFor(messageId);
        CHECK(update != nullptr);
        if (attempt < MAX_MESSAGE_RETRIES)
        {
            CHECK(update->status == MessageStatus::Retrying);
            CHECK(update->retryCount == attempt + 1);
        }
        else
        {
            CHECK(update->status == MessageStatus::Failed);
        }
    }

    CHECK((int)sender.transport.transmissions.size() == MAX_MESSAGE_RETRIES + 1);
    CHECK(sender.protocol.pendingCount() == 0);
}

void testBroadcastIsNotAcknowledged()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("CQ CQ from the chat window", "", error));
    CHECK(sender.observer.added[0].broadcast);

    sender.completeOneTransmission();

    int64_t messageId = sender.observer.added[0].id;
    const TextMessage* update = sender.observer.lastUpdateFor(messageId);
    CHECK(update != nullptr && update->status == MessageStatus::Sent);
    CHECK(sender.protocol.pendingCount() == 0);

    // Any station decodes a broadcast, and none of them answer it.
    receiver.receiveFrom(sender.transport);
    CHECK(receiver.observer.added.size() == 1);
    CHECK(receiver.observer.added[0].broadcast);
    CHECK(receiver.protocol.pendingCount() == 0);
}

void testMessageNotForUsIsIgnored()
{
    Station sender("W1AW");
    Station bystander("DJ2LS");

    std::string error;
    CHECK(sender.protocol.sendMessage("Private note", "VK3ABC", error));
    sender.completeOneTransmission();

    bystander.receiveFrom(sender.transport);

    // The station is still worth listing as heard, but the text is not ours.
    CHECK(bystander.stations.contains("W1AW"));
    CHECK(bystander.observer.added.empty());
    CHECK(bystander.protocol.pendingCount() == 0);
}

void testLongMessageIsFragmentedAndReassembled()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string body(perFragment('A') * 2 + 10, 'A');
    std::string error;
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions[0].size() == 3);

    receiver.receiveFrom(sender.transport);
    CHECK(receiver.observer.added.size() == 1);
    CHECK(receiver.observer.added[0].text == body);

    std::string tooLong(MAX_MESSAGE_TEXT_BYTES + 1, 'B');
    CHECK(!sender.protocol.sendMessage(tooLong, "VK3ABC", error));
    CHECK(!error.empty());
}

void testRetransmissionIsNotShownTwice()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("Say again", "VK3ABC", error));
    sender.completeOneTransmission();

    receiver.receiveFrom(sender.transport);
    CHECK(receiver.observer.added.size() == 1);
    receiver.completeOneTransmission(); // first acknowledgement

    // The acknowledgement was lost, so the sender transmits the same message
    // again: the receiver must re-acknowledge without duplicating the line.
    sender.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
    sender.protocol.tick();
    sender.completeOneTransmission();

    receiver.receiveFrom(sender.transport);
    CHECK(receiver.observer.added.size() == 1);
    CHECK(receiver.protocol.pendingCount() == 1);
}

void testPingAndPong()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendPing("VK3ABC", error));
    CHECK(sender.observer.added.size() == 1);
    CHECK(sender.observer.added[0].kind == MessageKind::System);
    CHECK(sender.observer.added[0].text == "W1AW >> VK3ABC : PING!");

    sender.completeOneTransmission();
    CHECK(sender.transport.modes[0][0] == BurstMode::Signalling);

    receiver.receiveFrom(sender.transport, 8.0f);
    CHECK(receiver.observer.added.size() == 1);
    CHECK(receiver.observer.added[0].text == "W1AW >> VK3ABC : PING!");
    CHECK(receiver.protocol.pendingCount() == 1); // the pong

    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport, 4.0f);

    CHECK(sender.observer.added.size() == 2);
    const TextMessage& pong = sender.observer.added[1];
    CHECK(pong.kind == MessageKind::System);
    CHECK(pong.text.find("VK3ABC >> W1AW : PONG!") == 0);
    CHECK(pong.text.find("heard you at 8.0 dB") != std::string::npos);
    CHECK(sender.protocol.pendingCount() == 0);
}

// Glissando timings: Allegro frames are 13.76 s and Adagio 55.04 s; the
// receiver reports a frame a quarter frame and a second after it ends.
constexpr double ALLEGRO_FRAME = 13.76;
constexpr double ADAGIO_FRAME = 55.04;
double glissandoLatency(double frame) { return frame / 4.0 + 1.0; }

// Given the slowest tempo an answer could come in, the waits cover an answer
// at our own tempo with the far end's turnaround jitter, and never end before
// the first frame of an answer at the slowest tempo could have been decoded.
// Without it (as Data2G calls it) the timings are as they were.
void testAirTimingWaitsForTheSlowestAnswer()
{
    auto ms = [](double seconds) { return (int)std::lround(seconds * 1000.0); };
    auto keysAfter = [&](double ourLatency, double farFrame) {
        return ms(ourLatency + farFrame / 2.0) + TURNAROUND_AFTER_RX_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS;
    };

    AirTiming own = AirTiming::forFrameSeconds(ALLEGRO_FRAME, 9, glissandoLatency(ALLEGRO_FRAME));
    CHECK(own.ackTimeoutMs == 52900); // unchanged, as logged on the air
    CHECK(own.pingTimeoutMs == 52900);

    double allegroLatency = glissandoLatency(ALLEGRO_FRAME);
    double adagioLatency = glissandoLatency(ADAGIO_FRAME);
    AirTiming mixed = AirTiming::forFrameSeconds(ALLEGRO_FRAME, 9, allegroLatency, ADAGIO_FRAME, adagioLatency);
    int firstAdagioFrame = keysAfter(allegroLatency, ADAGIO_FRAME) + ms(ADAGIO_FRAME + adagioLatency);
    CHECK(mixed.ackTimeoutMs == ACK_TIMEOUT_MILLISECONDS + firstAdagioFrame);
    CHECK(mixed.pingTimeoutMs == PING_TIMEOUT_MILLISECONDS + firstAdagioFrame);
    CHECK(mixed.replyWindowMs == REPLY_WINDOW_MILLISECONDS + firstAdagioFrame);
    CHECK(mixed.ackTimeoutMs > 115000); // 53 s before

    // At Adagio a whole answer at our tempo, with the far end's jitter, is
    // the longer wait: two frames of signalling after up to 29.5 s of it.
    AirTiming adagio = AirTiming::forFrameSeconds(ADAGIO_FRAME, 9, adagioLatency, ADAGIO_FRAME, adagioLatency);
    int adagioAnswer = keysAfter(adagioLatency, ADAGIO_FRAME) + ms(2 * ADAGIO_FRAME + adagioLatency);
    CHECK(adagio.ackTimeoutMs == ACK_TIMEOUT_MILLISECONDS + adagioAnswer);
    CHECK(adagio.ackTimeoutMs > 156100); // what it was, which the jitter could outrun
}

// Glissando's opening and closing chords (one bar, 4 of a frame's 86
// symbols) are air time: every burst is two chords longer, and an answer's
// first frame is heard one chord later.
void testAirTimingCountsTheChords()
{
    auto ms = [](double seconds) { return (int)std::lround(seconds * 1000.0); };
    const double allegroChord = ALLEGRO_FRAME * 4.0 / 86.0;
    const double adagioChord = ADAGIO_FRAME * 4.0 / 86.0;
    double allegroLatency = glissandoLatency(ALLEGRO_FRAME);
    double adagioLatency = glissandoLatency(ADAGIO_FRAME);
    AirTiming bare = AirTiming::forFrameSeconds(ALLEGRO_FRAME, 9, allegroLatency, ADAGIO_FRAME, adagioLatency);
    AirTiming chords = AirTiming::forFrameSeconds(ALLEGRO_FRAME, 9, allegroLatency, ADAGIO_FRAME, adagioLatency,
                                                  allegroChord, adagioChord);
    CHECK(std::abs(chords.textFragmentAirMs - bare.textFragmentAirMs - ms(2.0 * allegroChord)) <= 1);
    CHECK(std::abs(chords.ackTimeoutMs - bare.ackTimeoutMs - ms(adagioChord)) <= 1);
    CHECK(std::abs(chords.replyWindowMs - bare.replyWindowMs - ms(adagioChord)) <= 1);
}

// Where an answer is heard by its opening chord, the reply window ends once
// the chord would have been heard, not once the answer's first frame could
// decode; the timeouts still wait for that frame.
void testReplyWindowEndsAtTheAnswersChord()
{
    auto ms = [](double seconds) { return (int)std::lround(seconds * 1000.0); };
    auto keysAfter = [&](double ourLatency, double farFrame) {
        return ms(ourLatency + farFrame / 2.0) + TURNAROUND_AFTER_RX_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS;
    };
    const double prestoFrame = 6.88;
    const double prestoLatency = glissandoLatency(prestoFrame);
    const double adagioLatency = glissandoLatency(ADAGIO_FRAME);
    const double sensed = 1.6;

    AirTiming frames = AirTiming::forFrameSeconds(prestoFrame, 9, prestoLatency, ADAGIO_FRAME, adagioLatency,
                                                  0.6, 2.56);
    AirTiming chord = AirTiming::forFrameSeconds(prestoFrame, 9, prestoLatency, ADAGIO_FRAME, adagioLatency,
                                                 0.6, 2.56, -1.0, sensed);
    CHECK(chord.replyWindowMs == REPLY_WINDOW_MILLISECONDS + keysAfter(prestoLatency, ADAGIO_FRAME) + ms(sensed));
    CHECK(chord.replyWindowMs < frames.replyWindowMs - 50000); // an Adagio frame and more
    CHECK(chord.ackTimeoutMs == frames.ackTimeoutMs);
    CHECK(chord.pingTimeoutMs == frames.pingTimeoutMs);

    // Presto answering Presto: 16 s instead of 25.
    AirTiming presto = AirTiming::forFrameSeconds(prestoFrame, 9, prestoLatency, prestoFrame, prestoLatency,
                                                  0.6, 0.6, -1.0, sensed);
    CHECK(presto.replyWindowMs > 15000 && presto.replyWindowMs < 17000);

    // Data2G passes no far end, and keeps its window.
    AirTiming data2g = AirTiming::forFrameSeconds(2.0, 64, 1.0, 0.0, 0.0, 0.0, 0.0, -1.0, sensed);
    CHECK(data2g.replyWindowMs == AirTiming::forFrameSeconds(2.0, 64, 1.0).replyWindowMs);
}

// A ping sent at Allegro while listening at every tempo, answered at Adagio:
// the answer's first frame is heard 80 s after the ping ended, well past the
// 53 s an Allegro answer takes, and the channel stays busy until the pong is
// complete. The ping succeeds instead of giving up while the answer arrives.
void testPingWaitsForAnAnswerAtASlowerTempo()
{
    AirTiming timing = AirTiming::forFrameSeconds(ALLEGRO_FRAME, 9, glissandoLatency(ALLEGRO_FRAME),
                                                  ADAGIO_FRAME, glissandoLatency(ADAGIO_FRAME));
    Station sender("W1AW");
    Station receiver("VK3ABC");
    sender.protocol.setAirTiming(timing);
    receiver.protocol.setAirTiming(timing);

    std::string error;
    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.completeOneTransmission();
    receiver.receiveFrom(sender.transport, 8.0f);

    for (int s = 0; s < 80; s++)
    {
        sender.nowMs += 1000;
        sender.protocol.tick();
    }
    CHECK(sender.protocol.pendingCount() == 1);

    sender.transport.channelBusy = true; // the answer's first frame decodes
    for (int s = 0; s < 70; s++)
    {
        sender.nowMs += 1000;
        sender.protocol.tick();
    }
    CHECK(sender.protocol.pendingCount() == 1);

    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport, 4.0f);
    sender.transport.channelBusy = false;
    sender.protocol.tick();

    CHECK(sender.protocol.pendingCount() == 0);
    bool noResponse = false;
    for (const TextMessage& message : sender.observer.added)
    {
        if (message.text.find("no response") != std::string::npos) noResponse = true;
    }
    CHECK(!noResponse);
    CHECK(sender.observer.added.back().text.find("VK3ABC >> W1AW : PONG!") == 0);
}

// The far end cannot answer while we are keyed: it hears us and waits. A
// ping waiting for its pong while we send an acknowledgement owed to a
// third station must not spend its wait on that keying; at Adagio the
// acknowledgement alone took two minutes and the ping always gave up.
void testOwnKeyingHoldsTheWaitForAnAnswer()
{
    Station sender("W1AW");
    Station third("K6ABC");

    std::string error;
    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.completeOneTransmission();

    CHECK(third.protocol.sendMessage("Hello W1AW", "W1AW", error));
    third.completeOneTransmission();
    sender.receiveFrom(third.transport);

    // The acknowledgement keys as soon as the turnaround allows.
    uint64_t pingEnded = sender.nowMs;
    while (sender.transport.transmissions.size() < 2 && sender.nowMs - pingEnded < PING_TIMEOUT_MILLISECONDS)
    {
        sender.nowMs += 250;
        sender.protocol.tick();
    }
    CHECK(sender.transport.transmissions.size() == 2);
    uint64_t waited = sender.nowMs - pingEnded;

    // A long keying, many times the ping's timeout.
    for (int i = 0; i < 40; i++)
    {
        sender.nowMs += PING_TIMEOUT_MILLISECONDS / 4;
        sender.protocol.tick();
    }
    CHECK(!hasSystemLine(sender.observer, "no response to PING"));

    // Once we stop, the rest of the wait runs, and then it gives up.
    sender.transport.transmitting = false;
    sender.protocol.tick();
    sender.nowMs += PING_TIMEOUT_MILLISECONDS - waited - 1000;
    sender.protocol.tick();
    CHECK(!hasSystemLine(sender.observer, "no response to PING"));
    sender.nowMs += 2000;
    sender.protocol.tick();
    CHECK(hasSystemLine(sender.observer, "no response to PING"));
}

void testPingTimesOut()
{
    Station sender("W1AW");

    std::string error;
    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.completeOneTransmission();

    sender.nowMs += PING_TIMEOUT_MILLISECONDS + 1;
    sender.protocol.tick();

    // One transmission only: a ping is not worth retrying behind the
    // operator's back.
    CHECK(sender.transport.transmissions.size() == 1);
    CHECK(sender.observer.added.size() == 2);
    CHECK(sender.observer.added[1].text.find("no response to PING") != std::string::npos);
    CHECK(sender.protocol.pendingCount() == 0);
}

void testAutoReplyCanBeDisabled()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");
    receiver.protocol.setAutoReplyEnabled(false);
    CHECK(!receiver.protocol.autoReplyEnabled());

    std::string error;
    CHECK(sender.protocol.sendMessage("Are you listening", "VK3ABC", error));
    sender.completeOneTransmission();

    receiver.receiveFrom(sender.transport);
    CHECK(receiver.observer.added.size() == 1); // the message still shows
    CHECK(receiver.protocol.pendingCount() == 0); // but nothing is transmitted

    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.completeOneTransmission();
    receiver.receiveFrom(sender.transport);
    CHECK(receiver.observer.added.size() == 2);
    CHECK(receiver.protocol.pendingCount() == 0);
}

// A station with Auto acknowledge off says so on everything it sends, and a
// message to it goes once: no acknowledgement is coming, so it ends as sent
// rather than retrying and failing. Hearing it say otherwise turns retries
// back on.
void testStationWithAutoAckOffIsNotRetried()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");
    receiver.protocol.setAutoReplyEnabled(false);
    CHECK(sender.protocol.stationAutoAcks("VK3ABC"));

    std::string error;
    CHECK(receiver.protocol.sendMessage("Listening only", "W1AW", error));
    receiver.completeOneTransmission();
    CHECK((receiver.transport.transmissions[0][0][0] >> 4) == 0xB); // a message, Auto ACK off
    sender.receiveFrom(receiver.transport);
    CHECK(!sender.protocol.stationAutoAcks("vk3abc"));
    CHECK(hasSystemLine(sender.observer, "VK3ABC : Auto ACK off"));
    sender.completeOneTransmission(); // our acknowledgement of it
    size_t keyings = sender.transport.transmissions.size();

    CHECK(sender.protocol.sendMessage("Copy that", "VK3ABC", error));
    int64_t id = sender.observer.added.back().id;
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == keyings + 1);
    CHECK(sender.observer.lastUpdateFor(id)->status == MessageStatus::Sent);
    CHECK(sender.protocol.pendingCount() == 0);

    // Nothing more goes, however long we wait.
    sender.nowMs += ACK_TIMEOUT_MILLISECONDS * 10;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == keyings + 1);
    CHECK(!everUpdatedTo(sender.observer, id, MessageStatus::Retrying));

    // Its own messages still get our acknowledgement, and a ping from it
    // with Auto acknowledge back on turns retries back on.
    receiver.protocol.setAutoReplyEnabled(true);
    CHECK(receiver.protocol.sendPing("W1AW", error));
    receiver.completeOneTransmission();
    CHECK(decodeOne(receiver.transport.transmissions.back()[0]).senderAutoAck);
    sender.receiveFrom(receiver.transport);
    CHECK(sender.protocol.stationAutoAcks("VK3ABC"));
    CHECK(hasSystemLine(sender.observer, "VK3ABC : Auto ACK on"));
}

void testPingSaysAutoAckIsOff()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");
    receiver.protocol.setAutoReplyEnabled(false);

    std::string error;
    CHECK(receiver.protocol.sendPing("W1AW", error));
    receiver.completeOneTransmission();
    const std::vector<uint8_t>& ping = receiver.transport.transmissions[0][0];
    CHECK((ping[0] >> 4) == 0x4); // a ping, Auto ACK off
    sender.receiveFrom(receiver.transport);
    CHECK(!sender.protocol.stationAutoAcks("VK3ABC"));

    // We still answer its ping, and our ping to it says why nothing came back.
    CHECK(sender.protocol.pendingCount() == 1);
    sender.completeOneTransmission();
    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.completeOneTransmission();
    sender.nowMs += PING_TIMEOUT_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(hasSystemLine(sender.observer, "no response to PING (its Auto ACK is off)"));
}

// A message already waiting on its acknowledgement when the station says it
// will not send one ends as sent when the wait runs out.
void testWaitingMessageEndsWhenTheStationSaysItWillNotAnswer()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("Anybody there", "VK3ABC", error));
    int64_t id = sender.observer.added[0].id;
    sender.completeOneTransmission();
    CHECK(sender.observer.lastUpdateFor(id)->status == MessageStatus::AwaitingAck);

    receiver.protocol.setAutoReplyEnabled(false);
    CHECK(receiver.protocol.sendMessage("CQ CQ", "", error));
    receiver.completeOneTransmission();
    CHECK((receiver.transport.transmissions[0][0][0] >> 4) == 0xD); // a broadcast, Auto ACK off
    sender.receiveFrom(receiver.transport);

    sender.nowMs += ACK_TIMEOUT_MILLISECONDS * 4;
    sender.protocol.tick();
    CHECK(sender.observer.lastUpdateFor(id)->status == MessageStatus::Sent);
    CHECK(!everUpdatedTo(sender.observer, id, MessageStatus::Retrying));
    CHECK(sender.transport.transmissions.size() == 1);
    CHECK(sender.protocol.pendingCount() == 0);
}

// An acknowledgement from a station is that station acknowledging by itself,
// even of a message we sent it expecting none.
void testAcknowledgementTurnsRetriesBackOn()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");
    receiver.protocol.setAutoReplyEnabled(false);

    std::string error;
    CHECK(receiver.protocol.sendMessage("CQ CQ", "", error));
    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport);
    CHECK(!sender.protocol.stationAutoAcks("VK3ABC"));

    receiver.protocol.setAutoReplyEnabled(true);
    CHECK(sender.protocol.sendMessage("Hello", "VK3ABC", error));
    int64_t id = sender.observer.added.back().id;
    sender.completeOneTransmission();
    CHECK(sender.observer.lastUpdateFor(id)->status == MessageStatus::Sent);

    receiver.receiveFrom(sender.transport);
    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport);
    CHECK(sender.protocol.stationAutoAcks("VK3ABC"));
    CHECK(sender.observer.lastUpdateFor(id)->status == MessageStatus::Sent);

    // A message queued while it said no follows what it says by the time
    // the message goes.
    receiver.protocol.setAutoReplyEnabled(false);
    receiver.nowMs = sender.nowMs;
    CHECK(receiver.protocol.sendMessage("Going quiet", "", error));
    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport);
    CHECK(sender.protocol.sendMessage("Still there?", "VK3ABC", error));
    id = sender.observer.added.back().id;

    receiver.protocol.setAutoReplyEnabled(true);
    CHECK(receiver.protocol.sendMessage("Back", "", error));
    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport);
    sender.completeOneTransmission();
    CHECK(sender.observer.lastUpdateFor(id)->status == MessageStatus::AwaitingAck);
}

// A transmission parked on its acknowledgement timer leaves the transmitter
// idle, so the traffic queued behind it has to keep moving. Before this was
// fixed, one unanswered message stalled the whole outbox for the entire retry
// cycle, and the operator's next message or ping simply never went out.
void testAckWaitDoesNotBlockTheQueue()
{
    Station sender("W1AW");

    std::string error;
    CHECK(sender.protocol.sendMessage("First", "VK3ABC", error));
    sender.completeOneTransmission();

    int64_t firstId = sender.observer.added[0].id;
    const TextMessage* update = sender.observer.lastUpdateFor(firstId);
    CHECK(update != nullptr && update->status == MessageStatus::AwaitingAck);

    // Queued while the first message is still waiting to be acknowledged.
    CHECK(sender.protocol.sendMessage("Second", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == 2);
    CHECK(sender.protocol.pendingCount() == 2);

    int64_t secondId = sender.observer.added[1].id;
    uint16_t secondAirId = sender.observer.added[1].airId;
    update = sender.observer.lastUpdateFor(secondId);
    CHECK(update != nullptr && update->status == MessageStatus::AwaitingAck);

    // The second message is acknowledged first. It sits behind the first one
    // in the queue, and the acknowledgement has to find it there and leave the
    // other message's retry timer running.
    Frame ack;
    ack.type = FrameType::MessageAck;
    ack.originCallsign = "VK3ABC";
    ack.destinationCrc = FrameCodec::callsignHash("W1AW");
    ack.airId = secondAirId;
    sender.protocol.onFrameReceived(ack, 5.0f);

    update = sender.observer.lastUpdateFor(secondId);
    CHECK(update != nullptr && update->status == MessageStatus::Acknowledged);
    CHECK(sender.protocol.pendingCount() == 1);

    update = sender.observer.lastUpdateFor(firstId);
    CHECK(update != nullptr && update->status == MessageStatus::AwaitingAck);

    // The unacknowledged one still retries on its own timer.
    sender.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
    sender.protocol.tick();
    update = sender.observer.lastUpdateFor(firstId);
    CHECK(update != nullptr && update->status == MessageStatus::Retrying);
}

void testVoiceTransmissionDefersChat()
{
    Station sender("W1AW");
    sender.transport.voiceActive = true;

    std::string error;
    CHECK(sender.protocol.sendMessage("Wait your turn", "VK3ABC", error));

    sender.protocol.tick();
    CHECK(sender.transport.transmissions.empty());

    sender.transport.voiceActive = false;
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == 1);
}

// Two half duplex stations that key the moment the other stops cannot hear
// each other. On the loopback bench both stations keyed within the same second
// and each missed the other's reply, so a reply waits for the sender's receiver
// to come back, and the next burst waits for the far end to have its turn.
void testTurnaroundKeepsStationsOffEachOther()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("Hello", "VK3ABC", error));
    sender.completeOneTransmission();

    // The acknowledgement is queued, but keying now would land on top of a
    // sender that is still turning its receiver back on.
    receiver.receiveFrom(sender.transport);
    CHECK(receiver.protocol.pendingCount() == 1);
    receiver.protocol.tick();
    CHECK(receiver.transport.transmissions.empty());

    receiver.nowMs += TURNAROUND_AFTER_RX_MILLISECONDS + 1;
    receiver.protocol.tick();
    CHECK(receiver.transport.transmissions.size() == 1);
}

// The other half of the same problem: having just transmitted, we owe the far
// end room to answer before starting whatever else is queued.
void testNextBurstWaitsForTheFarEndToAnswer()
{
    Station sender("W1AW");

    std::string error;
    CHECK(sender.protocol.sendMessage("first", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == 1);

    // Queued while the far end is presumably composing its acknowledgement.
    CHECK(sender.protocol.sendMessage("second", "VK3ABC", error));
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 1);

    sender.nowMs += MAX_TURNAROUND_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 2);
}

// The status line asks what we are waiting for, and it has to keep saying so
// for the whole acknowledgement cycle. Reporting only while the timer runs
// would blink the notice off every time the message went back on the air.
void testAckWaitCoversTheWholeCycle()
{
    Station sender("W1AW");
    CHECK(sender.protocol.ackWait() == AckWait::Nothing);

    std::string error;
    // Queued but never sent: waiting its turn, not waiting on a reply.
    CHECK(sender.protocol.sendMessage("Anybody there", "VK3ABC", error));
    CHECK(sender.protocol.ackWait() == AckWait::Nothing);

    sender.completeOneTransmission();
    CHECK(sender.protocol.ackWait() == AckWait::Message);

    // Still a message we are waiting on, part way through the retries.
    sender.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(sender.protocol.ackWait() == AckWait::Message);

    for (int attempt = 1; attempt <= MAX_MESSAGE_RETRIES; attempt++)
    {
        sender.completeOneTransmission();
        sender.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
        sender.protocol.tick();
    }

    // Given up on: nothing is outstanding, so the status line goes quiet.
    CHECK(sender.protocol.ackWait() == AckWait::Nothing);

    // A ping is distinguishable, because the window names it separately.
    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.protocol.ackWait() == AckWait::Ping);

    // A broadcast expects nothing back and must not claim otherwise.
    Station broadcaster("W1AW");
    CHECK(broadcaster.protocol.sendMessage("CQ", "", error));
    broadcaster.completeOneTransmission();
    CHECK(broadcaster.protocol.ackWait() == AckWait::Nothing);
}

// The collision the bench caught: a station finished a message at 14:29:23,
// the far end began acknowledging it at 14:29:24, and the station keyed again
// at 14:29:25 and never heard the reply. A burst that asked for an
// acknowledgement has to leave room for a whole burst coming back, not just
// for our own changeover.
void testAWaitedReplyOutlastsThePlainTurnaround()
{
    Station sender("W1AW");

    std::string error;
    CHECK(sender.protocol.sendMessage("needs an ack", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == 1);

    // Something else the operator queued while the reply is outstanding.
    CHECK(sender.protocol.sendPing("VK3ABC", error));

    // Past the wait a reply-free burst would have earned, and this is exactly
    // where the far end is mid-acknowledgement.
    sender.nowMs += TURNAROUND_AFTER_TX_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 1);

    // Once the reply has had its chance, the queue moves again.
    sender.nowMs += REPLY_WINDOW_MILLISECONDS;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 2);
}

// The window is room for a reply, not a fixed delay: once the acknowledgement
// is in, the transmitter is free as soon as the ordinary turnaround is up.
void testTheWindowEndsWhenTheReplyArrives()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("needs an ack", "VK3ABC", error));
    sender.completeOneTransmission();

    receiver.receiveFrom(sender.transport);
    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport);

    CHECK(sender.protocol.pendingCount() == 0);

    // Nothing is outstanding now, so the ordinary turnaround is all that
    // stands between us and the next burst. Were the window still running it
    // would take REPLY_WINDOW_MILLISECONDS, which is far longer than this.
    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.nowMs += TURNAROUND_AFTER_TX_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 2);
}

// A reply we owe is not held for our own reply window. On the bench a station
// waiting on an acknowledgement decoded a ping, held the pong five seconds for
// its own window, and keyed it one second before the pinging station, whose
// identical window had just expired, keyed its next message over the top.
void testReplyIsNotHeldForOurOwnReplyWindow()
{
    Station sender("W1AW");
    Station other("DJ2LS");

    std::string error;
    CHECK(sender.protocol.sendMessage("needs an ack", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == 1);

    // Somebody else pings us while VK3ABC's reply is still owed.
    CHECK(other.protocol.sendPing("W1AW", error));
    other.completeOneTransmission();
    sender.receiveFrom(other.transport);

    // Past the turnarounds but well inside the reply window, which is where
    // testAWaitedReplyOutlastsThePlainTurnaround shows a message still waits.
    CHECK(TURNAROUND_AFTER_TX_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS + 1 <
          REPLY_WINDOW_MILLISECONDS);
    sender.nowMs += TURNAROUND_AFTER_TX_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 2);
    CHECK(sender.transport.modes.back()[0] == BurstMode::Signalling); // the pong
}

// A retry backs off by a random amount before keying again, bounded by the
// attempt number, and different stations draw differently: on the bench two
// stations' retry timers expired in the same second and they keyed together,
// too close for either to sense the other's carrier.
void testRetryBacksOffBeforeKeyingAgain()
{
    const char* callsigns[] = {"W1AW", "VK3ABC", "DJ2LS", "G0ABC", "K1ABC", "TEST1/P"};
    std::vector<uint64_t> firstBackoffs;

    for (const char* callsign : callsigns)
    {
        Station sender(callsign);

        std::string error;
        CHECK(sender.protocol.sendMessage("Anybody there", "VK3XYZ", error));

        for (int attempt = 1; attempt <= MAX_MESSAGE_RETRIES; attempt++)
        {
            sender.completeOneTransmission();
            size_t sentSoFar = sender.transport.transmissions.size();

            // The timer expires; the retry is queued but must not key at once
            // unless its backoff happened to be zero.
            sender.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
            uint64_t expiredAt = sender.nowMs;
            uint64_t bound = (uint64_t)RETRY_BACKOFF_MILLISECONDS * attempt;

            uint64_t keyedAfter = 0;
            for (;;)
            {
                sender.protocol.tick();
                if (sender.transport.transmissions.size() > sentSoFar)
                {
                    keyedAfter = sender.nowMs - expiredAt;
                    break;
                }
                CHECK(sender.nowMs - expiredAt <= bound); // never later than the bound
                if (sender.nowMs - expiredAt > bound) break;
                sender.nowMs += 100;
            }

            CHECK(keyedAfter <= bound);
            if (attempt == 1) firstBackoffs.push_back(keyedAfter);
        }
    }

    // Six stations with identical traffic must not all key at the same moment,
    // and the backoff has to be real, not a bound that is never used.
    bool allEqual = true;
    uint64_t largest = 0;
    for (uint64_t backoff : firstBackoffs)
    {
        if (backoff != firstBackoffs[0]) allEqual = false;
        if (backoff > largest) largest = backoff;
    }
    CHECK(!allEqual);
    CHECK(largest > 0);
}

// Having answered a station, we give it the channel for a whole reply window
// before starting a keying of our own. On the bench the acknowledged station
// keyed two seconds after our acknowledgement ended, exactly when the plain
// turnaround let us key, and the two collided four times in one run.
void testAReplyGivesTheOtherStationTheChannel()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("first", "VK3ABC", error));
    sender.completeOneTransmission();
    receiver.receiveFrom(sender.transport); // queues our acknowledgement

    // Nothing of ours is queued, so the acknowledgement goes alone.
    receiver.completeOneTransmission();
    CHECK(receiver.transport.transmissions.size() == 1);
    CHECK(receiver.transport.modes.back().size() == 1);
    CHECK(decodeOne(receiver.transport.transmissions.back()[0]).burstsFollowing == 0);

    // Traffic queued after it waits past the plain turnaround...
    CHECK(receiver.protocol.sendMessage("mine", "W1AW", error));
    receiver.nowMs += TURNAROUND_AFTER_TX_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS + 1;
    receiver.protocol.tick();
    CHECK(receiver.transport.transmissions.size() == 1);

    // ...until the window is up.
    receiver.nowMs += REPLY_WINDOW_MILLISECONDS;
    receiver.protocol.tick();
    CHECK(receiver.transport.transmissions.size() == 2);
}

// A station with a message queued sends it behind the acknowledgement it owes,
// in the same keying, rather than waiting for a gap that the other side's
// traffic never leaves. The acknowledgement says more follows, so listeners
// leave the channel alone for the message too.
void testQueuedMessageRidesBehindAReply()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    CHECK(receiver.protocol.sendMessage("mine", "W1AW", error));
    int64_t mine = receiver.observer.added[0].id;
    CHECK(sender.protocol.sendMessage("first", "VK3ABC", error));
    int64_t first = sender.observer.added[0].id;
    sender.completeOneTransmission();
    receiver.receiveFrom(sender.transport);

    receiver.completeOneTransmission();
    CHECK(receiver.transport.transmissions.size() == 1);
    const std::vector<BurstMode>& modes = receiver.transport.modes.back();
    CHECK(modes.size() == 2);
    CHECK(modes.size() == 2 && modes[0] == BurstMode::Signalling && modes[1] == BurstMode::Text);
    Frame ack = decodeOne(receiver.transport.transmissions.back()[0]);
    CHECK(ack.type == FrameType::MessageAck);
    CHECK(ack.burstsFollowing == 1);
    CHECK(decodeOne(receiver.transport.transmissions.back()[1]).burstsFollowing == 0);

    // Both went on the air and both concluded: one delivered, one awaiting.
    const TextMessage* update = receiver.observer.lastUpdateFor(mine);
    CHECK(update != nullptr && update->status == MessageStatus::AwaitingAck);
    CHECK(receiver.protocol.ackWait() == AckWait::Message);

    sender.receiveFrom(receiver.transport);
    update = sender.observer.lastUpdateFor(first);
    CHECK(update != nullptr && update->status == MessageStatus::Acknowledged);
    CHECK(sender.observer.added.size() == 2);
    CHECK(sender.observer.added.size() == 2 && sender.observer.added[1].text == "mine");
}

// What rides behind a reply is traffic of our own that is waiting its turn:
// not a message already sent and waiting on its answer, which would go out
// again early, and not another reply, which would take the place of our own
// message and leave it waiting.
void testOnlyWaitingTrafficOfOurOwnRides()
{
    std::string error;

    // Sent already and awaiting its acknowledgement: the reply goes alone.
    Station sender("W1AW");
    Station receiver("VK3ABC");
    CHECK(receiver.protocol.sendMessage("earlier", "W1AW", error));
    receiver.completeOneTransmission();
    CHECK(receiver.protocol.ackWait() == AckWait::Message);
    CHECK(sender.protocol.sendMessage("first", "VK3ABC", error));
    sender.completeOneTransmission();
    receiver.receiveFrom(sender.transport);
    receiver.completeOneTransmission();
    CHECK(receiver.transport.transmissions.size() == 2);
    CHECK(receiver.transport.modes.back().size() == 1);

    // Two replies owed and a message queued: the message rides behind the
    // first reply, and the second reply follows on its own.
    Station one("W1AW");
    Station two("DJ2LS");
    Station busy("VK3ABC");
    CHECK(busy.protocol.sendMessage("mine", "K1ABC", error));
    CHECK(one.protocol.sendMessage("from one", "VK3ABC", error));
    CHECK(two.protocol.sendMessage("from two", "VK3ABC", error));
    one.completeOneTransmission();
    two.completeOneTransmission();
    busy.receiveFrom(one.transport);
    busy.receiveFrom(two.transport);
    CHECK(busy.protocol.pendingCount() == 3);

    busy.completeOneTransmission();
    const std::vector<BurstMode>& modes = busy.transport.modes.back();
    CHECK(modes.size() == 2 && modes[0] == BurstMode::Signalling && modes[1] == BurstMode::Text);
}

// A listener told by a reply that more follows, which then loses what follows
// to a fade, holds the channel for two fragments after the reply, however
// short the keying was. The station that replied must not start a keying of
// its own until that station has had its turn after that: on the bench it
// counted from its own unkeying instead, and the two keyed together.
void testOwnTrafficWaitsUntilListenersLetGo()
{
    Station sender("W1AW");
    Station replier("VK3ABC");

    std::string error;
    CHECK(replier.protocol.sendMessage("rides", "W1AW", error));
    CHECK(replier.protocol.sendMessage("waits", "W1AW", error));
    CHECK(sender.protocol.sendMessage("first", "VK3ABC", error));
    sender.completeOneTransmission();
    replier.receiveFrom(sender.transport);

    // The acknowledgement with one message behind it; the harness's keying
    // starts and ends at the same instant, the shortest a keying can be.
    replier.completeOneTransmission();
    CHECK(replier.transport.transmissions.size() == 1);
    CHECK(replier.transport.modes.back().size() == 2);
    uint64_t keyedAt = replier.nowMs;

    // When a listener that lost the message could still be holding on, plus
    // the turn it is owed.
    uint64_t heldUntil = keyedAt + TEXT_FRAGMENT_AIR_MILLISECONDS +
                         SIGNALLING_FOLLOWED_RESERVATION_MILLISECONDS;
    uint64_t earliest = heldUntil + REPLY_WINDOW_MILLISECONDS;

    uint64_t firstOwn = 0;
    for (replier.nowMs = keyedAt; replier.nowMs <= earliest + 5000; replier.nowMs += 100)
    {
        replier.protocol.tick();
        if (replier.transport.transmissions.size() > 1)
        {
            firstOwn = replier.nowMs;
            break;
        }
    }
    CHECK(firstOwn >= earliest);
    CHECK(firstOwn != 0);
}

// That wait is for a station that lost what rode behind the reply. One heard
// again has not: it is answering, so our own traffic goes after its turn as
// usual. At Presto the wait above held every message of ours for over two
// minutes after a pong with a message behind it, where the far end's answer
// to that message had come within half a minute.
void testOwnTrafficGoesOnceTheAnsweredStationIsHeard()
{
    Station sender("W1AW");
    Station replier("VK3ABC");
    AirTiming presto = AirTiming::forFrameSeconds(6.88, 9, 2.72, 6.88, 2.72, 0.6, 0.6, 0.32);
    sender.protocol.setAirTiming(presto);
    replier.protocol.setAirTiming(presto);

    std::string error;
    CHECK(replier.protocol.sendMessage("rides", "W1AW", error));
    CHECK(replier.protocol.sendMessage("waits", "W1AW", error));
    CHECK(sender.protocol.sendMessage("first", "VK3ABC", error));
    sender.completeOneTransmission();
    replier.receiveFrom(sender.transport);

    replier.completeOneTransmission();
    CHECK(replier.transport.modes.back().size() == 2);
    uint64_t keyedAt = replier.nowMs;
    uint64_t heldUntil = keyedAt + (uint64_t)presto.textFragmentAirMs +
                         (uint64_t)presto.signallingFollowedReservationMs;

    // The sender heard it all, and acknowledges the message that rode along.
    sender.nowMs = replier.nowMs;
    sender.receiveFrom(replier.transport);
    sender.completeOneTransmission();
    CHECK(decodeOne(sender.transport.transmissions.back()[0]).type == FrameType::MessageAck);
    replier.nowMs = sender.nowMs;
    replier.receiveFrom(sender.transport);

    uint64_t firstOwn = 0;
    for (; replier.nowMs <= heldUntil + (uint64_t)presto.replyWindowMs + 10000; replier.nowMs += 100)
    {
        replier.protocol.tick();
        if (replier.transport.transmissions.size() > 1)
        {
            firstOwn = replier.nowMs;
            break;
        }
    }
    CHECK(firstOwn != 0);
    CHECK(firstOwn < heldUntil);
    CHECK(firstOwn >= keyedAt + (uint64_t)presto.replyWindowMs);
}

// The turn given to a station just answered holds back our own traffic, not a
// reply to what that station sends in its turn.
void testRepliesDoNotWaitForTheAnsweredStationsTurn()
{
    Station sender("W1AW");
    Station replier("VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("first", "VK3ABC", error));
    CHECK(sender.protocol.sendMessage("second", "VK3ABC", error));
    sender.completeOneTransmission();
    replier.receiveFrom(sender.transport);
    replier.completeOneTransmission(); // the acknowledgement, alone
    uint64_t keyedAt = replier.nowMs;

    // The sender takes its turn, and the second message arrives at once.
    sender.receiveFrom(replier.transport);
    sender.completeOneTransmission();
    replier.receiveFrom(sender.transport);

    // Its acknowledgement goes after the turnarounds, not after the window.
    uint64_t acked = 0;
    for (; replier.nowMs <= keyedAt + REPLY_WINDOW_MILLISECONDS; replier.nowMs += 100)
    {
        replier.protocol.tick();
        if (replier.transport.transmissions.size() > 1)
        {
            acked = replier.nowMs;
            break;
        }
    }
    CHECK(acked != 0);
    CHECK(acked != 0 &&
          decodeOne(replier.transport.transmissions.back()[0]).type == FrameType::MessageAck);
}

// Abort stops the message on the air, the one queued behind it and the retries
// of one already sent and waiting for its acknowledgement, and none of them
// keys again on its own afterwards, however long the station waits.
void testAbortDropsEverythingOutstanding()
{
    std::string error;

    Station station("VK3ABC");
    CHECK(station.protocol.sendMessage("sent earlier", "W1AW", error));
    int64_t sentEarlier = station.observer.added[0].id;
    station.completeOneTransmission();
    CHECK(station.transport.transmissions.size() == 1);
    const TextMessage* update = station.observer.lastUpdateFor(sentEarlier);
    CHECK(update != nullptr && update->status == MessageStatus::AwaitingAck);

    CHECK(station.protocol.sendMessage("on the air", "W1AW", error));
    CHECK(station.protocol.sendMessage("waiting", "W1AW", error));
    int64_t onAir = station.observer.added[1].id;
    int64_t waiting = station.observer.added[2].id;

    station.nowMs += std::max(MAX_TURNAROUND_MILLISECONDS, MAX_RETRY_BACKOFF_MILLISECONDS) + 1;
    station.protocol.tick();
    CHECK(station.transport.transmissions.size() == 2);
    CHECK(station.transport.transmitting);

    station.protocol.abortTransmission();
    station.transport.transmitting = false; // the transport has unkeyed
    CHECK(station.protocol.pendingCount() == 0);

    update = station.observer.lastUpdateFor(onAir);
    CHECK(update != nullptr && update->status == MessageStatus::Aborted);
    CHECK(update != nullptr && deliveryChipState(*update).kind == DeliveryChipKind::Aborted);
    update = station.observer.lastUpdateFor(waiting);
    CHECK(update != nullptr && update->status == MessageStatus::Aborted);
    update = station.observer.lastUpdateFor(sentEarlier);
    CHECK(update != nullptr && update->status == MessageStatus::Aborted);

    for (int i = 0; i < 20; i++)
    {
        station.nowMs += 60 * 1000;
        station.protocol.tick();
    }
    CHECK(station.transport.transmissions.size() == 2);
}

// The operator can take back one message without touching the rest. One
// queued while another is on the air has not gone anywhere yet, so it is
// removed and never sent; the one on the air is aborted, and the caller is
// told to stop the keying.
void testOneMessageCanBeRemovedOrAborted()
{
    std::string error;

    Station station("VK3ABC");
    CHECK(station.protocol.sendMessage("on the air", "W1AW", error));
    int64_t onAir = station.observer.added[0].id;
    station.nowMs += std::max(MAX_TURNAROUND_MILLISECONDS, MAX_RETRY_BACKOFF_MILLISECONDS) + 1;
    station.protocol.tick();
    CHECK(station.transport.transmitting);

    // Queued while the transmitter is keyed.
    CHECK(station.protocol.sendMessage("changed my mind", "W1AW", error));
    CHECK(station.protocol.sendMessage("still wanted", "W1AW", error));
    int64_t removed = station.observer.added[1].id;
    int64_t wanted = station.observer.added[2].id;

    CHECK(station.protocol.cancelFor(onAir) == TextMessagingProtocol::Cancel::Abort);
    CHECK(station.protocol.cancelFor(removed) == TextMessagingProtocol::Cancel::Remove);
    std::vector<int64_t> outstanding = station.protocol.outstandingMessageIds();
    CHECK(outstanding.size() == 3);

    bool keyed = true;
    CHECK(station.protocol.cancelMessage(removed, &keyed) == TextMessagingProtocol::Cancel::Remove);
    CHECK(!keyed);
    const TextMessage* update = station.observer.lastUpdateFor(removed);
    CHECK(update != nullptr && update->status == MessageStatus::NotSent);
    CHECK(station.protocol.cancelFor(removed) == TextMessagingProtocol::Cancel::None);
    CHECK(station.protocol.cancelMessage(removed) == TextMessagingProtocol::Cancel::None);

    CHECK(station.protocol.cancelMessage(onAir, &keyed) == TextMessagingProtocol::Cancel::Abort);
    CHECK(keyed);
    update = station.observer.lastUpdateFor(onAir);
    CHECK(update != nullptr && update->status == MessageStatus::Aborted);
    station.transport.transmitting = false; // the window stopped the keying

    // What is left goes on as before, and only it.
    station.completeOneTransmission();
    CHECK(station.transport.transmissions.size() == 2);
    CHECK(decodeOne(station.transport.transmissions[1][0]).payload.size() > 0);
    update = station.observer.lastUpdateFor(wanted);
    CHECK(update != nullptr && update->status == MessageStatus::AwaitingAck);
    CHECK(station.protocol.outstandingMessageIds() == std::vector<int64_t>{wanted});

    // Neither cancelled message is ever tried again.
    for (int i = 0; i < 20; i++)
    {
        station.completeOneTransmission();
        station.nowMs += 60 * 1000;
        station.protocol.tick();
    }
    update = station.observer.lastUpdateFor(onAir);
    CHECK(update != nullptr && update->status == MessageStatus::Aborted);
    update = station.observer.lastUpdateFor(removed);
    CHECK(update != nullptr && update->status == MessageStatus::NotSent);
    CHECK(station.protocol.pendingCount() == 0);
    CHECK((int)station.transport.transmissions.size() == 2 + MAX_MESSAGE_RETRIES);
}

// A message waiting for its acknowledgement, or for a retry, has been on the
// air, so it is aborted rather than removed. Pings, replies and finished
// messages offer nothing.
void testSentMessagesAreAbortedNotRemoved()
{
    std::string error;

    Station station("VK3ABC");
    Station far("W1AW");
    CHECK(station.protocol.sendMessage("waiting on you", "W1AW", error));
    int64_t waiting = station.observer.added[0].id;
    station.completeOneTransmission();
    CHECK(station.observer.lastUpdateFor(waiting)->status == MessageStatus::AwaitingAck);
    CHECK(station.protocol.cancelFor(waiting) == TextMessagingProtocol::Cancel::Abort);

    // Unanswered, it is queued again for a retry: still an abort.
    station.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
    station.protocol.tick();
    CHECK(station.observer.lastUpdateFor(waiting)->status == MessageStatus::Retrying);
    CHECK(station.protocol.cancelFor(waiting) == TextMessagingProtocol::Cancel::Abort);

    bool keyed = true;
    CHECK(station.protocol.cancelMessage(waiting, &keyed) == TextMessagingProtocol::Cancel::Abort);
    CHECK(!keyed);
    CHECK(station.observer.lastUpdateFor(waiting)->status == MessageStatus::Aborted);

    // Its acknowledgement turning up late changes nothing.
    far.receiveFrom(station.transport);
    far.completeOneTransmission();
    station.receiveFrom(far.transport);
    CHECK(station.observer.lastUpdateFor(waiting)->status == MessageStatus::Aborted);

    CHECK(station.protocol.sendMessage("delivered", "W1AW", error));
    int64_t delivered = station.observer.added.back().id;
    size_t keyings = station.transport.transmissions.size();
    for (int i = 0; i < 10 && station.transport.transmissions.size() == keyings; i++)
    {
        station.completeOneTransmission();
    }
    CHECK(station.transport.transmissions.size() == keyings + 1);
    far.receiveFrom(station.transport);
    far.completeOneTransmission();
    station.receiveFrom(far.transport);
    CHECK(station.observer.lastUpdateFor(delivered)->status == MessageStatus::Acknowledged);
    CHECK(station.protocol.cancelFor(delivered) == TextMessagingProtocol::Cancel::None);

    CHECK(station.protocol.cancelFor(12345678) == TextMessagingProtocol::Cancel::None);
}

// A ping is taken back the same way: removed while it waits for its turn,
// aborted once it is on the air or waiting for the pong.
void testPingsCanBeRemovedOrAborted()
{
    std::string error;

    Station station("VK3ABC");
    Station far("W1AW");
    CHECK(station.protocol.sendPing("W1AW", error));
    int64_t waiting = station.observer.added.back().id;
    CHECK(station.protocol.cancelFor(waiting) == TextMessagingProtocol::Cancel::Remove);
    CHECK(station.protocol.outstandingMessageIds() == std::vector<int64_t>{waiting});

    bool keyed = true;
    CHECK(station.protocol.cancelMessage(waiting, &keyed) == TextMessagingProtocol::Cancel::Remove);
    CHECK(!keyed);
    CHECK(station.observer.lastUpdateFor(waiting)->status == MessageStatus::NotSent);
    CHECK(station.protocol.pendingCount() == 0);
    station.completeOneTransmission();
    CHECK(station.transport.transmissions.empty());

    // On the air.
    CHECK(station.protocol.sendPing("W1AW", error));
    int64_t onAir = station.observer.added.back().id;
    station.nowMs += std::max(MAX_TURNAROUND_MILLISECONDS, MAX_RETRY_BACKOFF_MILLISECONDS) + 1;
    station.protocol.tick();
    CHECK(station.transport.transmitting);
    CHECK(station.protocol.cancelFor(onAir) == TextMessagingProtocol::Cancel::Abort);
    CHECK(station.protocol.cancelMessage(onAir, &keyed) == TextMessagingProtocol::Cancel::Abort);
    CHECK(keyed);
    CHECK(station.observer.lastUpdateFor(onAir)->status == MessageStatus::Aborted);
    station.transport.transmitting = false;

    // Waiting for the pong, which then arrives and changes nothing.
    CHECK(station.protocol.sendPing("W1AW", error));
    int64_t answered = station.observer.added.back().id;
    station.completeOneTransmission();
    CHECK(station.observer.lastUpdateFor(answered)->status == MessageStatus::AwaitingAck);
    CHECK(station.protocol.cancelMessage(answered, &keyed) == TextMessagingProtocol::Cancel::Abort);
    CHECK(!keyed);
    far.receiveFrom(station.transport);
    far.completeOneTransmission();
    station.receiveFrom(far.transport);
    CHECK(station.observer.lastUpdateFor(answered)->status == MessageStatus::Aborted);
    CHECK(station.protocol.pendingCount() == 0);
}

// A station on a frequency where it may not send data transmits nothing at
// all: what was waiting is discarded as not sent, new traffic is refused with
// the reason, and messages it receives are shown but not acknowledged.
void testInhibitedStationTransmitsNothing()
{
    const std::string reason = "not here";
    std::string error;

    Station station("VK3ABC");
    CHECK(station.protocol.sendMessage("waiting", "W1AW", error));
    int64_t waiting = station.observer.added[0].id;
    station.protocol.setTransmitInhibited(reason);
    CHECK(station.protocol.transmitInhibitedReason() == reason);
    CHECK(station.protocol.pendingCount() == 0);
    const TextMessage* update = station.observer.lastUpdateFor(waiting);
    CHECK(update != nullptr && update->status == MessageStatus::NotSent);
    CHECK(update != nullptr && deliveryChipState(*update).kind == DeliveryChipKind::NotSent);

    CHECK(!station.protocol.sendMessage("refused", "W1AW", error));
    CHECK(error == reason);
    error.clear();
    CHECK(!station.protocol.sendPing("W1AW", error));
    CHECK(error == reason);

    // A message arrives: shown, but its acknowledgement never goes out.
    Station sender("W1AW");
    CHECK(sender.protocol.sendMessage("hello", "VK3ABC", error));
    sender.completeOneTransmission();
    station.receiveFrom(sender.transport);
    CHECK(station.observer.added.size() == 2);
    for (int i = 0; i < 5; i++) station.completeOneTransmission();
    CHECK(station.transport.transmissions.empty());
    CHECK(station.protocol.pendingCount() == 0);

    // Lifted, it transmits again.
    station.protocol.setTransmitInhibited("");
    CHECK(station.protocol.sendMessage("now", "W1AW", error));
    station.completeOneTransmission();
    CHECK(station.transport.transmissions.size() == 1);
}

// Inhibiting stops what has not gone out, not what has: a message already sent
// can still be acknowledged, since receiving carries on, but one whose timer
// runs out is not retried.
void testInhibitingLeavesSentMessagesToTheirAnswers()
{
    std::string error;
    Station sender("W1AW");
    Station receiver("VK3ABC");

    CHECK(sender.protocol.sendMessage("answered", "VK3ABC", error));
    int64_t answered = sender.observer.added[0].id;
    sender.completeOneTransmission();
    sender.protocol.setTransmitInhibited("not here");
    receiver.receiveFrom(sender.transport);
    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport);
    const TextMessage* update = sender.observer.lastUpdateFor(answered);
    CHECK(update != nullptr && update->status == MessageStatus::Acknowledged);

    Station lonely("W1AW");
    CHECK(lonely.protocol.sendMessage("unanswered", "VK3ABC", error));
    int64_t unanswered = lonely.observer.added[0].id;
    lonely.completeOneTransmission();
    lonely.protocol.setTransmitInhibited("not here");
    lonely.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
    lonely.protocol.tick();
    for (int i = 0; i < 3; i++) lonely.completeOneTransmission();
    CHECK(lonely.transport.transmissions.size() == 1);
    update = lonely.observer.lastUpdateFor(unanswered);
    CHECK(update != nullptr && update->status == MessageStatus::NotSent);
    CHECK(lonely.protocol.pendingCount() == 0);
}

// Two stations with messages queued for each other alternate one each: every
// keying after the first is an acknowledgement with the next message behind
// it. Before, whichever station went first sent its whole queue while the
// other only acknowledged.
void testBusyStationsTakeTurns()
{
    Station a("W1AW");
    Station b("VK3ABC");

    std::string error;
    for (const char* text : {"a1", "a2", "a3"}) CHECK(a.protocol.sendMessage(text, "VK3ABC", error));
    for (const char* text : {"b1", "b2", "b3"}) CHECK(b.protocol.sendMessage(text, "W1AW", error));

    // Who said what, in the order it reached the other side.
    std::vector<std::string> heard;
    Station* talker = &a;
    Station* listener = &b;
    for (int keying = 0; keying < 7; keying++)
    {
        size_t before = listener->observer.added.size();
        talker->completeOneTransmission();
        listener->receiveFrom(talker->transport);
        for (size_t i = before; i < listener->observer.added.size(); i++)
        {
            heard.push_back(listener->observer.added[i].text);
        }

        if (keying > 0 && keying < 6) CHECK(talker->transport.modes.back().size() == 2);
        std::swap(talker, listener);
    }

    std::vector<std::string> expected{"a1", "b1", "a2", "b2", "a3", "b3"};
    CHECK(heard == expected);
    CHECK(a.protocol.pendingCount() == 0);
    CHECK(b.protocol.pendingCount() == 0);
}

// A fragment says how many more bursts its sender holds the channel for,
// whatever the demodulator says in between: on the bench it read the channel
// clear for two seconds in the middle of a four fragment message.
void testFragmentsStillToComeReserveTheChannel()
{
    Station sender("W1AW");

    std::string error;
    std::string body(perFragment('A') * 2 + 10, 'A'); // three fragments
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    sender.completeOneTransmission();
    const auto& frames = sender.transport.transmissions[0];
    CHECK(frames.size() == 3);

    auto decodeInto = [&](Station& station, size_t index)
    {
        Frame frame;
        CHECK(FrameCodec::decode(frames[index].data(), (int)frames[index].size(), frame));
        station.protocol.onFrameReceived(frame, 5.0f);
    };

    // Only the first fragment has been heard: two more bursts are coming.
    Station partial("VK3ABC");
    CHECK(partial.protocol.sendMessage("waiting", "W1AW", error));
    uint64_t heardAt = partial.nowMs;
    decodeInto(partial, 0);

    partial.nowMs = heardAt + MAX_TURNAROUND_MILLISECONDS + 1;
    partial.protocol.tick();
    CHECK(partial.transport.transmissions.empty());

    // The reservation lapses, then the random pause every release carries.
    partial.nowMs = heardAt + 2 * TEXT_FRAGMENT_AIR_MILLISECONDS + 1;
    partial.protocol.tick();
    partial.nowMs += TURNAROUND_JITTER_MILLISECONDS + 1;
    partial.protocol.tick();
    CHECK(partial.transport.transmissions.size() == 1);

    // A message for somebody else holds the channel just the same: on the
    // bench a third station's eight fragment message ran forty four seconds.
    Station bystander("DJ2LS");
    CHECK(bystander.protocol.sendMessage("waiting", "W1AW", error));
    heardAt = bystander.nowMs;
    decodeInto(bystander, 0);
    bystander.nowMs = heardAt + MAX_TURNAROUND_MILLISECONDS + 1;
    bystander.protocol.tick();
    CHECK(bystander.transport.transmissions.empty());

    // The last fragment ends the sender's keying, even with one lost before it.
    Station lossy("VK3ABC");
    CHECK(lossy.protocol.sendMessage("waiting", "W1AW", error));
    decodeInto(lossy, 0);
    decodeInto(lossy, 2);
    lossy.nowMs += MAX_TURNAROUND_MILLISECONDS + 1;
    lossy.protocol.tick();
    CHECK(lossy.transport.transmissions.size() == 1);
}

// Everybody who heard a burst comes unfrozen at the same instant, so the
// release carries a random pause: stations with identical traffic must not
// all key together the moment the channel clears.
void testClearingChannelReleasesStationsAtDifferentMoments()
{
    const char* callsigns[] = {"W1AW", "VK3ABC", "DJ2LS", "G0ABC", "K1ABC", "TEST1/P"};
    std::vector<uint64_t> delays;

    for (const char* callsign : callsigns)
    {
        Station station(callsign);
        station.transport.channelBusy = true;

        std::string error;
        CHECK(station.protocol.sendMessage("Hold it", "VK3XYZ", error));
        station.nowMs += 2000;
        station.protocol.tick();
        CHECK(station.transport.transmissions.empty());

        station.transport.channelBusy = false;
        uint64_t clearedAt = station.nowMs;
        for (;;)
        {
            station.protocol.tick();
            if (!station.transport.transmissions.empty()) break;
            CHECK(station.nowMs - clearedAt <= TURNAROUND_JITTER_MILLISECONDS);
            if (station.nowMs - clearedAt > TURNAROUND_JITTER_MILLISECONDS) break;
            station.nowMs += 100;
        }
        delays.push_back(station.nowMs - clearedAt);
    }

    bool allEqual = true;
    for (uint64_t delay : delays)
    {
        if (delay != delays[0]) allEqual = false;
    }
    CHECK(!allEqual);
}

// A retransmitted message we already have arrives one fragment at a time, and
// every duplicate fragment asks for the acknowledgement again. One queued
// acknowledgement answers them all.
void testRetransmittedFragmentsQueueOneAcknowledgement()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    std::string body(perFragment('A') * 2 + 10, 'A'); // three fragments
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    sender.completeOneTransmission();
    receiver.receiveFrom(sender.transport);
    CHECK(receiver.observer.added.size() == 1);
    receiver.completeOneTransmission(); // the acknowledgement, lost on the way

    sender.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
    sender.protocol.tick();
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.back().size() == 3);

    receiver.receiveFrom(sender.transport);
    CHECK(receiver.observer.added.size() == 1);
    CHECK(receiver.protocol.pendingCount() == 1);
}

// A sender that restarts must not have its new message taken for a
// retransmission of an old one that happened to carry the same ID: the
// receiver would acknowledge it and never show it, and the sender would see
// OK. The ID is forced to collide here, so the content check is what counts.
void testRestartedSenderIsNotMistakenForARetransmission()
{
    Station receiver("VK3ABC");
    std::string error;

    Station before("W1AW");
    CHECK(before.protocol.sendMessage("before the restart", "VK3ABC", error));
    before.completeOneTransmission();
    receiver.receiveFrom(before.transport);
    CHECK(receiver.observer.added.size() == 1);
    uint16_t reusedId = before.observer.added[0].airId;

    Station after("W1AW");
    CHECK(after.protocol.sendMessage("after the restart", "VK3ABC", error));
    after.completeOneTransmission();
    for (const std::vector<uint8_t>& raw : after.transport.transmissions.back())
    {
        Frame frame;
        CHECK(FrameCodec::decode(raw.data(), (int)raw.size(), frame));
        frame.airId = reusedId;
        receiver.protocol.onFrameReceived(frame, 5.0f);
    }

    CHECK(receiver.observer.added.size() == 2);
    CHECK(receiver.observer.added.back().text == "after the restart");

    // And a true retransmission of that new message is still recognised.
    for (const std::vector<uint8_t>& raw : after.transport.transmissions.back())
    {
        Frame frame;
        CHECK(FrameCodec::decode(raw.data(), (int)raw.size(), frame));
        frame.airId = reusedId;
        receiver.protocol.onFrameReceived(frame, 5.0f);
    }
    CHECK(receiver.observer.added.size() == 2);
}

// Message IDs start at a random point, so stations that restart do not all
// begin again at the same ID.
void testMessageIdsStartAtRandom()
{
    std::vector<uint16_t> firstIds;
    for (int i = 0; i < 4; i++)
    {
        Station sender("W1AW");
        std::string error;
        CHECK(sender.protocol.sendMessage("hello", "VK3ABC", error));
        firstIds.push_back(sender.observer.added[0].airId);
    }

    bool allEqual = true;
    for (uint16_t id : firstIds)
    {
        if (id != firstIds[0]) allEqual = false;
    }
    CHECK(!allEqual);
}

// Retries resend the same fragments, so a message can be pieced together from
// several attempts, each of which lost some of it to a fade. The partial copy
// has to last as long as fragments keep coming: timed from the first fragment
// it was dropped before a long message's second retry arrived.
void testRetriesFillInAMessageOverTime()
{
    Station sender("W1AW");
    std::string error;
    std::string body(perFragment('A') * 2 + 10, 'A'); // three fragments
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    sender.completeOneTransmission();
    const auto& frames = sender.transport.transmissions[0];
    CHECK(frames.size() == 3);

    auto decodeInto = [&](Station& station, size_t index)
    {
        Frame frame;
        CHECK(FrameCodec::decode(frames[index].data(), (int)frames[index].size(), frame));
        station.protocol.onFrameReceived(frame, 5.0f);
    };

    // One fragment per attempt, each attempt well inside the timeout of the
    // last but the whole span well beyond it.
    uint64_t step = REASSEMBLY_TIMEOUT_MILLISECONDS - 20000;
    Station receiver("VK3ABC");
    decodeInto(receiver, 0);
    receiver.nowMs += step;
    receiver.protocol.tick();
    decodeInto(receiver, 1);
    receiver.nowMs += step;
    receiver.protocol.tick();
    decodeInto(receiver, 2);
    CHECK(receiver.observer.added.size() == 1);
    CHECK(!receiver.observer.added.empty() && receiver.observer.added[0].text == body);

    // Silence for longer than the timeout still drops what was held.
    Station quiet("VK3ABC");
    decodeInto(quiet, 0);
    quiet.nowMs += REASSEMBLY_TIMEOUT_MILLISECONDS + 1;
    quiet.protocol.tick();
    decodeInto(quiet, 1);
    decodeInto(quiet, 2);
    CHECK(quiet.observer.added.empty());
}

// The reservation comes from what the frame says is still to come in its
// keying, not from its fragment number. A resend of fragments 2 and 6 of 8
// ends after fragment 6; "fragment k of n" would hold the channel for five
// more bursts after fragment 2 that are never coming.
void testReservationFollowsTheBurstsStillToCome()
{
    auto keyedAfter = [](Station& station, uint64_t heardAt, uint64_t limit) -> uint64_t
    {
        for (station.nowMs = heardAt; station.nowMs <= heardAt + limit; station.nowMs += 100)
        {
            station.protocol.tick();
            if (!station.transport.transmissions.empty()) return station.nowMs - heardAt;
        }
        return UINT64_MAX;
    };

    std::string error;
    Frame resend;
    resend.type = FrameType::Message;
    resend.destinationCrc = FrameCodec::callsignHash("K1ABC");
    resend.originCallsign = "DJ2LS";
    resend.airId = 0x242;
    resend.fragmentIndex = 1;
    resend.fragmentCount = 8;
    resend.burstsFollowing = 1; // fragment 6 comes next, then the keying ends
    resend.payload.assign(1, 'x');

    Station bystander("VK3ABC");
    CHECK(bystander.protocol.sendMessage("waiting", "W1AW", error));
    uint64_t heardAt = bystander.nowMs;
    bystander.protocol.onFrameReceived(resend, 5.0f);
    uint64_t waited = keyedAfter(bystander, heardAt, 8 * TEXT_FRAGMENT_AIR_MILLISECONDS);
    CHECK(waited >= (uint64_t)TEXT_FRAGMENT_AIR_MILLISECONDS);
    CHECK(waited <= (uint64_t)(TEXT_FRAGMENT_AIR_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS + 100));

    // A signalling frame that says more follows holds the channel for the
    // text that comes after it, allowing for the first fragment being lost.
    Frame ack;
    ack.type = FrameType::MessageAck;
    ack.destinationCrc = FrameCodec::callsignHash("K1ABC");
    ack.originCallsign = "DJ2LS";
    ack.airId = 0x243;
    ack.burstsFollowing = 1;

    Station listener("VK3ABC");
    CHECK(listener.protocol.sendMessage("waiting", "W1AW", error));
    heardAt = listener.nowMs;
    listener.protocol.onFrameReceived(ack, 5.0f);
    waited = keyedAfter(listener, heardAt, 4 * TEXT_FRAGMENT_AIR_MILLISECONDS);
    CHECK(waited >= (uint64_t)SIGNALLING_FOLLOWED_RESERVATION_MILLISECONDS);
    CHECK(waited <= (uint64_t)(SIGNALLING_FOLLOWED_RESERVATION_MILLISECONDS +
                               TURNAROUND_JITTER_MILLISECONDS + 100));

    // And a signalling frame with nothing after it reserves nothing.
    ack.burstsFollowing = 0;
    Station plain("VK3ABC");
    CHECK(plain.protocol.sendMessage("waiting", "W1AW", error));
    heardAt = plain.nowMs;
    plain.protocol.onFrameReceived(ack, 5.0f);
    waited = keyedAfter(plain, heardAt, 4 * TEXT_FRAGMENT_AIR_MILLISECONDS);
    CHECK(waited <= (uint64_t)MAX_TURNAROUND_MILLISECONDS);
}

// A receiver that heard part of a message tells the sender which fragments
// arrived once the sender's keying is over, and the sender resends just the
// rest, at once, without counting a retry.
void testMissingFragmentsAreAskedForAndResent()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    std::string body(perFragment('A') * 2 + 10, 'A'); // three fragments
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    int64_t id = sender.observer.added[0].id;
    sender.completeOneTransmission();
    std::vector<std::vector<uint8_t>> first = sender.transport.transmissions.back();
    CHECK(first.size() == 3);

    // Fragment two is lost to a fade; fragment three says the keying is over.
    receiver.protocol.onFrameReceived(decodeOne(first[0]), 5.0f);
    receiver.protocol.onFrameReceived(decodeOne(first[2]), 5.0f);
    CHECK(receiver.observer.added.empty());

    receiver.completeOneTransmission();
    CHECK(receiver.transport.transmissions.size() == 1);
    CHECK(receiver.transport.transmissions.back().size() == 1);
    Frame partial = decodeOne(receiver.transport.transmissions.back()[0]);
    CHECK(partial.type == FrameType::MessagePartialAck);
    CHECK(partial.airId == decodeOne(first[0]).airId);
    CHECK(partial.payload.size() == 1 && partial.payload[0] == 0x05);

    // The window learns how far the message got the moment the report arrives.
    sender.receiveFrom(receiver.transport);
    const TextMessage* progress = sender.observer.lastUpdateFor(id);
    CHECK(progress != nullptr && progress->fragmentsConfirmed == 2);
    CHECK(progress != nullptr && progress->fragmentCount == 3);
    CHECK(progress != nullptr && deliveryChipState(*progress).kind == DeliveryChipKind::Resend);

    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == 2);
    CHECK(sender.transport.transmissions.back().size() == 1);
    Frame resent = decodeOne(sender.transport.transmissions.back()[0]);
    CHECK(resent.fragmentIndex == 1);
    CHECK(resent.fragmentCount == 3);
    CHECK(resent.burstsFollowing == 0);
    CHECK(!everUpdatedTo(sender.observer, id, MessageStatus::Retrying));

    // The resend completes the message; one plain acknowledgement goes back.
    receiver.receiveFrom(sender.transport);
    CHECK(receiver.observer.added.size() == 1);
    CHECK(!receiver.observer.added.empty() && receiver.observer.added[0].text == body);
    CHECK(receiver.protocol.pendingCount() == 1);
    receiver.completeOneTransmission();
    CHECK(decodeOne(receiver.transport.transmissions.back()[0]).type == FrameType::MessageAck);

    sender.receiveFrom(receiver.transport);
    const TextMessage* update = sender.observer.lastUpdateFor(id);
    CHECK(update != nullptr && update->status == MessageStatus::Acknowledged);
    CHECK(update != nullptr && update->retryCount == 0);

    // Delivered is all of it: the bench showed "OK 2/3" here once.
    CHECK(update != nullptr && update->fragmentsConfirmed == 3);
    CHECK(update != nullptr && !deliveryChipState(*update).showsProgress());
}

// With the last fragment lost there is nothing saying the keying ended, so
// the receiver waits for as long as the last fragment it heard said the rest
// would take, and then asks. A broadcast asks for nothing, and neither does a
// station that may not transmit on its own.
void testLostLastFragmentStillAsksForTheRest()
{
    Station sender("W1AW");
    std::string error;
    std::string body(perFragment('A') * 2 + 10, 'A');
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    CHECK(sender.protocol.sendMessage(body, "", error)); // and as a broadcast
    sender.completeOneTransmission();
    std::vector<std::vector<uint8_t>> addressed = sender.transport.transmissions.back();
    sender.completeOneTransmission();
    std::vector<std::vector<uint8_t>> broadcast = sender.transport.transmissions.back();
    CHECK(decodeOne(broadcast[0]).type == FrameType::Broadcast);

    Station receiver("VK3ABC");
    uint64_t heardAt = receiver.nowMs;
    receiver.protocol.onFrameReceived(decodeOne(addressed[0]), 5.0f); // two still to come
    receiver.nowMs = heardAt + 2 * TEXT_FRAGMENT_AIR_MILLISECONDS - 1;
    receiver.protocol.tick();
    CHECK(receiver.protocol.pendingCount() == 0);
    receiver.nowMs = heardAt + 2 * TEXT_FRAGMENT_AIR_MILLISECONDS;
    receiver.protocol.tick();
    CHECK(receiver.protocol.pendingCount() == 1);
    receiver.completeOneTransmission();
    CHECK(receiver.transport.transmissions.size() == 1);
    Frame partial = decodeOne(receiver.transport.transmissions.back()[0]);
    CHECK(partial.type == FrameType::MessagePartialAck);
    CHECK(partial.payload.size() == 1 && partial.payload[0] == 0x01);

    Station listener("VK3ABC");
    listener.protocol.onFrameReceived(decodeOne(broadcast[0]), 5.0f);
    listener.nowMs += 3 * TEXT_FRAGMENT_AIR_MILLISECONDS;
    listener.protocol.tick();
    CHECK(listener.protocol.pendingCount() == 0);

    Station unattended("VK3ABC");
    unattended.protocol.setAutoReplyEnabled(false);
    unattended.protocol.onFrameReceived(decodeOne(addressed[0]), 5.0f);
    unattended.nowMs += 3 * TEXT_FRAGMENT_AIR_MILLISECONDS;
    unattended.protocol.tick();
    CHECK(unattended.protocol.pendingCount() == 0);
}

// A report still waiting to go out must say what is true when it goes: a
// later keying that brings more fragments updates it rather than queuing a
// second, and one that completes the message replaces it with a plain
// acknowledgement.
void testQueuedReportsStayCurrent()
{
    Station sender("W1AW");
    std::string error;
    std::string body(perFragment('D') * 2 + 10, 'D'); // three fragments
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    sender.completeOneTransmission();
    std::vector<std::vector<uint8_t>> frames = sender.transport.transmissions.back();

    // Fragment one, then the keying ends with nothing more heard. Somebody is
    // still on the channel, so the report waits.
    Station receiver("VK3ABC");
    Frame first = decodeOne(frames[0]);
    receiver.protocol.onFrameReceived(first, 5.0f);
    receiver.transport.channelBusy = true;
    receiver.nowMs += (uint64_t)first.burstsFollowing * TEXT_FRAGMENT_AIR_MILLISECONDS;
    receiver.protocol.tick();
    CHECK(receiver.protocol.pendingCount() == 1);
    CHECK(receiver.transport.transmissions.empty());

    // Before the report can go out, another keying brings fragment three.
    receiver.protocol.onFrameReceived(decodeOne(frames[2]), 5.0f);
    receiver.protocol.tick();
    CHECK(receiver.protocol.pendingCount() == 1);
    receiver.transport.channelBusy = false;
    receiver.protocol.tick(); // the channel clears, and its random pause begins
    receiver.completeOneTransmission();
    CHECK(receiver.transport.transmissions.size() == 1);
    Frame report = decodeOne(receiver.transport.transmissions.back()[0]);
    CHECK(report.type == FrameType::MessagePartialAck);
    CHECK(report.payload.size() == 1 && report.payload[0] == 0x05);

    // Fragments one and three again, and the report is queued; then fragment
    // two completes the message before it is sent.
    Station overtaken("VK3ABC");
    overtaken.protocol.onFrameReceived(decodeOne(frames[0]), 5.0f);
    overtaken.protocol.onFrameReceived(decodeOne(frames[2]), 5.0f);
    overtaken.protocol.tick();
    CHECK(overtaken.protocol.pendingCount() == 1);
    CHECK(overtaken.transport.transmissions.empty()); // still in the turnaround
    overtaken.protocol.onFrameReceived(decodeOne(frames[1]), 5.0f);
    CHECK(overtaken.observer.added.size() == 1);
    CHECK(overtaken.protocol.pendingCount() == 1);
    overtaken.completeOneTransmission();
    CHECK(overtaken.transport.transmissions.size() == 1);
    CHECK(decodeOne(overtaken.transport.transmissions.back()[0]).type == FrameType::MessageAck);
}

// A message that gets one new fragment through per keying needs more keyings
// than it has retries. Each of those made progress, so none is a retry, and
// the message is finished rather than failed.
void testProgressDoesNotUseUpRetries()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");

    std::string error;
    std::string body(perFragment('B') * 4 + 10, 'B'); // five fragments
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    int64_t id = sender.observer.added[0].id;
    CHECK(5 > MAX_MESSAGE_RETRIES + 1);

    for (int keying = 0; keying < 5; keying++)
    {
        sender.completeOneTransmission();
        const std::vector<std::vector<uint8_t>>& frames = sender.transport.transmissions.back();
        CHECK((int)frames.size() == 5 - keying);

        // Only the first burst of each keying gets through.
        Frame heard = decodeOne(frames[0]);
        CHECK(heard.fragmentIndex == keying);
        receiver.protocol.onFrameReceived(heard, 5.0f);
        if (keying == 4) break;

        receiver.nowMs += (uint64_t)heard.burstsFollowing * TEXT_FRAGMENT_AIR_MILLISECONDS;
        receiver.protocol.tick();
        receiver.completeOneTransmission();
        sender.receiveFrom(receiver.transport);
    }

    CHECK(receiver.observer.added.size() == 1);
    CHECK(!receiver.observer.added.empty() && receiver.observer.added[0].text == body);
    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport);

    const TextMessage* update = sender.observer.lastUpdateFor(id);
    CHECK(update != nullptr && update->status == MessageStatus::Acknowledged);
    CHECK(!everUpdatedTo(sender.observer, id, MessageStatus::Retrying));
    CHECK(!everUpdatedTo(sender.observer, id, MessageStatus::Failed));
}

// A report that confirms nothing new means the last keying got nothing new
// through: that is a retry, backed off like one, and it resends only what the
// far end has still not confirmed.
void testPartialAckWithNoNewsCountsAsARetry()
{
    Station sender("W1AW");
    std::string error;
    std::string body(perFragment('C') * 2 + 10, 'C'); // three fragments
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    int64_t id = sender.observer.added[0].id;
    sender.completeOneTransmission();

    Frame partial;
    partial.type = FrameType::MessagePartialAck;
    partial.destinationCrc = FrameCodec::callsignHash("W1AW");
    partial.originCallsign = "VK3ABC";
    partial.airId = sender.observer.added[0].airId;
    partial.payload.assign(1, 0x01);

    sender.protocol.onFrameReceived(partial, 5.0f); // news: fragment one arrived
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.back().size() == 2);
    CHECK(!everUpdatedTo(sender.observer, id, MessageStatus::Retrying));

    sender.protocol.onFrameReceived(partial, 5.0f); // the same again: no news
    const TextMessage* update = sender.observer.lastUpdateFor(id);
    CHECK(update != nullptr && update->status == MessageStatus::Retrying);
    CHECK(update != nullptr && update->retryCount == 1);

    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == 3);
    CHECK(sender.transport.transmissions.back().size() == 2);
    CHECK(decodeOne(sender.transport.transmissions.back()[0]).fragmentIndex == 1);

    // A partial acknowledgement from anyone but the addressee changes nothing.
    Frame stranger = partial;
    stranger.originCallsign = "DJ2LS";
    stranger.payload.assign(1, 0x07);
    sender.protocol.onFrameReceived(stranger, 5.0f);
    update = sender.observer.lastUpdateFor(id);
    CHECK(update != nullptr && update->status != MessageStatus::Acknowledged);
}

// Which delivery chip a sent message shows. SENDING and SENT belong to the
// first attempt alone; a retry keeps its number through the whole attempt so
// the chip never appears to go backwards; a message the far end holds part
// of says so, and how much.
// With the console disengaged the transport refuses every keying. The chat
// window asks whether anything is still waiting, to tell the operator to
// engage; it stops asking once the message is on the air.
void testQueuedTransmissionsAreReported()
{
    Station sender("W1AW");
    CHECK(!sender.protocol.hasQueuedTransmissions());

    std::string error;
    sender.transport.refuse = true;
    CHECK(sender.protocol.sendMessage("Anybody there?", "VK3ABC", error));
    sender.nowMs += MAX_TURNAROUND_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.empty());
    CHECK(sender.protocol.hasQueuedTransmissions());
    int64_t id = sender.observer.added[0].id;
    CHECK(sender.protocol.isMessageQueued(id));
    CHECK(!sender.protocol.isMessageQueued(id + 1));

    // Engaged: it goes, and is waiting on an answer, not on the transmitter.
    sender.transport.refuse = false;
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.size() == 1);
    CHECK(!sender.protocol.hasQueuedTransmissions());
    CHECK(!sender.protocol.isMessageQueued(id));
}

void testDeliveryChip()
{
    auto kindOf = [](MessageStatus status, int retry, int confirmed, int count)
    {
        TextMessage message;
        message.status = status;
        message.retryCount = retry;
        message.fragmentsConfirmed = confirmed;
        message.fragmentCount = count;
        return deliveryChipState(message);
    };

    CHECK(kindOf(MessageStatus::Queued, 0, 0, 3).kind == DeliveryChipKind::Queued);

    // Disengaged, a queued message says what it is waiting for. Nothing else
    // changes: a message already sent is past needing the transmitter.
    TextMessage waiting;
    waiting.status = MessageStatus::Queued;
    CHECK(deliveryChipState(waiting, true).kind == DeliveryChipKind::EngageToSend);
    waiting.status = MessageStatus::AwaitingAck;
    CHECK(deliveryChipState(waiting, true).kind == DeliveryChipKind::Sent);
    waiting.status = MessageStatus::Acknowledged;
    CHECK(deliveryChipState(waiting, true).kind == DeliveryChipKind::Acknowledged);
    CHECK(kindOf(MessageStatus::Transmitting, 0, 0, 3).kind == DeliveryChipKind::Sending);
    CHECK(kindOf(MessageStatus::AwaitingAck, 0, 0, 3).kind == DeliveryChipKind::Sent);
    CHECK(kindOf(MessageStatus::Sent, 0, 0, 3).kind == DeliveryChipKind::Sent);
    CHECK(kindOf(MessageStatus::Received, 0, 0, 0).kind == DeliveryChipKind::None);
    CHECK(kindOf(MessageStatus::Acknowledged, 2, 3, 3).kind == DeliveryChipKind::Acknowledged);

    for (MessageStatus status : {MessageStatus::Retrying, MessageStatus::Transmitting,
                                 MessageStatus::AwaitingAck})
    {
        DeliveryChipState retry = kindOf(status, 2, 0, 3);
        CHECK(retry.kind == DeliveryChipKind::Retry);
        CHECK(retry.retry == 2);
        CHECK(!retry.showsProgress());

        // Part delivered, no failed attempt: a resend, with how far it got.
        DeliveryChipState resend = kindOf(status, 0, 5, 8);
        CHECK(resend.kind == DeliveryChipKind::Resend);
        CHECK(resend.showsProgress());
        CHECK(resend.fragmentsConfirmed == 5 && resend.fragmentCount == 8);

        // A failed attempt after progress keeps both.
        DeliveryChipState both = kindOf(status, 1, 5, 8);
        CHECK(both.kind == DeliveryChipKind::Retry);
        CHECK(both.showsProgress());
    }

    // Out of retries part way through still says how far it got.
    DeliveryChipState failed = kindOf(MessageStatus::Failed, 3, 5, 8);
    CHECK(failed.kind == DeliveryChipKind::NotAcknowledged);
    CHECK(failed.showsProgress());

    // Nothing or everything confirmed is not progress worth showing.
    CHECK(!kindOf(MessageStatus::AwaitingAck, 0, 0, 8).showsProgress());
    CHECK(!kindOf(MessageStatus::Acknowledged, 0, 8, 8).showsProgress());
}

// Carrier sense: while the receiver is locked onto somebody else's burst,
// nothing we have queued may start, however long it has been waiting. Once
// it clears, the queue moves again within the random pause a release carries.
void testBusyChannelFreezesTheQueue()
{
    Station sender("W1AW");
    sender.transport.channelBusy = true;

    std::string error;
    CHECK(sender.protocol.sendMessage("Hold it", "VK3ABC", error));

    for (int i = 0; i < 10; i++)
    {
        sender.nowMs += 1000;
        sender.protocol.tick();
    }
    CHECK(sender.transport.transmissions.empty());

    sender.transport.channelBusy = false;
    sender.protocol.tick();
    sender.nowMs += TURNAROUND_JITTER_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 1);
}

// The far end cannot answer us through somebody else's burst, so time spent
// frozen must not count against the acknowledgement: nothing retries during
// it, and the timer resumes rather than expiring the moment the channel clears.
void testBusyChannelHoldsTheAcknowledgementTimer()
{
    Station sender("W1AW");

    std::string error;
    CHECK(sender.protocol.sendMessage("Anybody there", "VK3ABC", error));
    sender.completeOneTransmission();
    int64_t id = sender.observer.added[0].id;

    // Busy for longer than the whole acknowledgement timeout.
    sender.transport.channelBusy = true;
    for (int i = 0; i < 20; i++)
    {
        sender.nowMs += 1000;
        sender.protocol.tick();
    }

    const TextMessage* update = sender.observer.lastUpdateFor(id);
    CHECK(update != nullptr && update->status == MessageStatus::AwaitingAck);
    CHECK(update != nullptr && update->retryCount == 0);

    sender.transport.channelBusy = false;
    sender.protocol.tick();
    update = sender.observer.lastUpdateFor(id);
    CHECK(update != nullptr && update->status == MessageStatus::AwaitingAck);

    sender.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
    sender.protocol.tick();
    update = sender.observer.lastUpdateFor(id);
    CHECK(update != nullptr && update->status == MessageStatus::Retrying);
}

// A third station's burst freezes both ends of our exchange, and both come
// unfrozen at the same moment. The far end is owed its turn to answer first,
// so the reply window has to be held through the busy spell as well; were it
// not, it would already have lapsed and we would key over the late reply.
void testBusyChannelHoldsTheReplyWindow()
{
    Station sender("W1AW");

    std::string error;
    CHECK(sender.protocol.sendMessage("needs an ack", "VK3ABC", error));
    sender.completeOneTransmission();

    sender.transport.channelBusy = true;
    for (int i = 0; i < 10; i++)
    {
        sender.nowMs += 1000;
        sender.protocol.tick();
    }
    sender.transport.channelBusy = false;

    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.nowMs += TURNAROUND_AFTER_TX_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 1);

    sender.nowMs += REPLY_WINDOW_MILLISECONDS;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 2);
}

// A receiver that never drops sync is false triggering on noise, not hearing a
// transmission. It must not be able to silence the station for good.
void testChannelThatNeverClearsIsEventuallyIgnored()
{
    Station sender("W1AW");
    sender.transport.channelBusy = true;

    std::string error;
    CHECK(sender.protocol.sendMessage("Still here", "VK3ABC", error));

    sender.protocol.tick();
    sender.nowMs += MAX_CHANNEL_BUSY_MILLISECONDS - 1000;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.empty());

    sender.nowMs += 2000;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 1);
}

void testSendRequiresCallsign()
{
    MessageStore store;
    CHECK(store.open(":memory:"));
    HeardStationList stations;
    TextMessagingProtocol protocol(store, stations);
    FakeTransport transport;
    protocol.setTransport(&transport);

    std::string error;
    CHECK(!protocol.sendMessage("Hello", "VK3ABC", error));
    CHECK(!error.empty());

    protocol.setMyCallsign("W1AW");
    error.clear();
    CHECK(!protocol.sendMessage("   ", "VK3ABC", error));
    CHECK(!error.empty());
    CHECK(!protocol.sendPing("", error));
}

} // namespace

// The countdown on a queued message's chip: each waits for everything
// ahead of it, ticks down while that goes on the air, and says when the
// channel is somebody else's.
void testQueuedWaitsCountDown()
{
    Station sender("W1AW");
    std::string error;
    CHECK(sender.protocol.sendMessage("one", "K1ABC", error));
    CHECK(sender.protocol.sendMessage("two", "", error));
    CHECK(sender.protocol.sendPing("K1ABC", error));
    AirTiming timing = sender.protocol.airTiming();

    std::vector<QueuedWait> waits = sender.protocol.queuedWaits();
    CHECK(waits.size() == 2); // the ping has no chip
    CHECK(waits[0].messageId == sender.observer.added[0].id);
    CHECK(waits[1].messageId == sender.observer.added[1].id);
    CHECK(!waits[0].channelBusy);
    // "one" asks for an acknowledgement, so "two" waits for its air time,
    // the turnaround and the far end's turn.
    CHECK(waits[1].waitMs - waits[0].waitMs >=
          (int64_t)timing.textFragmentAirMs + timing.turnaroundAfterTxMs + timing.replyWindowMs);

    // "one" goes on the air; "two" counts down with it.
    sender.nowMs += std::max(MAX_TURNAROUND_MILLISECONDS, MAX_RETRY_BACKOFF_MILLISECONDS) + 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmitting);
    waits = sender.protocol.queuedWaits();
    CHECK(waits.size() == 1);
    CHECK(waits[0].messageId == sender.observer.added[1].id);
    int64_t onAir = waits[0].waitMs;
    CHECK(onAir >= (int64_t)timing.textFragmentAirMs + timing.turnaroundAfterTxMs + timing.replyWindowMs);
    sender.nowMs += 1000;
    sender.protocol.tick();
    waits = sender.protocol.queuedWaits();
    CHECK(waits.size() == 1 && waits[0].waitMs == onAir - 1000);

    // Somebody else on the channel.
    sender.transport.channelBusy = true;
    sender.protocol.tick();
    waits = sender.protocol.queuedWaits();
    CHECK(waits.size() == 1 && waits[0].channelBusy);
}

// "Woah!": the operator hears somebody the receiver missed. Nothing keys,
// a reply included, until a frame's air time has passed, and each press
// adds another.
void testWoahHoldsTheQueue()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");
    AirTiming timing = sender.protocol.airTiming();
    timing.frameAirMs = 60 * 1000; // well past every turnaround
    sender.protocol.setAirTiming(timing);
    receiver.protocol.setAirTiming(timing);
    uint64_t frame = (uint64_t)timing.frameAirMs;

    std::string error;
    CHECK(sender.protocol.sendMessage("one", "VK3ABC", error));
    CHECK(sender.protocol.holdTransmissions() == frame);
    CHECK(sender.protocol.holdTransmissions() == 2 * frame);
    std::vector<QueuedWait> waits = sender.protocol.queuedWaits();
    CHECK(waits.size() == 1 && waits[0].waitMs == (int64_t)(2 * frame));

    // The turnarounds alone would have let it go by now.
    sender.nowMs += 2 * frame - 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.empty());
    sender.nowMs += 1;
    sender.protocol.tick();
    CHECK(sender.transport.transmissions.size() == 1);
    sender.transport.transmitting = false;
    sender.protocol.tick();

    // The acknowledgement waits too.
    receiver.receiveFrom(sender.transport);
    CHECK(receiver.protocol.holdTransmissions() == frame);
    receiver.nowMs += frame - 1;
    receiver.protocol.tick();
    CHECK(receiver.transport.transmissions.empty());
    receiver.nowMs += 1;
    receiver.protocol.tick();
    CHECK(receiver.transport.transmissions.size() == 1);
}

// A queued message moved to a tempo of its own: its bursts say so, the
// countdowns behind it allow for the longer air time, it keys on its own
// rather than behind a reply, and once on the air it can no longer move.
void testQueuedMessageTakesATempoOfItsOwn()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");
    AirTiming timing = sender.protocol.airTiming();

    std::string error;
    CHECK(receiver.protocol.sendMessage("mine", "W1AW", error));
    int64_t mine = receiver.observer.added[0].id;
    CHECK(receiver.protocol.sendMessage("after", "", error));
    std::vector<QueuedWait> waits = receiver.protocol.queuedWaits();
    CHECK(waits.size() == 2 && waits[0].gear == 0);
    int64_t gap = waits[1].waitMs - waits[0].waitMs;

    CHECK(receiver.protocol.setMessageTempo(mine, 1));
    waits = receiver.protocol.queuedWaits();
    CHECK(waits.size() == 2 && waits[0].gear == 1 && waits[1].gear == 0);
    CHECK(waits[1].waitMs - waits[0].waitMs == gap + 7 * (int64_t)timing.textFragmentAirMs);
    CHECK(!receiver.protocol.setMessageTempo(12345, 2)); // not ours
    CHECK(receiver.protocol.setMessageTempo(mine, 0));
    CHECK(receiver.protocol.queuedWaits()[0].gear == 0);
    CHECK(receiver.protocol.setMessageTempo(mine, 1));

    // The acknowledgement it would have ridden behind goes alone, at the
    // tempo set now, and the broadcast behind it does not ride instead.
    CHECK(sender.protocol.sendMessage("first", "VK3ABC", error));
    sender.completeOneTransmission();
    receiver.receiveFrom(sender.transport);
    receiver.completeOneTransmission();
    CHECK(receiver.transport.transmissions.size() == 1);
    CHECK(receiver.transport.modes.back().size() == 1 &&
          receiver.transport.modes.back()[0] == BurstMode::Signalling);
    CHECK(receiver.transport.gears.back()[0] == 0);

    // Then it keys at its own.
    for (int i = 0; i < 20 && receiver.transport.transmissions.size() < 2; i++)
    {
        receiver.nowMs += 5000;
        receiver.protocol.tick();
    }
    CHECK(receiver.transport.transmissions.size() == 2);
    CHECK(receiver.transport.gears.back().size() == 1 && receiver.transport.gears.back()[0] == 1);
    CHECK(decodeOne(receiver.transport.transmissions.back()[0]).type == FrameType::Message);
    CHECK(!receiver.protocol.setMessageTempo(mine, 3));
}

// What a station's store says about another station's locator.
StationLocator storedLocator(MessageStore& store, const std::string& callsign)
{
    for (const StationLocator& station : store.stationLocators())
    {
        if (station.callsign == callsign) return station;
    }
    return StationLocator();
}

// As an earlier session would have left it: the station takes locators.
void knowsLocators(Station& station, const std::string& callsign)
{
    StationLocator known;
    known.callsign = callsign;
    known.support = LocatorSupport::Yes;
    station.protocol.restoreStationLocators({known});
}

// How long a station with traffic queued holds off after hearing a keying.
uint64_t keyedAfter(Station& station, uint64_t heardAt, uint64_t limit)
{
    for (station.nowMs = heardAt; station.nowMs <= heardAt + limit; station.nowMs += 100)
    {
        station.protocol.tick();
        if (!station.transport.transmissions.empty()) return station.nowMs - heardAt;
    }
    return UINT64_MAX;
}

// Our locator rides only to a station that has said, in an acknowledgement
// or a ping, that it takes locator frames: Glissando 0.5 would hold its
// answer for a whole text burst after the message. It rides last, counted
// among the bursts that follow the message, until the station says it has it.
void testLocatorRidesToAStationThatTakesIt()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");
    sender.protocol.setMyLocator(" cn87UX ", true);
    receiver.protocol.setMyLocator("QF22", true);
    CHECK(sender.protocol.myLocator() == "CN87ux");

    std::string error;
    CHECK(sender.protocol.sendMessage("Hello", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions[0].size() == 1); // nothing known of it yet
    receiver.receiveFrom(sender.transport);
    receiver.completeOneTransmission();

    // Its acknowledgement says it takes locators, and has not had ours.
    Frame ack = decodeOne(receiver.transport.transmissions[0][0]);
    CHECK(ack.type == FrameType::MessageAck);
    CHECK(ack.features == FEATURE_UNDERSTANDS_LOCATOR);
    sender.receiveFrom(receiver.transport);
    CHECK(storedLocator(sender.store, "VK3ABC").support == LocatorSupport::Yes);

    CHECK(sender.protocol.sendMessage("Where are you?", "VK3ABC", error));
    sender.completeOneTransmission();
    const std::vector<std::vector<uint8_t>>& keying = sender.transport.transmissions[1];
    CHECK(keying.size() == 2);
    CHECK(sender.transport.modes[1].size() == 2 && sender.transport.modes[1][1] == BurstMode::Signalling);
    Frame message = decodeOne(keying[0]);
    Frame locator = decodeOne(keying[1]);
    CHECK(message.type == FrameType::Message && message.burstsFollowing == 1);
    CHECK(locator.type == FrameType::Locator);
    CHECK(locator.locator == "CN87");
    CHECK(locator.originCallsign == "W1AW");
    CHECK(locator.destinationCrc == FrameCodec::callsignHash("VK3ABC"));
    CHECK(locator.burstsFollowing == 0);

    // The station keeps it, and says so in its acknowledgement.
    receiver.receiveFrom(sender.transport);
    CHECK(receiver.protocol.stationLocator("w1aw") == "CN87");
    StationLocator stored = storedLocator(receiver.store, "W1AW");
    CHECK(stored.gridSquare == "CN87" && stored.support == LocatorSupport::Yes);
    receiver.completeOneTransmission();
    ack = decodeOne(receiver.transport.transmissions[1][0]);
    CHECK(ack.features == (FEATURE_UNDERSTANDS_LOCATOR | FEATURE_HEARD_YOUR_LOCATOR));
    sender.receiveFrom(receiver.transport);

    // Which is the end of it for this contact.
    CHECK(sender.protocol.sendMessage("Nice", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions[2].size() == 1);
}

// A locator that is lost leaves the acknowledgement without the bit, and
// ours rides again behind the next message.
void testLocatorRidesUntilTheStationHasIt()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");
    sender.protocol.setMyLocator("CN87", true);
    knowsLocators(sender, "VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("One", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions[0].size() == 2);

    receiver.protocol.onFrameReceived(decodeOne(sender.transport.transmissions[0][0]), 5.0f);
    receiver.nowMs += TEXT_FRAGMENT_AIR_MILLISECONDS; // it waits out the burst it missed
    receiver.completeOneTransmission();
    CHECK(decodeOne(receiver.transport.transmissions[0][0]).features == FEATURE_UNDERSTANDS_LOCATOR);
    sender.receiveFrom(receiver.transport);
    CHECK(sender.observer.lastUpdateFor(sender.observer.added[0].id)->status == MessageStatus::Acknowledged);

    CHECK(sender.protocol.sendMessage("Two", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions[1].size() == 2);
    receiver.receiveFrom(sender.transport);
    receiver.completeOneTransmission();
    CHECK((decodeOne(receiver.transport.transmissions[1][0]).features & FEATURE_HEARD_YOUR_LOCATOR) != 0);
    sender.receiveFrom(receiver.transport);

    CHECK(sender.protocol.sendMessage("Three", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions[2].size() == 1);

    // A changed locator goes again, behind the last fragment of a longer
    // message.
    std::string body(perFragment('A') + 10, 'A'); // two fragments
    CHECK(sender.protocol.sendMessage(body, "VK3ABC", error));
    sender.protocol.setMyLocator("CN88", true);
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions[3].size() == 3);
    CHECK(decodeOne(sender.transport.transmissions[3][0]).burstsFollowing == 2);
    CHECK(decodeOne(sender.transport.transmissions[3][2]).locator == "CN88");
}

// A station with Auto acknowledge off never says it has our locator, so it
// gets it once a contact. A contact ends after a long enough silence, and a
// changed locator goes again to everybody.
void testLocatorGoesOnceToAStationWithAutoAckOff()
{
    Station sender("W1AW");
    Station receiver("VK3ABC");
    sender.protocol.setMyLocator("CN87", true);
    knowsLocators(sender, "VK3ABC");
    receiver.protocol.setAutoReplyEnabled(false);

    std::string error;
    CHECK(receiver.protocol.sendMessage("Listening only", "W1AW", error));
    receiver.completeOneTransmission();
    sender.receiveFrom(receiver.transport);
    CHECK(!sender.protocol.stationAutoAcks("VK3ABC"));
    sender.completeOneTransmission(); // our acknowledgement of it carries nothing
    size_t keyings = sender.transport.transmissions.size();
    CHECK(sender.transport.transmissions.back().size() == 1);

    auto sendAndCount = [&](const std::string& text) -> size_t
    {
        CHECK(sender.protocol.sendMessage(text, "VK3ABC", error));
        sender.completeOneTransmission();
        return sender.transport.transmissions.back().size();
    };

    CHECK(sendAndCount("One") == 2);
    CHECK(sendAndCount("Two") == 1);

    sender.nowMs += LOCATOR_CONTACT_IDLE_MILLISECONDS + 1;
    CHECK(sendAndCount("Three") == 2);
    CHECK(sendAndCount("Four") == 1);

    // The same grid square is no news; another one is.
    sender.protocol.setMyLocator("CN87ab", true);
    CHECK(sendAndCount("Five") == 1);
    sender.protocol.setMyLocator("CN88", true);
    CHECK(sendAndCount("Six") == 2);

    // Turned off, or not a locator, nothing goes.
    sender.protocol.setMyLocator("CN97", false);
    CHECK(sendAndCount("Seven") == 1);
    CHECK(sender.transport.fillers.back().empty());
    sender.protocol.setMyLocator("ZZ99", true);
    CHECK(sender.protocol.myLocator().empty());
    CHECK(sendAndCount("Eight") == 1);
    CHECK(sender.transport.transmissions.size() == keyings + 8);
}

// A station that pings or acknowledges without the feature bit is an older
// build, whatever an earlier session thought of it.
void testOlderStationGetsNoLocator()
{
    Station sender("W1AW");
    sender.protocol.setMyLocator("CN87", true);
    knowsLocators(sender, "VK3ABC");

    Frame ping;
    ping.type = FrameType::Ping;
    ping.destinationCrc = FrameCodec::callsignHash("W1AW");
    ping.originCallsign = "VK3ABC";
    ping.airId = 7;
    ping.senderAutoAck = true;
    sender.protocol.onFrameReceived(ping, 5.0f);
    CHECK(storedLocator(sender.store, "VK3ABC").support == LocatorSupport::No);
    sender.completeOneTransmission(); // the pong

    std::string error;
    CHECK(sender.protocol.sendMessage("Hello", "VK3ABC", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.back().size() == 1);

    // Our own pings say we take locators.
    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.completeOneTransmission();
    Frame ours = decodeOne(sender.transport.transmissions.back()[0]);
    CHECK(ours.type == FrameType::Ping && ours.features == FEATURE_UNDERSTANDS_LOCATOR);

    // What the store kept comes back in the next session.
    Station later("W1AW");
    later.protocol.restoreStationLocators(sender.store.stationLocators());
    StationLocator heard;
    heard.callsign = "DJ2LS";
    heard.gridSquare = "JO62";
    heard.support = LocatorSupport::Yes;
    later.protocol.restoreStationLocators({heard});
    CHECK(later.protocol.stationLocator("DJ2LS") == "JO62");
    CHECK(later.protocol.stationLocator("K1ABC").empty());

    // A square kept from an earlier session is not current: the station
    // may have moved. Heard again it is, until its contact is over.
    CHECK(!later.protocol.stationLocatorIsCurrent("DJ2LS"));
    CHECK(!later.protocol.stationLocatorIsCurrent("K1ABC"));
    Frame locator;
    locator.type = FrameType::Locator;
    locator.originCallsign = "DJ2LS";
    locator.locator = "JO43";
    later.protocol.onFrameReceived(locator, 5.0f);
    CHECK(later.protocol.stationLocator("dj2ls") == "JO43");
    CHECK(later.protocol.stationLocatorIsCurrent("dj2ls"));
    later.nowMs += LOCATOR_CONTACT_IDLE_MILLISECONDS + 1;
    CHECK(!later.protocol.stationLocatorIsCurrent("DJ2LS"));
    CHECK(later.protocol.stationLocator("DJ2LS") == "JO43");
}

// The locator ends the sender's keying. Whoever hears it lets go of the
// channel then, rather than a whole text burst after the message; whoever
// misses it waits that out, and so the sender waits that much longer for
// its acknowledgement.
void testLocatorEndsTheKeying()
{
    Station sender("W1AW");
    sender.protocol.setMyLocator("CN87", true);
    knowsLocators(sender, "VK3ABC");

    std::string error;
    CHECK(sender.protocol.sendMessage("Hello", "VK3ABC", error));
    int64_t id = sender.observer.added[0].id;
    sender.completeOneTransmission();
    const std::vector<std::vector<uint8_t>>& frames = sender.transport.transmissions[0];
    CHECK(frames.size() == 2);

    Station bystander("DJ2LS");
    CHECK(bystander.protocol.sendMessage("waiting", "W1AW", error));
    uint64_t heardAt = bystander.nowMs;
    bystander.receiveFrom(sender.transport);
    CHECK(bystander.protocol.stationLocator("W1AW") == "CN87");
    uint64_t released = keyedAfter(bystander, heardAt, 4 * TEXT_FRAGMENT_AIR_MILLISECONDS);

    Station lossy("DJ2LS");
    CHECK(lossy.protocol.sendMessage("waiting", "W1AW", error));
    heardAt = lossy.nowMs;
    lossy.protocol.onFrameReceived(decodeOne(frames[0]), 5.0f);
    uint64_t held = keyedAfter(lossy, heardAt, 4 * TEXT_FRAGMENT_AIR_MILLISECONDS);
    CHECK(held >= (uint64_t)TEXT_FRAGMENT_AIR_MILLISECONDS);
    CHECK(released < (uint64_t)TEXT_FRAGMENT_AIR_MILLISECONDS);

    // The addressee answers once the keying is over.
    Station receiver("VK3ABC");
    heardAt = receiver.nowMs;
    receiver.receiveFrom(sender.transport);
    CHECK(keyedAfter(receiver, heardAt, 4 * TEXT_FRAGMENT_AIR_MILLISECONDS) <
          (uint64_t)TEXT_FRAGMENT_AIR_MILLISECONDS);

    // And one that lost a fragment asks for it then.
    Station longSender("W1AW");
    longSender.protocol.setMyLocator("CN87", true);
    knowsLocators(longSender, "VK3ABC");
    CHECK(longSender.protocol.sendMessage(std::string(perFragment('A') + 10, 'A'), "VK3ABC", error));
    longSender.completeOneTransmission();
    const std::vector<std::vector<uint8_t>>& longFrames = longSender.transport.transmissions[0];
    CHECK(longFrames.size() == 3);
    Station gappy("VK3ABC");
    heardAt = gappy.nowMs;
    gappy.protocol.onFrameReceived(decodeOne(longFrames[0]), 5.0f);
    gappy.protocol.onFrameReceived(decodeOne(longFrames[2]), 5.0f);
    CHECK(keyedAfter(gappy, heardAt, 4 * TEXT_FRAGMENT_AIR_MILLISECONDS) < (uint64_t)TEXT_FRAGMENT_AIR_MILLISECONDS);
    CHECK(decodeOne(gappy.transport.transmissions[0][0]).type == FrameType::MessagePartialAck);

    sender.nowMs += ACK_TIMEOUT_MILLISECONDS + 1;
    sender.protocol.tick();
    CHECK(sender.observer.lastUpdateFor(id)->status == MessageStatus::AwaitingAck);
    sender.nowMs += TEXT_FRAGMENT_AIR_MILLISECONDS;
    sender.protocol.tick();
    CHECK(sender.observer.lastUpdateFor(id)->status == MessageStatus::Retrying);
}

// A duet keying with a voice to spare sings our locator in it, to anybody,
// whatever else the keying holds, in the short form that fits one nine-byte
// segment from any callsign.
void testDuetFillerCarriesTheLocator()
{
    auto oneSegment = [](const std::vector<uint8_t>& filler) {
        return filler.size() > 9 && std::all_of(filler.begin() + 9, filler.end(), [](uint8_t b) { return b == 0; });
    };

    Station sender("W1AW");
    std::string error;
    CHECK(sender.protocol.sendMessage("CQ", "", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.fillers.back().empty()); // no locator set

    sender.protocol.setMyLocator("CN87", true);
    CHECK(sender.protocol.sendMessage("CQ again", "", error));
    sender.completeOneTransmission();
    CHECK(sender.transport.transmissions.back().size() == 1); // a broadcast carries no rider
    const std::vector<uint8_t>& filler = sender.transport.fillers.back();
    CHECK((filler[0] >> 4) == 0xF);
    Frame frame = decodeOne(filler);
    CHECK(frame.type == FrameType::Locator && frame.locator == "CN87");
    CHECK(frame.originCallsign == "W1AW" && frame.destinationCrc == 0 && frame.burstsFollowing == 0);
    CHECK(oneSegment(filler));

    CHECK(sender.protocol.sendPing("VK3ABC", error));
    sender.completeOneTransmission();
    frame = decodeOne(sender.transport.fillers.back());
    CHECK(frame.type == FrameType::Locator && frame.locator == "CN87" && frame.destinationCrc == 0);

    // A portable call is too long for the full form in one segment, but the
    // short form fits, and whoever hears it books it.
    CHECK(!FrameCodec::isStandardCallsign("TEST1/P"));
    Station portable("TEST1/P");
    portable.protocol.setMyLocator("CN87", true);
    CHECK(portable.protocol.sendMessage("CQ", "", error));
    portable.completeOneTransmission();
    const std::vector<uint8_t>& portableFiller = portable.transport.fillers.back();
    CHECK(oneSegment(portableFiller));
    frame = decodeOne(portableFiller);
    CHECK(frame.type == FrameType::Locator && frame.originCallsign == "TEST1/P" && frame.locator == "CN87");
    Frame full = frame;
    full.destinationCrc = 0;
    CHECK(!oneSegment(FrameCodec::encode(full, SIGNALLING_FRAME_BYTES)));

    Station listener("TEST2/P");
    listener.protocol.onFrameReceived(frame, 5.0f);
    CHECK(listener.protocol.stationLocator("TEST1/P") == "CN87");
    CHECK(listener.protocol.mapStation() == "TEST1/P");
}

// The console's map follows the last station heard or keyed to whose
// locator we know, and stays put for one whose locator we do not, until the
// operator picks a station in the list.
void testMapFollowsStationsWithKnownLocators()
{
    Station station("W1AW");
    CHECK(station.protocol.mapStation().empty());

    StationLocator known;
    known.callsign = "VK3ABC";
    known.gridSquare = "QF22";
    known.support = LocatorSupport::Yes;
    station.protocol.restoreStationLocators({known});
    CHECK(station.protocol.mapStation().empty()); // not heard yet

    auto hear = [&](const std::string& from, FrameType type) {
        Frame frame;
        frame.type = type;
        frame.destinationCrc = FrameCodec::callsignHash("K1ABC");
        frame.originCallsign = from;
        frame.airId = 9;
        frame.senderAutoAck = true;
        if (type == FrameType::Locator) frame.locator = "JO62";
        station.protocol.onFrameReceived(frame, 5.0f);
    };

    hear("VK3ABC", FrameType::Ping);
    CHECK(station.protocol.mapStation() == "VK3ABC");

    hear("DJ2LS", FrameType::Ping);                 // its locator unknown: stay put
    CHECK(station.protocol.mapStation() == "VK3ABC");

    hear("DJ2LS", FrameType::Locator);              // now it is known
    CHECK(station.protocol.mapStation() == "DJ2LS");
    CHECK(station.protocol.stationLocator(station.protocol.mapStation()) == "JO62");

    // Keying to a station with a known locator turns the map to it.
    std::string error;
    CHECK(station.protocol.sendPing("VK3ABC", error));
    station.completeOneTransmission();
    CHECK(station.protocol.mapStation() == "VK3ABC");

    // And to one without, leaves it.
    CHECK(station.protocol.sendPing("G0ABC", error));
    station.completeOneTransmission();
    CHECK(station.protocol.mapStation() == "VK3ABC");

    // Once the operator picks a station in the list, the map follows the
    // pick rather than the traffic, its locator known or not.
    station.protocol.setMapSelection("dj2ls");
    CHECK(station.protocol.mapStation() == "DJ2LS");
    hear("VK3ABC", FrameType::Ping);
    CHECK(station.protocol.mapStation() == "DJ2LS");
    station.protocol.setMapSelection("G0ABC");
    CHECK(station.protocol.mapStation() == "G0ABC");
    CHECK(station.protocol.stationLocator(station.protocol.mapStation()).empty());

    // With the pick cleared it shows none, whatever is heard or keyed to,
    // until another is picked.
    station.protocol.setMapSelection("");
    CHECK(station.protocol.mapStation().empty());
    hear("DJ2LS", FrameType::Locator);
    CHECK(station.protocol.sendPing("VK3ABC", error));
    station.completeOneTransmission();
    CHECK(station.protocol.mapStation().empty());
    station.protocol.setMapSelection("VK3ABC");
    CHECK(station.protocol.mapStation() == "VK3ABC");
}

// A message for a station a link that acknowledges by itself reaches (a
// Data2G session) goes to that link as soon as it is queued, whatever our
// turn taking says, and the link's report settles it: no acknowledgement
// timer, and nothing for the far end's own acknowledgement to do.
void testReliableLinkDeliversAMessage()
{
    Station a("W1AW");
    a.transport.reliableTo.insert("K1ABC");

    // Our own turn taking would hold an ordinary keying back now.
    Frame heard;
    heard.type = FrameType::Broadcast;
    heard.originCallsign = "N0CALL";
    heard.airId = 9;
    heard.fragmentCount = 1;
    heard.burstsFollowing = 3; // reserves the channel
    a.protocol.onFrameReceived(heard, 5.0f);
    a.transport.channelBusy = true;

    std::string error;
    CHECK(a.protocol.sendMessage("hello", "K1ABC", error));
    a.protocol.tick();

    CHECK(a.transport.transmissions.empty());
    CHECK(a.transport.reliable.size() == 1);
    CHECK(!a.transport.transmitting); // the transmitter was never asked for
    int64_t id = a.observer.added.back().id;
    const TextMessage* sending = a.observer.lastUpdateFor(id);
    CHECK(sending != nullptr && sending->status == MessageStatus::Transmitting);

    // Long past any acknowledgement timer: still the link's to settle.
    a.nowMs += 10 * 60 * 1000;
    a.protocol.tick();
    CHECK(a.protocol.ackWait() == AckWait::Nothing);
    CHECK(!everUpdatedTo(a.observer, id, MessageStatus::AwaitingAck));
    CHECK(!everUpdatedTo(a.observer, id, MessageStatus::Retrying));

    a.transport.reports.push_back({a.transport.reliableKeyings[0], KeyingReport::Result::Delivered});
    a.protocol.tick();
    const TextMessage* done = a.observer.lastUpdateFor(id);
    CHECK(done != nullptr && done->status == MessageStatus::Acknowledged);
    CHECK(done != nullptr && done->fragmentsConfirmed == 1);
    CHECK(!a.protocol.hasQueuedTransmissions());
}

// Several messages for the station go to the link back to back, with no
// turnaround or reply window between them, and each is settled on its own.
void testReliableLinkTakesMessagesBackToBack()
{
    Station a("W1AW");
    a.transport.reliableTo.insert("K1ABC");

    std::string error;
    CHECK(a.protocol.sendMessage("one", "K1ABC", error));
    int64_t first = a.observer.added.back().id;
    CHECK(a.protocol.sendMessage("two", "K1ABC", error));
    int64_t second = a.observer.added.back().id;
    a.protocol.tick();
    CHECK(a.transport.reliable.size() == 2);

    a.transport.reports.push_back({a.transport.reliableKeyings[1], KeyingReport::Result::Delivered});
    a.protocol.tick();
    CHECK(a.observer.lastUpdateFor(second)->status == MessageStatus::Acknowledged);
    CHECK(a.observer.lastUpdateFor(first)->status == MessageStatus::Transmitting);

    a.transport.reports.push_back({a.transport.reliableKeyings[0], KeyingReport::Result::Delivered});
    a.protocol.tick();
    CHECK(a.observer.lastUpdateFor(first)->status == MessageStatus::Acknowledged);
}

// The link losing the far end fails the message: it has already retried for
// longer than we would. A ping fails the same way, and one delivered is the
// pong.
void testReliableLinkFailureEndsTheMessage()
{
    Station a("W1AW");
    a.transport.reliableTo.insert("K1ABC");

    std::string error;
    CHECK(a.protocol.sendMessage("hello", "K1ABC", error));
    int64_t message = a.observer.added.back().id;
    CHECK(a.protocol.sendPing("K1ABC", error));
    a.protocol.tick();
    CHECK(a.transport.reliable.size() == 2);

    a.transport.reports.push_back({a.transport.reliableKeyings[0], KeyingReport::Result::Failed});
    a.transport.reports.push_back({a.transport.reliableKeyings[1], KeyingReport::Result::Delivered});
    a.protocol.tick();

    CHECK(a.observer.lastUpdateFor(message)->status == MessageStatus::Failed);
    CHECK(!everUpdatedTo(a.observer, message, MessageStatus::Retrying));
    CHECK(hasSystemLine(a.observer, "PING delivered"));
    CHECK(!a.protocol.hasQueuedTransmissions());
    CHECK(a.transport.transmissions.empty());
}

// A link that never got through to the station gives the message back, and
// it goes the ordinary way, acknowledgement and all.
void testMessageTheLinkDidNotTakeGoesTheOrdinaryWay()
{
    Station a("W1AW");
    Station b("K1ABC");
    a.transport.reliableTo.insert("K1ABC");

    std::string error;
    CHECK(a.protocol.sendMessage("hello", "K1ABC", error));
    int64_t id = a.observer.added.back().id;
    a.protocol.tick();
    CHECK(a.transport.reliable.size() == 1);

    a.transport.reliableTo.clear();
    a.transport.reports.push_back({a.transport.reliableKeyings[0], KeyingReport::Result::NotTaken});
    a.protocol.tick();
    CHECK(everUpdatedTo(a.observer, id, MessageStatus::Queued));

    a.completeOneTransmission();
    CHECK(a.transport.transmissions.size() == 1);
    CHECK(a.observer.lastUpdateFor(id)->status == MessageStatus::AwaitingAck);

    b.receiveFrom(a.transport);
    b.completeOneTransmission();
    a.receiveFrom(b.transport);
    CHECK(a.observer.lastUpdateFor(id)->status == MessageStatus::Acknowledged);
}

// Deselecting a station with a link to it drops everything of ours still
// outstanding for it, on the air, waiting or queued, and nothing for any
// other station. Over our own modem deselecting changes nothing.
void testReleasingAStationDropsWhatIsOutstanding()
{
    {
        Station a("W1AW");
        std::string error;
        CHECK(a.protocol.sendMessage("hello", "K1ABC", error));
        int64_t id = a.observer.added.back().id;
        a.completeOneTransmission();
        CHECK(a.protocol.releaseStation("K1ABC") == 0);
        CHECK(a.observer.lastUpdateFor(id)->status == MessageStatus::AwaitingAck);
    }

    Station a("W1AW");
    a.transport.releases = true;
    a.transport.reliableTo.insert("K1ABC");

    std::string error;
    CHECK(a.protocol.sendMessage("first", "K1ABC", error));
    int64_t delivering = a.observer.added.back().id;
    a.protocol.tick();
    CHECK(a.transport.reliable.size() == 1);
    CHECK(a.protocol.sendPing("K1ABC", error));
    int64_t ping = a.observer.added.back().id;
    CHECK(a.protocol.sendMessage("for somebody else", "N0CALL", error));
    int64_t other = a.observer.added.back().id;

    CHECK(a.protocol.releaseStation("K1ABC") == 2);
    CHECK(a.transport.released == std::vector<std::string>{"K1ABC"});
    CHECK(a.observer.lastUpdateFor(delivering)->status == MessageStatus::Aborted);
    CHECK(a.observer.lastUpdateFor(ping)->status != MessageStatus::Acknowledged);
    CHECK(everUpdatedTo(a.observer, ping, MessageStatus::NotSent) ||
          everUpdatedTo(a.observer, ping, MessageStatus::Aborted));
    CHECK(a.protocol.cancelFor(other) != TextMessagingProtocol::Cancel::None);

    // A late report on the dropped keying changes nothing.
    a.transport.reports.push_back({a.transport.reliableKeyings[0], KeyingReport::Result::Delivered});
    a.protocol.tick();
    CHECK(a.observer.lastUpdateFor(delivering)->status == MessageStatus::Aborted);
}

// What arrives through such a link has been acknowledged by it: no
// acknowledgement, pong or report of missing fragments of ours, whatever
// the Auto acknowledge setting, and nothing held up on the channel.
void testFramesThroughAReliableLinkAreNotAnswered()
{
    Station a("W1AW");
    Station b("K1ABC");
    b.transport.reliableTo.insert("W1AW");

    std::string error;
    CHECK(b.protocol.sendMessage(std::string(perFragment('x') + 3, 'x'), "W1AW", error));
    CHECK(b.protocol.sendPing("W1AW", error));
    b.protocol.tick();
    CHECK(b.transport.reliable.size() == 2);

    // Only the first fragment of the message, then the ping.
    Frame first = decodeOne(b.transport.reliable[0][0]);
    CHECK(first.burstsFollowing == 1);
    a.protocol.onFrameReceived(first, NAN, true);
    a.protocol.onFrameReceived(decodeOne(b.transport.reliable[1][0]), NAN, true);
    CHECK(hasSystemLine(a.observer, "PING!"));

    for (int i = 0; i < 10; i++)
    {
        a.nowMs += 2000;
        a.protocol.tick();
    }
    CHECK(a.transport.transmissions.empty());
    CHECK(!a.protocol.hasQueuedTransmissions());

    // The rest of the message completes it, still unanswered.
    a.protocol.onFrameReceived(decodeOne(b.transport.reliable[0][1]), NAN, true);
    bool received = std::any_of(a.observer.added.begin(), a.observer.added.end(), [](const TextMessage& m)
                                { return m.direction == MessageDirection::Received && m.kind == MessageKind::Chat; });
    CHECK(received);
    a.protocol.tick();
    CHECK(a.transport.transmissions.empty());
    CHECK(!a.protocol.hasQueuedTransmissions());

    // A message of ours goes at once: the frames just heard hold nothing up.
    CHECK(a.protocol.sendMessage("back", "N0CALL", error));
    a.protocol.tick();
    CHECK(a.transport.transmissions.size() == 1);
}

// A message for the link shows no tempo or wait, and one for anybody else
// counts down as before.
void testQueuedWaitsLeaveTheLinkOut()
{
    Station a("W1AW");
    a.transport.reliableTo.insert("K1ABC");
    a.transport.transmitting = true; // our own keying holds the queue

    std::string error;
    CHECK(a.protocol.sendMessage("other", "N0CALL", error));
    int64_t other = a.observer.added.back().id;
    a.protocol.holdTransmissions(); // "Woah!" holds the link too
    CHECK(a.protocol.sendMessage("hello", "K1ABC", error));
    int64_t linked = a.observer.added.back().id;
    a.protocol.tick();
    CHECK(a.transport.reliable.empty());

    std::vector<QueuedWait> waits = a.protocol.queuedWaits();
    CHECK(waits.size() == 2);
    for (const QueuedWait& wait : waits)
    {
        if (wait.messageId == linked) CHECK(wait.reliableLink && wait.waitMs == 0);
        if (wait.messageId == other) CHECK(!wait.reliableLink);
    }
}

// Changing transport while the link holds a message gives it back to the
// queue, since that link will never report on it.
void testChangingTransportRequeuesWhatTheLinkHeld()
{
    Station a("W1AW");
    a.transport.reliableTo.insert("K1ABC");

    std::string error;
    CHECK(a.protocol.sendMessage("hello", "K1ABC", error));
    int64_t id = a.observer.added.back().id;
    a.protocol.tick();
    CHECK(a.transport.reliable.size() == 1);

    FakeTransport plain;
    a.protocol.setTransport(&plain);
    a.nowMs += MAX_TURNAROUND_MILLISECONDS + 1;
    a.protocol.tick();
    CHECK(plain.transmissions.size() == 1);
    CHECK(a.observer.lastUpdateFor(id)->status == MessageStatus::Transmitting);
}


int main()
{
    testAddressedMessageIsAcknowledged();
    testRetriesThenFails();
    testBroadcastIsNotAcknowledged();
    testMessageNotForUsIsIgnored();
    testLongMessageIsFragmentedAndReassembled();
    testRetransmissionIsNotShownTwice();
    testRetransmittedFragmentsQueueOneAcknowledgement();
    testRestartedSenderIsNotMistakenForARetransmission();
    testMessageIdsStartAtRandom();
    testRetriesFillInAMessageOverTime();
    testMissingFragmentsAreAskedForAndResent();
    testLostLastFragmentStillAsksForTheRest();
    testQueuedReportsStayCurrent();
    testQueuedTransmissionsAreReported();
    testProgressDoesNotUseUpRetries();
    testPartialAckWithNoNewsCountsAsARetry();
    testDeliveryChip();
    testPingAndPong();
    testPingTimesOut();
    testOwnKeyingHoldsTheWaitForAnAnswer();
    testAirTimingWaitsForTheSlowestAnswer();
    testAirTimingCountsTheChords();
    testReplyWindowEndsAtTheAnswersChord();
    testQueuedWaitsCountDown();
    testWoahHoldsTheQueue();
    testQueuedMessageTakesATempoOfItsOwn();
    testPingWaitsForAnAnswerAtASlowerTempo();
    testAutoReplyCanBeDisabled();
    testStationWithAutoAckOffIsNotRetried();
    testPingSaysAutoAckIsOff();
    testWaitingMessageEndsWhenTheStationSaysItWillNotAnswer();
    testAcknowledgementTurnsRetriesBackOn();
    testAckWaitDoesNotBlockTheQueue();
    testTurnaroundKeepsStationsOffEachOther();
    testNextBurstWaitsForTheFarEndToAnswer();
    testAWaitedReplyOutlastsThePlainTurnaround();
    testTheWindowEndsWhenTheReplyArrives();
    testReplyIsNotHeldForOurOwnReplyWindow();
    testRetryBacksOffBeforeKeyingAgain();
    testAReplyGivesTheOtherStationTheChannel();
    testQueuedMessageRidesBehindAReply();
    testBusyStationsTakeTurns();
    testOnlyWaitingTrafficOfOurOwnRides();
    testOwnTrafficWaitsUntilListenersLetGo();
    testOwnTrafficGoesOnceTheAnsweredStationIsHeard();
    testRepliesDoNotWaitForTheAnsweredStationsTurn();
    testInhibitedStationTransmitsNothing();
    testAbortDropsEverythingOutstanding();
    testOneMessageCanBeRemovedOrAborted();
    testSentMessagesAreAbortedNotRemoved();
    testPingsCanBeRemovedOrAborted();
    testInhibitingLeavesSentMessagesToTheirAnswers();
    testFragmentsStillToComeReserveTheChannel();
    testReservationFollowsTheBurstsStillToCome();
    testClearingChannelReleasesStationsAtDifferentMoments();
    testAckWaitCoversTheWholeCycle();
    testBusyChannelFreezesTheQueue();
    testBusyChannelHoldsTheAcknowledgementTimer();
    testBusyChannelHoldsTheReplyWindow();
    testChannelThatNeverClearsIsEventuallyIgnored();
    testVoiceTransmissionDefersChat();
    testSendRequiresCallsign();
    testLocatorRidesToAStationThatTakesIt();
    testLocatorRidesUntilTheStationHasIt();
    testLocatorGoesOnceToAStationWithAutoAckOff();
    testReliableLinkDeliversAMessage();
    testReliableLinkTakesMessagesBackToBack();
    testReliableLinkFailureEndsTheMessage();
    testMessageTheLinkDidNotTakeGoesTheOrdinaryWay();
    testFramesThroughAReliableLinkAreNotAnswered();
    testReleasingAStationDropsWhatIsOutstanding();
    testQueuedWaitsLeaveTheLinkOut();
    testChangingTransportRequeuesWhatTheLinkHeld();
    testOlderStationGetsNoLocator();
    testLocatorEndsTheKeying();
    testDuetFillerCarriesTheLocator();
    testMapFollowsStationsWithKnownLocators();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    printf("all text messaging protocol checks passed\n");
    return 0;
}
