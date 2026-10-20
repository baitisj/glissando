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

std::vector<Payload> segmentBursts(const std::vector<LinkBurst>& bursts, int voices,
                                   const std::vector<uint8_t>& fillerFrame)
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
        LinkBurst filler{false, fillerFrame};
        bool fits = !fillerFrame.empty() && sentLength(filler) <= SEGMENT_DATA_BYTES;
        while (payloads.size() % (size_t)voices != 0)
        {
            payloads.push_back(makeSegment(false, FILLER_SEGMENT_INDEX, false,
                                           fits ? fillerFrame.data() : nullptr,
                                           fits ? sentLength(filler) : 0));
        }
    }
    return payloads;
}

KnownBits firstSegmentKnownBits(const uint8_t* bytes, const uint8_t* masks, int count)
{
    KnownBits known = noKnownBits();
    // Header: mode (unknown), index 0 in three bits. Whether it is the last
    // segment is unknown too: a pong or an acknowledgement is one segment.
    for (int bit = 1; bit < SEGMENT_HEADER_BITS - 1; bit++) known[(size_t)bit] = 0;
    count = std::min(std::max(count, 0), SEGMENT_DATA_BYTES);
    for (int i = 0; i < count; i++)
    {
        for (int bit = 0; bit < 8; bit++)
        {
            uint8_t weight = (uint8_t)(0x80 >> bit);
            if (masks[i] & weight)
                known[(size_t)(SEGMENT_HEADER_BITS + 8 * i + bit)] = (bytes[i] & weight) ? 1 : 0;
        }
    }
    return known;
}

Segment readSegment(const Payload& payload)
{
    Segment segment;
    int position = 0;
    segment.text = getBits(payload, position, 1) != 0;
    segment.index = (int)getBits(payload, position, 3);
    segment.last = getBits(payload, position, 1) != 0;
    for (uint8_t& byte : segment.bytes) byte = (uint8_t)getBits(payload, position, 8);
    return segment;
}

bool isLinkSegment(const Payload& payload, int signallingBytes, int textBytes)
{
    Segment segment = readSegment(payload);
    if (segment.index == FILLER_SEGMENT_INDEX) return !segment.text && !segment.last;

    int bytes = std::max(segment.text ? textBytes : signallingBytes, 1);
    int segments = (bytes + SEGMENT_DATA_BYTES - 1) / SEGMENT_DATA_BYTES;
    if (segment.index >= segments) return false;
    if (segment.index == segments - 1 && !segment.last) return false;
    // Past the end of the frame the segment is zero padding.
    for (int i = std::max(bytes - segment.index * SEGMENT_DATA_BYTES, 0); i < SEGMENT_DATA_BYTES; i++)
    {
        if (segment.bytes[(size_t)i] != 0) return false;
    }
    return true;
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
    partials_.clear();
    incomplete_.clear();
    haveLast_ = false;
}

bool Reassembler::inProgress(int stream) const
{
    auto it = partials_.find(stream);
    if (it == partials_.end()) return false;
    for (const Partial& partial : it->second)
    {
        if (partial.active && partial.lastIndex < 0) return true;
    }
    return false;
}

int Reassembler::maxSegments(bool text) const
{
    int bytes = std::max(text ? textBytes_ : signallingBytes_, 1);
    return (bytes + SEGMENT_DATA_BYTES - 1) / SEGMENT_DATA_BYTES;
}

LinkBurst Reassembler::burstOf(const Partial& partial, bool text) const
{
    LinkBurst burst;
    burst.text = text;
    burst.stream = partial.stream;
    burst.bytes = partial.bytes;
    burst.bytes.resize((size_t)(text ? textBytes_ : signallingBytes_), 0);
    // Everything not heard is missing, up to the last segment or, when that
    // never came, to as many segments as the frame can have.
    int segments = partial.lastIndex >= 0 ? partial.lastIndex + 1 : maxSegments(text);
    for (int i = 0; i < segments && i < 8; i++)
    {
        if (!(partial.heard & (1u << i))) burst.missingSegments |= (uint8_t)(1u << i);
    }
    return burst;
}

void Reassembler::cutShort(Partial& partial, bool text)
{
    // One already handed over with its gaps has nothing new to say.
    if (partial.active && partial.lastIndex < 0) incomplete_.push_back(burstOf(partial, text));
    partial = Partial();
}

void Reassembler::expire(long long nowSample)
{
    for (auto& stream : partials_)
    {
        for (int mode = 0; mode < 2; mode++)
        {
            Partial& partial = stream.second[(size_t)mode];
            if (!partial.active || partial.frameSamples <= 0) continue;
            // When the burst's last segment would have begun, at the latest,
            // and a frame and a half for it to be heard.
            int still = std::max(maxSegments(mode == 1) - 1 - partial.latestIndex, 0);
            long long over = partial.latestStart + (long long)still * partial.frameSamples / partial.voices +
                             partial.frameSamples * 3 / 2;
            if (nowSample > over) cutShort(partial, mode == 1);
        }
    }
}

std::vector<LinkBurst> Reassembler::takeIncomplete()
{
    std::vector<LinkBurst> out;
    out.swap(incomplete_);
    return out;
}

bool Reassembler::add(const Payload& payload, long long startSample, long long duplicateSamples,
                      LinkBurst& burstOut, SegmentProgress* progressOut, int stream, long long frameSamples,
                      int voices)
{
    SegmentProgress progress;
    SegmentProgress& p = progressOut != nullptr ? *progressOut : progress;
    p = SegmentProgress();
    voices = std::max(voices, 1);

    if (haveLast_ && payload == lastPayload_ &&
        std::llabs(startSample - lastStartSample_) <= duplicateSamples)
    {
        p.duplicate = true;
        return false;
    }
    haveLast_ = true;
    lastPayload_ = payload;
    lastStartSample_ = startSample;

    const Segment segment = readSegment(payload);
    const bool text = segment.text;
    const int index = segment.index;
    const bool last = segment.last;
    p.text = text;
    p.index = index;
    p.last = last;
    const uint8_t* data = segment.bytes.data();

    if (index == FILLER_SEGMENT_INDEX)
    {
        p.filler = true;
        bool carries = false;
        for (uint8_t byte : segment.bytes) carries = carries || byte != 0;
        if (!carries) return false;

        p.bytes.assign(data, data + SEGMENT_DATA_BYTES);
        p.completed = true;
        burstOut = LinkBurst();
        burstOut.text = false;
        burstOut.filler = true;
        burstOut.stream = stream;
        burstOut.bytes.assign(data, data + SEGMENT_DATA_BYTES);
        burstOut.bytes.resize((size_t)std::max(signallingBytes_, SEGMENT_DATA_BYTES), 0);
        return true;
    }

    // What a segment that cannot be placed in a burst still shows: its own
    // bytes at their place.
    auto alone = [&]() {
        if (progressOut == nullptr) return;
        p.knownFrom = index * SEGMENT_DATA_BYTES;
        p.bytes.assign((size_t)p.knownFrom, 0);
        p.bytes.insert(p.bytes.end(), data, data + SEGMENT_DATA_BYTES);
    };

    // No frame of this mode has that many segments.
    if (index >= maxSegments(text))
    {
        alone();
        return false;
    }

    Partial& partial = partials_[stream][text ? 1 : 0];
    if (index == 0)
    {
        cutShort(partial, text);
        partial.active = true;
        partial.stream = stream;
    }
    else if (!partial.active)
    {
        alone();
        return false;
    }
    else if (partial.heard & (1u << index))
    {
        // That segment again, decoded twice out of turn.
        if (std::equal(data, data + SEGMENT_DATA_BYTES, partial.bytes.begin() + index * SEGMENT_DATA_BYTES))
        {
            p.duplicate = true;
            return false;
        }
        // Otherwise a later burst whose first segment was lost.
        cutShort(partial, text);
        alone();
        return false;
    }
    else if ((partial.lastIndex >= 0 && (index > partial.lastIndex || last)) ||
             (last && index < partial.nextIndex - 1))
    {
        // Past the end of this burst, or its end before segments already
        // heard: another burst.
        cutShort(partial, text);
        alone();
        return false;
    }
    else if (partial.frameSamples > 0 && frameSamples > 0)
    {
        // Segment k of a burst goes out about k frames after segment 0 (k / 2
        // in a duet); a frame's slack covers how a duet pairs them.
        long long expected = partial.latestStart + (long long)(index - partial.latestIndex) *
                                                       partial.frameSamples / partial.voices;
        if (std::llabs(startSample - expected) > partial.frameSamples)
        {
            cutShort(partial, text);
            alone();
            return false;
        }
    }

    if (index >= partial.nextIndex)
    {
        partial.nextIndex = index + 1;
        partial.bytes.resize((size_t)partial.nextIndex * SEGMENT_DATA_BYTES, 0);
    }
    std::copy(data, data + SEGMENT_DATA_BYTES, partial.bytes.begin() + index * SEGMENT_DATA_BYTES);
    partial.heard |= 1u << index;
    if (index >= partial.latestIndex)
    {
        partial.latestIndex = index;
        partial.latestStart = startSample;
        partial.frameSamples = frameSamples;
        partial.voices = voices;
    }
    if (last) partial.lastIndex = index;

    if (progressOut != nullptr)
    {
        p.bytes = partial.bytes;
        int from = index;
        while (from > 0 && (partial.heard & (1u << (from - 1)))) from--;
        p.knownFrom = from * SEGMENT_DATA_BYTES;
    }

    // Once the last segment is in, the burst is handed over, and again with
    // each gap filled after that.
    if (partial.lastIndex < 0) return false;
    p.completed = true;
    burstOut = burstOf(partial, text);
    if (burstOut.missingSegments == 0) partial = Partial();
    return true;
}

} // namespace Glissando
