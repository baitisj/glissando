//=========================================================================
// Name:            GlissandoFecTest.cpp
// Purpose:         Pins Glissando's CRC, convolutional code and interleaver
//                  to the prototype (prototype/fec.py).
//=========================================================================

#include <cmath>
#include <cstring>
#include <string>

#include "../GlissandoFec.h"
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

} // namespace

int main()
{
    testAgainstPrototype();
    testDecode();
    testKnownZeroTail();
    if (failures == 0) printf("glissando FEC tests passed\n");
    return failures == 0 ? 0 : 1;
}
