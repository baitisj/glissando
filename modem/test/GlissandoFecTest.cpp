//=========================================================================
// Name:            GlissandoFecTest.cpp
// Purpose:         Pins Glissando's CRC, convolutional code and interleaver
//                  to the prototype (prototype/fec.py).
//=========================================================================

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

} // namespace

int main()
{
    testAgainstPrototype();
    testDecode();
    if (failures == 0) printf("glissando FEC tests passed\n");
    return failures == 0 ? 0 : 1;
}
