//=========================================================================
// Name:            GlissandoLink.cpp
// Purpose:         Carries text chat frames over Glissando's 77 bit payloads.
//=========================================================================

#include "GlissandoLink.h"

#include <algorithm>
#include <cstdlib>

namespace Glissando
{

namespace
{

void putBits(Payload& payload, int& position, unsigned value, int bits)
{
    for (int bit = bits - 1; bit >= 0; bit--)
    {
        payload[position++] = (uint8_t)((value >> bit) & 1);
    }
}

unsigned getBits(const Payload& payload, int& position, int bits)
{
    unsigned value = 0;
    for (int bit = 0; bit < bits; bit++)
    {
        value = (value << 1) | (payload[position++] & 1);
    }
    return value;
}

Payload makeSegment(bool text, int index, bool last, const uint8_t* data, int length)
{
    Payload payload{};
    int position = 0;
    putBits(payload, position, text ? 1 : 0, 1);
    putBits(payload, position, (unsigned)index, 3);
    putBits(payload, position, last ? 1 : 0, 1);
    for (int i = 0; i < SEGMENT_DATA_BYTES; i++)
    {
        putBits(payload, position, i < length ? data[i] : 0, 8);
    }
    return payload;
}

// Bytes of a burst actually sent: everything up to the last non-zero byte,
// and at least one byte so an all-zero frame still arrives as something.
int sentLength(const LinkBurst& burst)
{
    int length = (int)burst.bytes.size();
    while (length > 1 && burst.bytes[length - 1] == 0) length--;
    return std::max(length, 1);
}

int segmentsFor(const LinkBurst& burst)
{
    return (sentLength(burst) + SEGMENT_DATA_BYTES - 1) / SEGMENT_DATA_BYTES;
}

} // namespace

std::vector<Payload> segmentBursts(const std::vector<LinkBurst>& bursts, int voices)
{
    std::vector<Payload> payloads;
    for (const LinkBurst& burst : bursts)
    {
        int length = sentLength(burst);
        int segments = segmentsFor(burst);
        std::vector<uint8_t> bytes(burst.bytes.begin(), burst.bytes.end());
        bytes.resize(std::max<size_t>(bytes.size(), 1), 0);

        for (int index = 0; index < segments; index++)
        {
            int offset = index * SEGMENT_DATA_BYTES;
            payloads.push_back(makeSegment(burst.text, index, index == segments - 1,
                                           bytes.data() + offset,
                                           std::min(SEGMENT_DATA_BYTES, length - offset)));
        }
    }

    if (voices > 1)
    {
        while (payloads.size() % (size_t)voices != 0)
        {
            payloads.push_back(makeSegment(false, FILLER_SEGMENT_INDEX, false, nullptr, 0));
        }
    }
    return payloads;
}

int framesForBursts(const std::vector<LinkBurst>& bursts, int voices)
{
    int segments = 0;
    for (const LinkBurst& burst : bursts) segments += segmentsFor(burst);
    voices = std::max(voices, 1);
    return (segments + voices - 1) / voices;
}

Reassembler::Reassembler(int signallingBytes, int textBytes)
    : signallingBytes_(signallingBytes)
    , textBytes_(textBytes)
    , haveLast_(false)
    , lastPayload_{}
    , lastStartSample_(0)
{
    // empty
}

void Reassembler::reset()
{
    partial_[0] = Partial();
    partial_[1] = Partial();
    haveLast_ = false;
}

bool Reassembler::add(const Payload& payload, long long startSample, long long duplicateSamples,
                      LinkBurst& burstOut)
{
    if (haveLast_ && payload == lastPayload_ &&
        std::llabs(startSample - lastStartSample_) <= duplicateSamples)
    {
        return false;
    }
    haveLast_ = true;
    lastPayload_ = payload;
    lastStartSample_ = startSample;

    int position = 0;
    bool text = getBits(payload, position, 1) != 0;
    int index = (int)getBits(payload, position, 3);
    bool last = getBits(payload, position, 1) != 0;
    if (index == FILLER_SEGMENT_INDEX) return false;

    Partial& partial = partial_[text ? 1 : 0];
    if (index == 0)
    {
        partial = Partial();
        partial.active = true;
    }
    else if (!partial.active || index != partial.nextIndex)
    {
        partial = Partial();
        return false;
    }

    for (int i = 0; i < SEGMENT_DATA_BYTES; i++)
    {
        partial.bytes.push_back((uint8_t)getBits(payload, position, 8));
    }
    partial.nextIndex = index + 1;

    if (!last) return false;

    int size = text ? textBytes_ : signallingBytes_;
    partial.bytes.resize((size_t)size, 0);
    burstOut.text = text;
    burstOut.bytes = std::move(partial.bytes);
    partial = Partial();
    return true;
}

} // namespace Glissando
