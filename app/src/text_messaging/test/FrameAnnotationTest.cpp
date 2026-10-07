//=========================================================================
// Name:            FrameAnnotationTest.cpp
// Purpose:         Tests for describeSegment(): a chat frame read out nine
//                  bytes at a time, as Glissando delivers it.
//=========================================================================

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "FrameAnnotation.h"
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

constexpr int SEGMENT = 9;  // Glissando::SEGMENT_DATA_BYTES

// Every segment's tokens, in order, as "role:text" strings, with the text
// tokens also joined into `text`.
std::vector<std::string> readOut(const std::vector<uint8_t>& frame, bool textBurst, std::string& text,
                                 int firstSegment = 0)
{
    CallsignForCrc names = [](uint32_t crc) {
        return crc == FrameCodec::callsignHash("W1AW") ? std::string("W1AW") : std::string();
    };
    std::vector<std::string> out;
    text.clear();
    for (int from = firstSegment * SEGMENT; from < (int)frame.size(); from += SEGMENT)
    {
        std::vector<uint8_t> soFar(frame.begin(), frame.begin() + std::min<size_t>(frame.size(), from + SEGMENT));
        for (const AnnotationToken& token :
             describeSegment(soFar, from, firstSegment * SEGMENT, textBurst, names))
        {
            out.push_back(std::to_string((int)token.role) + ":" + token.text);
            if (token.role == AnnotationToken::Role::Text) text += token.text;
        }
    }
    return out;
}

bool has(const std::vector<std::string>& tokens, AnnotationToken::Role role, const std::string& text)
{
    int count = 0;
    for (const std::string& t : tokens)
    {
        if (t == std::to_string((int)role) + ":" + text) count++;
    }
    return count == 1;
}

void testMessage()
{
    Frame frame;
    frame.type = FrameType::Message;
    frame.destinationCrc = FrameCodec::callsignHash("W1AW");
    frame.originCallsign = "K6ABC";
    frame.airId = 123;
    frame.fragmentIndex = 0;
    frame.fragmentCount = 2;
    frame.burstsFollowing = 1;
    std::string message = "HELLO FROM THE HOLOSUITE, CAPTAIN";
    frame.payload.assign(message.begin(), message.end());
    std::vector<uint8_t> bytes = FrameCodec::encode(frame, TEXT_FRAME_BYTES);
    check(!bytes.empty(), "message encodes");

    std::string text;
    std::vector<std::string> tokens = readOut(bytes, true, text);
    using Role = AnnotationToken::Role;
    check(has(tokens, Role::Kind, "MESSAGE"), "kind told once");
    check(has(tokens, Role::Station, "TO W1AW"), "destination named");
    check(has(tokens, Role::Station, "DE K6ABC"), "origin told once");
    check(has(tokens, Role::Field, "No.123"), "message number");
    check(has(tokens, Role::Field, "PART 1/2"), "part");
    check(has(tokens, Role::Field, "1 MORE TO COME"), "bursts following");
    check(text == message, "text read out whole, padding left off");

    // The type, destination, callsign and message number arrive in the first
    // segment; the part number ends in the second, so it waits for it.
    std::vector<uint8_t> first(bytes.begin(), bytes.begin() + SEGMENT);
    std::vector<AnnotationToken> firstTokens = describeSegment(first, 0, 0, true, nullptr);
    check(firstTokens.size() == 4, "first segment: kind, destination, origin and number");
    check(firstTokens.size() == 4 && firstTokens[1].text.rfind("TO #", 0) == 0, "unknown destination as a hash");

    // Only the third segment onwards heard: the header is lost, the text
    // still reads, since no character's code crosses a segment.
    tokens = readOut(bytes, true, text, 2);
    check(!tokens.empty() && tokens[0].rfind(std::to_string((int)Role::Text) + ":", 0) == 0,
          "nothing but text after a lost header");
    check(!text.empty() && text.size() < message.size() &&
              message.compare(message.size() - text.size(), text.size(), text) == 0,
          "text after a lost header, from where it was heard");

    // The second segment alone cannot tell where the header ends.
    tokens = readOut(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 2 * SEGMENT), true, text, 1);
    check(tokens.size() == 1 && tokens[0] == std::to_string((int)Role::Unknown) + ":...", "lost header told");
}

// A station with Auto acknowledge off says so in a message's type and in a
// ping's flags.
void testNoAutoAck()
{
    using Role = AnnotationToken::Role;
    Frame frame;
    frame.type = FrameType::Broadcast;
    frame.originCallsign = "K6ABC";
    frame.fragmentCount = 1;
    frame.senderAutoAck = false;
    frame.payload.assign(3, 'X');
    std::string text;
    std::vector<std::string> tokens = readOut(FrameCodec::encode(frame, TEXT_FRAME_BYTES), true, text);
    check(has(tokens, Role::Kind, "BROADCAST"), "flagged broadcast kind");
    check(has(tokens, Role::Field, "NO AUTO ACK"), "flagged broadcast says so");
    check(text == "XXX", "flagged broadcast text");

    frame.type = FrameType::Ping;
    frame.destinationCrc = FrameCodec::callsignHash("W1AW");
    frame.payload.clear();
    tokens = readOut(FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES), false, text);
    check(has(tokens, Role::Kind, "PING"), "flagged ping kind");
    check(has(tokens, Role::Field, "NO AUTO ACK"), "flagged ping says so");

    frame.senderAutoAck = true;
    tokens = readOut(FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES), false, text);
    for (const std::string& t : tokens) check(t.find("NO AUTO ACK") == std::string::npos, "plain ping says nothing");
}

void testSignalling()
{
    Frame frame;
    frame.type = FrameType::PingAck;
    frame.destinationCrc = FrameCodec::callsignHash("N0CALL");
    frame.originCallsign = "VK3ABC/P";
    frame.airId = 7;
    frame.burstsFollowing = 1;
    frame.payload = {0x2A};
    std::vector<uint8_t> bytes = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
    check(!bytes.empty(), "pong encodes");

    std::string text;
    std::vector<std::string> tokens = readOut(bytes, false, text);
    using Role = AnnotationToken::Role;
    check(has(tokens, Role::Kind, "PONG"), "pong kind");
    check(has(tokens, Role::Station, "DE VK3ABC/P"), "pong origin");
    check(has(tokens, Role::Field, "MORE TO COME"), "pong more follows");
    check(has(tokens, Role::Field, "No.7"), "pong number");
    check(has(tokens, Role::Field, "2A"), "pong payload in hex");
    check(text.empty(), "no text in a pong");
}

void testBroadcastAndJunk()
{
    Frame frame;
    frame.type = FrameType::Broadcast;
    frame.originCallsign = "K6ABC";
    frame.payload = {'C', 'Q'};
    std::vector<uint8_t> bytes = FrameCodec::encode(frame, TEXT_FRAME_BYTES);
    std::string text;
    std::vector<std::string> tokens = readOut(bytes, true, text);
    check(has(tokens, AnnotationToken::Role::Station, "TO ALL"), "broadcast to all");
    check(text == "CQ", "broadcast text");

    // A type code no frame uses: 9 began a Glissando 0.3 frame.
    std::vector<uint8_t> junk(SEGMENT, 0x99);
    std::vector<AnnotationToken> t = describeSegment(junk, 0, 0, true, nullptr);
    check(t.size() == 1 && t[0].role == AnnotationToken::Role::Unknown, "not a chat frame");

    // The short locator a duet's spare voice carries.
    Frame locator;
    locator.type = FrameType::Locator;
    locator.originCallsign = "TEST1/P";
    locator.locator = "CN87";
    std::vector<uint8_t> shortForm = FrameCodec::encodeShortLocator(locator, SIGNALLING_FRAME_BYTES);
    shortForm.resize(SEGMENT);
    std::vector<std::string> read = readOut(shortForm, false, text);
    check(has(read, AnnotationToken::Role::Kind, "LOCATOR"), "short locator kind");
    check(has(read, AnnotationToken::Role::Station, "DE TEST1/P"), "short locator callsign");
    check(has(read, AnnotationToken::Role::Field, "LOC CN87"), "short locator square");
    check(!has(read, AnnotationToken::Role::Station, "TO ALL"), "short locator addressed to nobody");
}

} // namespace

int main()
{
    testMessage();
    testNoAutoAck();
    testSignalling();
    testBroadcastAndJunk();
    if (failures == 0) std::printf("frame annotation tests passed\n");
    return failures == 0 ? 0 : 1;
}
