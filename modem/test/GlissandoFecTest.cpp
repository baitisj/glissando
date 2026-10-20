//=========================================================================
// Name:            GlissandoFecTest.cpp
// Purpose:         Pins Glissando's CRC, convolutional code and interleaver
//                  to the prototype (prototype/fec.py).
//=========================================================================

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../GlissandoFec.h"
#include "../GlissandoInternal.h"
#include "GlissandoTestUtil.h"
#include "GlissandoVectors.h"

using namespace Glissando;
using namespace GlissandoTest;

namespace
{

Payload payloadFromString(const char* bits)
{
    Payload p{};
    for (int i = 0; i < PAYLOAD_BITS; i++) p[i] = (uint8_t)(bits[i] == '1');
    return p;
}

std::string toString(const uint8_t* bits, int n)
{
    std::string s;
    for (int i = 0; i < n; i++) s += bits[i] ? '1' : '0';
    return s;
}

void testAgainstPrototype()
{
    for (const auto& v : GlissandoVectors::FEC)
    {
        CHECK(strlen(v.payload) == (size_t)PAYLOAD_BITS);
        Payload payload = payloadFromString(v.payload);

        uint16_t crc = crc14(payload.data(), PAYLOAD_BITS);
        uint8_t crcBits[CRC_BITS];
        for (int i = 0; i < CRC_BITS; i++) crcBits[i] = (uint8_t)((crc >> (CRC_BITS - 1 - i)) & 1);
        bool crcMatches = toString(crcBits, CRC_BITS) == v.crc;
        CHECK(crcMatches);

        CodedFrame coded = encodeFrame(payload);
        bool codedMatches = toString(coded.data(), FRAME_BITS) == v.coded;
        CHECK(codedMatches);
        if (!crcMatches || !codedMatches) fprintf(stderr, "  vector %s\n", v.name);
    }
}

FrameLlrs cleanLlrs(const CodedFrame& coded, float magnitude)
{
    FrameLlrs llrs;
    for (int i = 0; i < FRAME_BITS; i++) llrs[i] = coded[i] ? -magnitude : magnitude;
    return llrs;
}

void testDecode()
{
    Random rng(7);
    for (int trial = 0; trial < 20; trial++)
    {
        Payload payload = rng.payload();
        CodedFrame coded = encodeFrame(payload);

        Payload decoded{};
        CHECK(decodeFrame(cleanLlrs(coded, 4.0f), decoded));
        CHECK(decoded == payload);

        // Soft decisions with a handful of confident errors spread across
        // the frame (the interleaver scatters them over the trellis) still
        // decode.
        FrameLlrs noisy = cleanLlrs(coded, 2.0f);
        for (int i = 0; i < 8; i++)
        {
            int bit = (int)(rng.next() % FRAME_BITS);
            noisy[bit] = -noisy[bit] * 0.5f;
        }
        CHECK(decodeFrame(noisy, decoded));
        CHECK(decoded == payload);
    }

    Payload decoded{};
    // Pure noise (random LLRs) decodes to something, but almost never with
    // a good CRC: allow at most one pass in 200 (expected 200 / 16384).
    int passes = 0;
    for (int trial = 0; trial < 200; trial++)
    {
        FrameLlrs random;
        for (auto& l : random) l = (float)(rng.gaussian() * 3.0);
        if (decodeFrame(random, decoded)) passes++;
    }
    CHECK(passes <= 1);
}

// A frame ending in zero padding, as a burst's last segment does: told
// which bits are zero, the decoder copes with noise that defeats it
// otherwise; told wrongly, it fails the CRC rather than inventing a payload.
void testKnownZeroTail()
{
    Random rng(11);
    int plain = 0;
    int partly = 0;
    int known = 0;
    for (int trial = 0; trial < 200; trial++)
    {
        Payload payload = rng.payload();
        for (int i = PAYLOAD_BITS - 64; i < PAYLOAD_BITS; i++) payload[i] = 0;
        CodedFrame coded = encodeFrame(payload);

        // About 0 dB Es/N0 per coded bit in BPSK terms, where the code alone
        // decodes well under half the frames (prototype/fec.py measures 50 %
        // at -2.7 dB plain and -4.8 dB with 64 known bits).
        const double sigma = std::sqrt(1.0 / (2.0 * std::pow(10.0, -4.0 / 10.0)));
        FrameLlrs noisy;
        for (int i = 0; i < FRAME_BITS; i++)
        {
            double y = (coded[i] ? -1.0 : 1.0) + sigma * rng.gaussian();
            noisy[i] = (float)(2.0 * y / (sigma * sigma));
        }

        Payload decoded{};
        if (decodeFrame(noisy, decoded) && decoded == payload) plain++;
        if (decodeFrame(noisy, decoded, 64) && decoded == payload) known++;
        // Fewer known bits than there are is still a right guess.
        if (decodeFrame(noisy, decoded, 32) && decoded == payload) partly++;
    }
    printf("64 zero bits, noisy: %d/200 decoded plain, %d/200 knowing 32 of them, %d/200 knowing all\n", plain,
           partly, known);
    CHECK(partly >= plain);
    CHECK(known >= plain + 40);

    // Clean frames whose tail is not zero: a wrong guess must not decode.
    int wrongPasses = 0;
    for (int trial = 0; trial < 200; trial++)
    {
        Payload payload = rng.payload();
        payload[PAYLOAD_BITS - 1] = 1;
        Payload decoded{};
        if (decodeFrame(cleanLlrs(encodeFrame(payload), 4.0f), decoded, 64)) wrongPasses++;
        // The guess changes nothing for a frame it does not cover.
        CHECK(decodeFrame(cleanLlrs(encodeFrame(payload), 4.0f), decoded, 0) && decoded == payload);
    }
    CHECK(wrongPasses <= 1);
}

// Known bits of either value: the start of a reply whose sender and
// addressee the receiver knows. Right, they rescue noisy frames; wrong, they
// fail the CRC.
void testKnownBits()
{
    Random rng(13);
    Payload expected = rng.payload();
    KnownBits known = noKnownBits();
    for (int i = 1; i < 72; i++) known[(size_t)i] = (int8_t)expected[(size_t)i];

    int plain = 0;
    int helped = 0;
    const double sigma = std::sqrt(1.0 / (2.0 * std::pow(10.0, -4.0 / 10.0)));
    for (int trial = 0; trial < 200; trial++)
    {
        Payload payload = rng.payload();
        for (int i = 1; i < 72; i++) payload[(size_t)i] = expected[(size_t)i];
        CodedFrame coded = encodeFrame(payload);
        FrameLlrs noisy;
        for (int i = 0; i < FRAME_BITS; i++)
        {
            double y = (coded[i] ? -1.0 : 1.0) + sigma * rng.gaussian();
            noisy[i] = (float)(2.0 * y / (sigma * sigma));
        }
        Payload decoded{};
        if (decodeFrame(noisy, decoded) && decoded == payload) plain++;
        if (decodeFrame(noisy, decoded, known) && decoded == payload) helped++;
    }
    printf("71 bits known, noisy: %d/200 decoded plain, %d/200 with them known\n", plain, helped);
    CHECK(helped >= plain + 100);

    int wrongPasses = 0;
    for (int trial = 0; trial < 200; trial++)
    {
        Payload payload = rng.payload();
        Payload decoded{};
        if (decodeFrame(cleanLlrs(encodeFrame(payload), 4.0f), decoded, known)) wrongPasses++;
    }
    CHECK(wrongPasses <= 1);
}

// List decoding: rank 0 is the path decodeFrame() picks, every payload
// listed passes the CRC, and the runners-up rescue some frames the best
// path gets wrong. Through noise each runner-up passes the CRC by luck one
// time in 16384, as the best path does.
void testListDecode()
{
    Random rng(17);
    const double sigma = std::sqrt(1.0 / (2.0 * std::pow(10.0, -3.0 / 10.0)));
    int plain = 0;
    int listed = 0;
    for (int trial = 0; trial < 200; trial++)
    {
        Payload payload = rng.payload();
        CodedFrame coded = encodeFrame(payload);
        FrameLlrs noisy;
        for (int i = 0; i < FRAME_BITS; i++)
        {
            double y = (coded[i] ? -1.0 : 1.0) + sigma * rng.gaussian();
            noisy[i] = (float)(2.0 * y / (sigma * sigma));
        }

        Payload decoded{};
        bool best = decodeFrame(noisy, decoded);
        std::vector<ListDecode> list = decodeFrameList(noisy, noKnownBits(), MAX_LIST_SIZE);
        CHECK(best == (!list.empty() && list[0].rank == 0));
        if (best) CHECK(list[0].payload == decoded);
        int lastRank = -1;
        for (const ListDecode& d : list)
        {
            CHECK(d.rank > lastRank && d.rank < MAX_LIST_SIZE);
            lastRank = d.rank;
            Payload again{};
            CHECK(decodeFrame(cleanLlrs(encodeFrame(d.payload), 4.0f), again) && again == d.payload);
        }
        if (best && decoded == payload) plain++;
        for (const ListDecode& d : list)
        {
            if (d.payload == payload)
            {
                listed++;
                break;
            }
        }

        // Known bits hold on every path.
        KnownBits known = noKnownBits();
        for (int i = 0; i < 8; i++) known[(size_t)i] = (int8_t)payload[(size_t)i];
        for (const ListDecode& d : decodeFrameList(noisy, known, MAX_LIST_SIZE))
        {
            for (int i = 0; i < 8; i++) CHECK(d.payload[(size_t)i] == payload[(size_t)i]);
        }
    }
    printf("list of %d, noisy: %d/200 decoded by the best path, %d/200 somewhere on the list\n", MAX_LIST_SIZE,
           plain, listed);
    CHECK(listed >= plain + 30);

    int luckyPaths = 0;
    const int noiseFrames = 2000;
    for (int trial = 0; trial < noiseFrames; trial++)
    {
        FrameLlrs random;
        for (auto& l : random) l = (float)(rng.gaussian() * 3.0);
        for (const ListDecode& d : decodeFrameList(random, noKnownBits(), MAX_LIST_SIZE))
        {
            if (d.rank > 0) luckyPaths++;
        }
    }
    // Expected 2000 x 15 / 16384, about 1.8.
    CHECK(luckyPaths <= 8);
}

// How much of what was heard argues against a payload: nothing for the
// payload sent, everything without any evidence, and a confident wrong bit
// more than a doubtful one.
void testDisagreement()
{
    Random rng(19);
    Payload payload = rng.payload();
    CodedFrame coded = encodeFrame(payload);
    FrameLlrs llrs = cleanLlrs(coded, 2.0f);
    CHECK(detail::disagreement(llrs, payload) == 0.0);
    CHECK(detail::hardErrors(llrs, payload) == 0);

    FrameLlrs none{};
    CHECK(detail::disagreement(none, payload) == 1.0);

    FrameLlrs doubtful = llrs;
    FrameLlrs confident = llrs;
    doubtful[0] = -0.2f * llrs[0];
    confident[0] = -4.0f * llrs[0];
    CHECK(detail::hardErrors(doubtful, payload) == 1 && detail::hardErrors(confident, payload) == 1);
    double small = detail::disagreement(doubtful, payload);
    double large = detail::disagreement(confident, payload);
    CHECK(small > 0.0 && small < large && large < 1.0);
    // 0.4 of the 388.4 total weight.
    CHECK(std::fabs(small - 0.4 / (194 * 2.0 + 0.4)) < 1e-6);

    // Another payload, re-encoded, disagrees with about half the weight.
    double other = detail::disagreement(llrs, rng.payload());
    CHECK(other > 0.3 && other < 0.7);
}

} // namespace

int main()
{
    testAgainstPrototype();
    testDecode();
    testKnownZeroTail();
    testKnownBits();
    testListDecode();
    testDisagreement();
    if (failures == 0) printf("glissando FEC tests passed\n");
    return failures == 0 ? 0 : 1;
}
