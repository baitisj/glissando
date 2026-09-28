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
        return crc == FrameCodec::callsignCrc24("W1AW") ? std::string("W1AW") : std::string();
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
    frame.destinationCrc = FrameCodec::callsignCrc24("W1AW");
    frame.originCallsign = "K6ABC";
    frame.airId = 1234;
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
    check(has(tokens, Role::Field, "No.1234"), "message number");
    check(has(tokens, Role::Field, "PART 1/2"), "part");
    check(has(tokens, Role::Field, "1 MORE TO COME"), "bursts following");
    check(has(tokens, Role::Field, "33 CH"), "length");
    check(text == message, "text read out whole, padding left off");

    // The destination arrives in the first segment, the callsign across the
    // first two: nothing is told before its last byte is heard.
    std::vector<uint8_t> first(bytes.begin(), bytes.begin() + SEGMENT);
    std::vector<AnnotationToken> firstTokens = describeSegment(first, 0, 0, true, nullptr);
    check(firstTokens.size() == 2, "first segment: kind and destination only");
    check(firstTokens.size() == 2 && firstTokens[1].text.rfind("TO #", 0) == 0, "unknown destination as a CRC");

    // Only the third segment onwards heard: the header is lost, the text
    // still reads.
    tokens = readOut(bytes, true, text, 2);
    check(!tokens.empty() && tokens[0] == std::to_string((int)Role::Text) + ":" + message.substr(3, SEGMENT),
          "nothing but text after a lost header");
    check(text == message.substr(2 * SEGMENT - TEXT_HEADER_BYTES), "text after a lost header, from where it was heard");
}

void testSignalling()
{
    Frame frame;
    frame.type = FrameType::Ping;
    frame.destinationCrc = FrameCodec::callsignCrc24("N0CALL");
    frame.originCallsign = "VK3ABC/P";
    frame.airId = 7;
    frame.burstsFollowing = 1;
    frame.payload = {0x2A};
    std::vector<uint8_t> bytes = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
    check(!bytes.empty(), "ping encodes");

    std::string text;
    std::vector<std::string> tokens = readOut(bytes, false, text);
    using Role = AnnotationToken::Role;
    check(has(tokens, Role::Kind, "PING"), "ping kind");
    check(has(tokens, Role::Station, "DE VK3ABC/P"), "ping origin");
    check(has(tokens, Role::Field, "MORE TO COME"), "ping more follows");
    check(has(tokens, Role::Field, "1 B"), "ping length");
    check(has(tokens, Role::Field, "2A"), "ping payload in hex");
    check(text.empty(), "no text in a ping");
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

    std::vector<uint8_t> junk(SEGMENT, 0x7E);
    std::vector<AnnotationToken> t = describeSegment(junk, 0, 0, true, nullptr);
    check(t.size() == 1 && t[0].role == AnnotationToken::Role::Unknown, "not a chat frame");
}

} // namespace

int main()
{
    testMessage();
    testSignalling();
    testBroadcastAndJunk();
    if (failures == 0) std::printf("frame annotation tests passed\n");
    return failures == 0 ? 0 : 1;
}
