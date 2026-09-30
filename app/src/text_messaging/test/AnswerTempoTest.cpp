//=========================================================================
// Name:            AnswerTempoTest.cpp
// Purpose:         Tests for StationTempos.
//=========================================================================

#include <cstdio>
#include <vector>

#include "AnswerTempo.h"
#include "FrameCodec.h"

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

OutgoingBurst burst(FrameType type, const std::string& destination)
{
    Frame frame;
    frame.type = type;
    frame.destinationCrc = FrameCodec::callsignCrc24(destination);
    frame.originCallsign = "W1AW";
    frame.airId = 7;
    if (type == FrameType::PingAck) frame.payload = {12};
    if (type == FrameType::MessagePartialAck) frame.payload = {0x01};

    bool text = type == FrameType::Message || type == FrameType::Broadcast;
    if (text) frame.payload = {'h', 'i'};

    OutgoingBurst out;
    out.mode = text ? BurstMode::Text : BurstMode::Signalling;
    out.frame = FrameCodec::encode(frame, text ? TEXT_FRAME_BYTES : SIGNALLING_FRAME_BYTES);
    return out;
}

} // namespace

int main()
{
    const uint64_t LIFETIME = 15 * 60 * 1000;
    const int ADAGIO = 1;
    const int PRESTO = 4;

    StationTempos tempos(LIFETIME);
    tempos.heard("vk3abc", PRESTO, 1000);
    tempos.heard("K1ABC", ADAGIO, 2000);

    check(!burst(FrameType::PingAck, "VK3ABC").frame.empty(), "a pong encodes");

    // Every kind of reply goes back in the tempo its station was heard in,
    // whatever case the callsign was heard in.
    check(tempos.answerTempo({burst(FrameType::PingAck, "VK3ABC")}, 5000) == PRESTO, "pong in the ping's tempo");
    check(tempos.answerTempo({burst(FrameType::MessageAck, "K1ABC")}, 5000) == ADAGIO, "ack in the message's tempo");
    check(tempos.answerTempo({burst(FrameType::MessagePartialAck, "K1ABC")}, 5000) == ADAGIO,
          "partial ack in the message's tempo");

    // A message riding behind a reply goes in the reply's keying and tempo.
    check(tempos.answerTempo({burst(FrameType::MessageAck, "K1ABC"), burst(FrameType::Message, "VK3ABC")}, 5000) ==
              ADAGIO,
          "rider goes in the reply's tempo");

    // Traffic of our own goes in our own tempo, whoever it is for.
    check(tempos.answerTempo({burst(FrameType::Ping, "K1ABC")}, 5000) == 0, "ping in our own tempo");
    check(tempos.answerTempo({burst(FrameType::Message, "K1ABC")}, 5000) == 0, "message in our own tempo");
    check(tempos.answerTempo({burst(FrameType::Broadcast, "")}, 5000) == 0, "broadcast in our own tempo");
    check(tempos.answerTempo({}, 5000) == 0, "an empty keying has no tempo");

    // A station not heard lately is answered in our own tempo.
    check(tempos.answerTempo({burst(FrameType::PingAck, "G4XYZ")}, 5000) == 0, "unheard station");
    check(tempos.answerTempo({burst(FrameType::PingAck, "VK3ABC")}, 1000 + LIFETIME) == 0, "forgotten station");

    // Heard again, in another tempo: the latest wins.
    tempos.heard("VK3ABC", ADAGIO, 6000);
    check(tempos.answerTempo({burst(FrameType::PingAck, "VK3ABC")}, 7000) == ADAGIO, "latest tempo wins");

    // Tempos heard lately, each once, until they age out.
    tempos.heard("G4XYZ", PRESTO, 8000);
    std::vector<int> recent = tempos.recentTempos(9000);
    check(recent == std::vector<int>({ADAGIO, PRESTO}), "recent tempos");
    check(tempos.recentTempos(8000 + LIFETIME) == std::vector<int>(), "every tempo ages out");
    check(tempos.recentTempos(6000 + LIFETIME) == std::vector<int>({PRESTO}), "the oldest ages out first");

    // Nothing is remembered for a tempo of zero or an empty callsign.
    StationTempos empty(LIFETIME);
    empty.heard("", PRESTO, 0);
    empty.heard("W1AW", 0, 0);
    check(empty.recentTempos(1).empty(), "nothing usable heard");

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}
