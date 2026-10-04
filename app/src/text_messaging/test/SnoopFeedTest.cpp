//=========================================================================
// Name:            SnoopFeedTest.cpp
// Purpose:         Tests for SnoopFeed.
//=========================================================================

#include <cstdio>
#include <string>
#include <vector>

#include "SnoopFeed.h"

using namespace TextMessaging;

namespace
{

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

Frame textFrame(const std::string& from, const std::string& to, uint16_t airId, int index, int count,
                const std::string& text)
{
    Frame frame;
    frame.type = to.empty() ? FrameType::Broadcast : FrameType::Message;
    frame.destinationCrc = to.empty() ? 0 : FrameCodec::callsignHash(to);
    frame.originCallsign = from;
    frame.airId = airId;
    frame.fragmentIndex = (uint8_t)index;
    frame.fragmentCount = (uint8_t)count;
    frame.payload.assign(text.begin(), text.end());
    return frame;
}

Frame signalling(FrameType type, const std::string& from, const std::string& to, uint16_t airId,
                 std::vector<uint8_t> payload = {})
{
    Frame frame;
    frame.type = type;
    frame.destinationCrc = FrameCodec::callsignHash(to);
    frame.originCallsign = from;
    frame.airId = airId;
    frame.payload = payload;
    return frame;
}

std::vector<SnoopEvent> messages(const std::vector<SnoopEvent>& events)
{
    std::vector<SnoopEvent> out;
    for (const SnoopEvent& event : events)
    {
        if (event.kind == SnoopEvent::Kind::Message) out.push_back(event);
    }
    return out;
}

} // namespace

int main()
{
    // A conversation between two other stations is reassembled and names
    // both ends once each has been heard.
    {
        SnoopFeed feed;
        feed.setMyCallsign("N0ME");
        std::vector<SnoopEvent> heard;
        feed.addListener([&](const SnoopEvent& event) { heard.push_back(event); });

        feed.onFrame(signalling(FrameType::Ping, "K2XYZ", "W1ABC", 7), -12.0f, SnoopSource::Glissando, 100);
        check(heard.size() == 1, "a ping is one event");
        check(heard[0].destination.size() == 7 && heard[0].destination[0] == '#',
              "an unheard addressee shows as its CRC");
        check(!heard[0].destinationKnown, "and is marked unknown");
        check(describeSnoopFrame(heard[0]) == "PING", "ping described");

        feed.onFrame(signalling(FrameType::PingAck, "W1ABC", "K2XYZ", 8, {(uint8_t)(int8_t)-19}),
                     -10.0f, SnoopSource::Glissando, 101);
        check(heard.back().destination == "K2XYZ", "a station heard earlier is named");
        check(heard.back().hasReportedSnr && heard.back().reportedSnr == -9.5f, "pong carries its SNR");
        check(describeSnoopFrame(heard.back()) == "PONG, heard at -9.5 dB", "pong described");

        feed.onFrame(textFrame("W1ABC", "K2XYZ", 42, 1, 2, "world"), -8.0f, SnoopSource::Glissando, 110);
        check(messages(heard).empty(), "half a message is not a message");
        check(heard.back().fragmentsHeld == 2u, "second fragment held");
        check(describeSnoopFrame(heard.back()) == "#42 fragment 2/2", "fragment described");

        feed.onFrame(textFrame("W1ABC", "K2XYZ", 42, 0, 2, "hello "), -6.0f, SnoopSource::Glissando, 111);
        std::vector<SnoopEvent> done = messages(heard);
        check(done.size() == 1, "both fragments make a message");
        check(done[0].text == "hello world", "fragments in order, not arrival order");
        check(done[0].origin == "W1ABC" && done[0].destination == "K2XYZ", "ends named");
        check(!done[0].toMe && !done[0].broadcast, "not for us, not a broadcast");
        check(done[0].snr == -7.0f, "SNR averaged over the fragments");

        // The sender did not get its acknowledgement and tries again.
        feed.onFrame(textFrame("W1ABC", "K2XYZ", 42, 0, 2, "hello "), -6.0f, SnoopSource::Glissando, 150);
        check(heard.back().repeat, "a retry is marked");
        feed.onFrame(textFrame("W1ABC", "K2XYZ", 42, 1, 2, "world"), -6.0f, SnoopSource::Glissando, 151);
        check(messages(heard).size() == 1, "a retry is not a second message");

        feed.onFrame(signalling(FrameType::MessageAck, "K2XYZ", "W1ABC", 42), -12.0f, SnoopSource::Data2G, 160);
        check(describeSnoopFrame(heard.back()) == "ACK #42", "ack described");
        check(heard.back().source == SnoopSource::Data2G, "source kept");

        // The ID comes round again with new words: a new message.
        feed.onFrame(textFrame("W1ABC", "K2XYZ", 42, 0, 1, "73"), -6.0f, SnoopSource::Glissando, 400);
        check(messages(heard).size() == 2 && messages(heard).back().text == "73", "reused ID is a new message");

        check(feed.recent().size() == heard.size(), "history holds every event");
    }

    // Our own echo is dropped; traffic to us and broadcasts are marked.
    {
        SnoopFeed feed;
        feed.setMyCallsign("n0me");
        std::vector<SnoopEvent> heard;
        int id = feed.addListener([&](const SnoopEvent& event) { heard.push_back(event); });

        feed.onFrame(textFrame("N0ME", "K2XYZ", 1, 0, 1, "echo"), 0.0f, SnoopSource::Glissando, 1);
        check(heard.empty(), "own transmission ignored");

        feed.onFrame(textFrame("K2XYZ", "N0ME", 2, 0, 1, "hi"), 0.0f, SnoopSource::Glissando, 2);
        check(messages(heard).size() == 1 && messages(heard)[0].toMe, "message to us marked");
        check(messages(heard)[0].destination == "N0ME", "our own callsign is known");

        feed.onFrame(textFrame("K2XYZ", "", 3, 0, 1, "CQ"), 0.0f, SnoopSource::Glissando, 3);
        check(messages(heard).back().broadcast && messages(heard).back().destination.empty(),
              "broadcast has no addressee");

        feed.removeListener(id);
        size_t before = heard.size();
        feed.onFrame(textFrame("K2XYZ", "", 4, 0, 1, "CQ"), 0.0f, SnoopSource::Glissando, 4);
        check(heard.size() == before, "removed listener not called");
    }

    // A station added by hand is named before it has transmitted.
    {
        SnoopFeed feed;
        feed.addKnownCallsign("vk3abc");
        std::vector<SnoopEvent> heard;
        feed.addListener([&](const SnoopEvent& event) { heard.push_back(event); });
        feed.onFrame(signalling(FrameType::Ping, "K2XYZ", "VK3ABC", 1), 0.0f, SnoopSource::Glissando, 1);
        check(heard[0].destination == "VK3ABC" && heard[0].destinationKnown, "known callsign named");

        feed.onFrame(signalling(FrameType::MessagePartialAck, "K2XYZ", "VK3ABC", 9, {0x05}), 0.0f,
                     SnoopSource::Glissando, 2);
        check(describeSnoopFrame(heard.back()) == "PARTIAL ACK #9, holds 1,3", "partial ack described");
    }

    // A fragment left hanging is given up on, and history is bounded.
    {
        SnoopFeed feed;
        std::vector<SnoopEvent> heard;
        feed.addListener([&](const SnoopEvent& event) { heard.push_back(event); });
        feed.onFrame(textFrame("K2XYZ", "", 5, 0, 2, "a"), 0.0f, SnoopSource::Glissando, 0);
        feed.onFrame(textFrame("K2XYZ", "", 5, 1, 2, "b"), 0.0f, SnoopSource::Glissando,
                     SnoopFeed::REASSEMBLY_TIMEOUT_SECONDS + 10);
        check(messages(heard).empty(), "stale fragment not joined to a late one");

        for (int i = 0; i < (int)SnoopFeed::HISTORY + 50; i++)
        {
            feed.onFrame(signalling(FrameType::Ping, "K2XYZ", "W1ABC", (uint16_t)i), 0.0f,
                         SnoopSource::Glissando, 5000);
        }
        std::vector<SnoopEvent> recent = feed.recent();
        check(recent.size() == SnoopFeed::HISTORY, "history bounded");
        check(recent.back().sequence == heard.back().sequence, "newest kept");
    }

    if (failures == 0) std::printf("All SnoopFeed tests passed.\n");
    return failures == 0 ? 0 : 1;
}
