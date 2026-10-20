//=========================================================================
// Name:            PartialCopiesTest.cpp
// Purpose:         Copies of a text frame that each lost segments, put
//                  together (PartialCopies.h).
//=========================================================================

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../FrameCodec.h"
#include "../PartialCopies.h"

using namespace TextMessaging;

namespace
{

int failures = 0;

void check(bool condition, const char* what, int line)
{
    if (!condition)
    {
        failures++;
        fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

const std::string SENTENCE = "The quick brown fox jumps over the lazy dog, 73 de W1AW";

std::vector<uint8_t> encode(const std::string& sentence, int burstsFollowing, uint16_t airId = 0x123)
{
    Frame frame;
    frame.type = FrameType::Broadcast;
    frame.originCallsign = "W1AW";
    frame.airId = airId;
    frame.fragmentIndex = 0;
    frame.fragmentCount = 2;
    frame.burstsFollowing = (uint8_t)burstsFollowing;
    frame.payload.assign(sentence.begin(), sentence.end());
    return FrameCodec::encode(frame, TEXT_FRAME_BYTES);
}

std::vector<uint8_t> lose(std::vector<uint8_t> bytes, uint8_t blocks)
{
    for (int b = 0; b < 6; b++)
    {
        if (blocks & (1u << b)) std::fill(bytes.begin() + 9 * b, bytes.begin() + 9 * (b + 1), 0);
    }
    return bytes;
}

std::string text(const Frame& frame)
{
    return std::string(frame.payload.begin(), frame.payload.end());
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// What one copy lost, a retry has: together they are whole. The retry's
// first block says less is to come in its keying, and that is what the
// whole frame says.
void testRetryFillsTheGap()
{
    std::vector<uint8_t> first = encode(SENTENCE, 1);
    std::vector<uint8_t> retry = encode(SENTENCE, 0);
    CHECK(first != retry);

    PartialCopies copies;
    Frame frame;
    CHECK(copies.add(lose(first, 1u << 2), 1u << 2, 1000, frame));
    CHECK(frame.partial && contains(text(frame), "...") && frame.burstsFollowing == 1);
    CHECK(copies.add(lose(retry, 1u << 4), 1u << 4, 60000, frame));
    CHECK(!frame.partial && text(frame) == SENTENCE && frame.burstsFollowing == 0);

    // Used up: the next copy with a gap stands alone again.
    CHECK(copies.add(lose(retry, 1u << 4), 1u << 4, 70000, frame));
    CHECK(frame.partial);

    // Two copies that lost the same block are still missing it.
    copies.clear();
    CHECK(copies.add(lose(first, 0x0C), 0x0C, 1000, frame));
    CHECK(copies.add(lose(retry, 0x04), 0x04, 2000, frame));
    CHECK(frame.partial && contains(text(frame), "..."));
    CHECK(copies.add(lose(first, 0x20), 0x20, 3000, frame));
    CHECK(!frame.partial && text(frame) == SENTENCE);
}

// A whole copy goes straight through and makes the partial ones redundant.
void testWholeCopyForgetsPartials()
{
    std::vector<uint8_t> bytes = encode(SENTENCE, 0);
    PartialCopies copies;
    Frame frame;
    CHECK(copies.add(lose(bytes, 1u << 2), 1u << 2, 1000, frame));
    CHECK(copies.add(bytes, 0, 2000, frame));
    CHECK(!frame.partial && text(frame) == SENTENCE);
    CHECK(copies.add(lose(bytes, 1u << 4), 1u << 4, 3000, frame));
    CHECK(frame.partial);
}

// Under the same ID a sender that restarted sends something else: where
// both copies have a block and it differs, the old copy goes.
void testDifferentMessageReplaces()
{
    PartialCopies copies;
    Frame frame;
    std::vector<uint8_t> other = encode("Something else entirely, sent under the same ID", 0);
    CHECK(copies.add(lose(encode(SENTENCE, 0), 1u << 4), 1u << 4, 1000, frame));
    CHECK(copies.add(lose(other, 1u << 2), 1u << 2, 2000, frame));
    CHECK(frame.partial && !contains(text(frame), "fox"));

    // Another message ID is another frame altogether.
    copies.clear();
    CHECK(copies.add(lose(encode(SENTENCE, 0, 0x123), 1u << 2), 1u << 2, 1000, frame));
    CHECK(copies.add(lose(encode(SENTENCE, 0, 0x124), 1u << 4), 1u << 4, 2000, frame));
    CHECK(frame.partial && frame.airId == 0x124);
}

// Copies are kept for a while, not for ever.
void testCopiesExpire()
{
    std::vector<uint8_t> bytes = encode(SENTENCE, 0);
    PartialCopies copies;
    Frame frame;
    CHECK(copies.add(lose(bytes, 1u << 2), 1u << 2, 1000, frame));
    CHECK(copies.add(lose(bytes, 1u << 4), 1u << 4, 1000 + PartialCopies::KEEP_MILLISECONDS + 1, frame));
    CHECK(frame.partial);
}

// What cannot be read is turned away: a frame that lost its header.
void testUnreadable()
{
    PartialCopies copies;
    Frame frame;
    CHECK(!copies.add(lose(encode(SENTENCE, 0), 1u << 0), 1u << 0, 1000, frame));
}

} // namespace

int main()
{
    testRetryFillsTheGap();
    testWholeCopyForgetsPartials();
    testDifferentMessageReplaces();
    testCopiesExpire();
    testUnreadable();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all partial copy checks passed\n");
    return 0;
}
