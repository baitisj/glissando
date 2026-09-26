//=========================================================================
// Name:            GlissandoLinkTest.cpp
// Purpose:         Chat frames cut into Glissando payloads and put back
//                  together (GlissandoLink.h).
//=========================================================================

#include "../GlissandoLink.h"
#include "GlissandoTestUtil.h"

using namespace Glissando;
using namespace GlissandoTest;

namespace
{

constexpr int SIGNALLING_BYTES = 14;    // text_messaging's SIGNALLING_FRAME_BYTES
constexpr int TEXT_BYTES = 54;          // text_messaging's TEXT_FRAME_BYTES
constexpr long long DUPLICATE_SAMPLES = 640;

LinkBurst makeBurst(bool text, int used, Random& rng)
{
    LinkBurst burst;
    burst.text = text;
    burst.bytes.assign(text ? TEXT_BYTES : SIGNALLING_BYTES, 0);
    for (int i = 0; i < used; i++) burst.bytes[(size_t)i] = (uint8_t)(1 + rng.next() % 255);
    return burst;
}

// Feeds payloads as if each arrived in its own frame, far apart in time.
std::vector<LinkBurst> reassemble(Reassembler& r, const std::vector<Payload>& payloads, long long firstStart = 0)
{
    std::vector<LinkBurst> out;
    long long start = firstStart;
    for (const Payload& p : payloads)
    {
        LinkBurst burst;
        if (r.add(p, start, DUPLICATE_SAMPLES, burst)) out.push_back(burst);
        start += 100000;
    }
    return out;
}

bool sameBurst(const LinkBurst& a, const LinkBurst& b)
{
    return a.text == b.text && a.bytes == b.bytes;
}

void testRoundTrip()
{
    Random rng(1);
    for (bool text : {false, true})
    {
        LinkBurst burst = makeBurst(text, text ? TEXT_BYTES : SIGNALLING_BYTES, rng);
        std::vector<Payload> payloads = segmentBursts({burst}, 1);
        int expected = text ? 6 : 2; // ceil(54 / 9), ceil(14 / 9)
        CHECK((int)payloads.size() == expected);
        CHECK(framesForBursts({burst}, 1) == expected);

        Reassembler r(SIGNALLING_BYTES, TEXT_BYTES);
        std::vector<LinkBurst> out = reassemble(r, payloads);
        CHECK(out.size() == 1 && sameBurst(out[0], burst));
    }
}

void testTrailingZeros()
{
    Random rng(2);
    // Ten bytes of text and 44 bytes of padding go in two segments.
    LinkBurst shortText = makeBurst(true, 10, rng);
    std::vector<Payload> payloads = segmentBursts({shortText}, 1);
    CHECK(payloads.size() == 2);
    Reassembler r(SIGNALLING_BYTES, TEXT_BYTES);
    std::vector<LinkBurst> out = reassemble(r, payloads);
    CHECK(out.size() == 1 && sameBurst(out[0], shortText));
    if (!out.empty()) CHECK(out[0].bytes.size() == (size_t)TEXT_BYTES);

    // An all-zero frame still sends one segment, and comes back whole.
    LinkBurst zeros = makeBurst(false, 0, rng);
    payloads = segmentBursts({zeros}, 1);
    CHECK(payloads.size() == 1);
    out = reassemble(r, payloads);
    CHECK(out.size() == 1 && sameBurst(out[0], zeros));

    // A zero byte inside the frame is not padding.
    LinkBurst gap = makeBurst(false, SIGNALLING_BYTES, rng);
    gap.bytes[9] = 0;
    gap.bytes[13] = 0;
    payloads = segmentBursts({gap}, 1);
    CHECK(payloads.size() == 2);
    out = reassemble(r, payloads);
    CHECK(out.size() == 1 && sameBurst(out[0], gap));
}

void testDuetFiller()
{
    Random rng(3);
    // 2 + 3 segments: the duet needs a filler to make three whole frames.
    std::vector<LinkBurst> bursts = {makeBurst(false, SIGNALLING_BYTES, rng), makeBurst(true, 20, rng)};
    std::vector<Payload> payloads = segmentBursts(bursts, 2);
    CHECK(payloads.size() == 6);
    CHECK(framesForBursts(bursts, 2) == 3);
    CHECK(framesForBursts(bursts, 1) == 5);
    CHECK(segmentBursts(bursts, 1).size() == 5);

    Reassembler r(SIGNALLING_BYTES, TEXT_BYTES);
    std::vector<LinkBurst> out = reassemble(r, payloads);
    CHECK(out.size() == 2);
    if (out.size() == 2)
    {
        CHECK(sameBurst(out[0], bursts[0]));
        CHECK(sameBurst(out[1], bursts[1]));
    }

    // A filler alone completes nothing and disturbs nothing.
    LinkBurst burst;
    CHECK(!r.add(payloads.back(), 999999999, DUPLICATE_SAMPLES, burst));
}

void testOutOfOrder()
{
    Random rng(4);
    LinkBurst text = makeBurst(true, TEXT_BYTES, rng);
    std::vector<Payload> p = segmentBursts({text}, 1);
    Reassembler r(SIGNALLING_BYTES, TEXT_BYTES);

    // Segment 2 missing: the partial burst is abandoned, and the segments
    // after the gap complete nothing.
    CHECK(reassemble(r, {p[0], p[1], p[3], p[4], p[5]}).empty());
    // Starting without segment 0 completes nothing either.
    CHECK(reassemble(r, {p[1], p[2], p[3], p[4], p[5]}, 10000000).empty());
    // The retried burst, whole, arrives.
    std::vector<LinkBurst> out = reassemble(r, p, 20000000);
    CHECK(out.size() == 1 && sameBurst(out[0], text));

    // A signalling burst in the middle of a text burst has its own partial.
    LinkBurst sig = makeBurst(false, SIGNALLING_BYTES, rng);
    std::vector<Payload> s = segmentBursts({sig}, 1);
    out = reassemble(r, {p[0], p[1], s[0], s[1], p[2], p[3], p[4], p[5]}, 30000000);
    CHECK(out.size() == 2);
    if (out.size() == 2)
    {
        CHECK(sameBurst(out[0], sig));
        CHECK(sameBurst(out[1], text));
    }
}

void testDuplicates()
{
    Random rng(5);
    LinkBurst sig = makeBurst(false, SIGNALLING_BYTES, rng);
    std::vector<Payload> p = segmentBursts({sig}, 1);
    Reassembler r(SIGNALLING_BYTES, TEXT_BYTES);
    LinkBurst burst;

    // Segment 0 decoded twice, by two gears or two overlapping searches, a
    // few samples apart: the second copy is ignored and does not restart
    // or break the burst.
    CHECK(!r.add(p[0], 50000, DUPLICATE_SAMPLES, burst));
    CHECK(!r.add(p[0], 50003, DUPLICATE_SAMPLES, burst));
    CHECK(r.add(p[1], 150000, DUPLICATE_SAMPLES, burst));
    CHECK(sameBurst(burst, sig));
    // The last segment again: no second copy of the burst.
    CHECK(!r.add(p[1], 150002, DUPLICATE_SAMPLES, burst));

    // The same burst sent again later is a new burst.
    CHECK(!r.add(p[0], 400000, DUPLICATE_SAMPLES, burst));
    CHECK(r.add(p[1], 500000, DUPLICATE_SAMPLES, burst));

    // reset() forgets partial bursts.
    CHECK(!r.add(p[0], 600000, DUPLICATE_SAMPLES, burst));
    r.reset();
    CHECK(!r.add(p[1], 700000, DUPLICATE_SAMPLES, burst));
}

// A full text burst over the air in the duet: three G5 frames, decoded and
// reassembled in voice order.
void testOverTheModem()
{
    Random rng(6);
    LinkBurst text = makeBurst(true, TEXT_BYTES, rng);
    std::vector<Payload> payloads = segmentBursts({text}, 2);
    CHECK(payloads.size() == 6);

    ModemSettings settings;
    settings.gear = 5;
    Reassembler r(SIGNALLING_BYTES, TEXT_BYTES);
    std::vector<LinkBurst> out;
    for (size_t frame = 0; frame * 2 < payloads.size(); frame++)
    {
        std::vector<float> x = modulate({payloads[2 * frame], payloads[2 * frame + 1]}, settings);
        std::vector<float> buffer = pad(x, 3000, 3000);
        addNoise(buffer, -6.0, meanSquare(x), rng);
        for (const Decode& d : receive(buffer.data(), buffer.size(), settings))
        {
            CHECK(d.ok);
            LinkBurst burst;
            long long start = (long long)frame * 100000 + d.startSample;
            if (d.ok && r.add(d.payload, start, DUPLICATE_SAMPLES, burst)) out.push_back(burst);
        }
    }
    CHECK(out.size() == 1 && sameBurst(out[0], text));
}

} // namespace

int main()
{
    testRoundTrip();
    testTrailingZeros();
    testDuetFiller();
    testOutOfOrder();
    testDuplicates();
    testOverTheModem();
    if (failures == 0) printf("glissando link tests passed\n");
    return failures == 0 ? 0 : 1;
}
