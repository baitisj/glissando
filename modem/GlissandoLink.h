//=========================================================================
// Name:            GlissandoLink.h
// Purpose:         Carries text chat frames over Glissando's 77 bit payloads.
//
// A chat frame is up to TEXT_FRAME_BYTES long, a Glissando frame carries 77
// bits, so each chat burst is cut into segments of SEGMENT_DATA_BYTES with a
// five bit header:
//
//   bit 0      burst mode: 0 signalling, 1 text
//   bits 1-3   segment index within the burst, first segment 0
//   bit 4      last segment of the burst
//   bits 5-76  nine bytes of the chat frame, first byte first, MSB first
//
// Chat frames are zero padded to their fixed size, and the padding is not
// sent: trailing zero bytes are dropped before cutting and put back on
// arrival, so a short message costs fewer (slow) frames. Index 7 never
// occurs in real traffic (a text frame needs at most six segments) and marks
// a filler segment, which pads the second voice of a duet frame when a
// keying has an odd number of segments. Its nine bytes are zero, or a whole
// chat frame short enough for one segment, which the chat layer uses to send
// the station's locator for free; builds that predate that skip a filler
// unread.
//
// Nothing here depends on wxWidgets or codec2.
//=========================================================================

#ifndef GLISSANDO__GLISSANDO_LINK_H
#define GLISSANDO__GLISSANDO_LINK_H

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "GlissandoModem.h"

namespace Glissando
{

constexpr int SEGMENT_HEADER_BITS = 5;
constexpr int SEGMENT_DATA_BYTES = (PAYLOAD_BITS - SEGMENT_HEADER_BITS) / 8; // 9
constexpr int FILLER_SEGMENT_INDEX = 7;

struct LinkBurst
{
    bool text = true;               // false: a signalling burst
    std::vector<uint8_t> bytes;     // the whole chat frame, padding included
    bool filler = false;            // received: it came in a filler segment

    // Received: the segments that never arrived, bit i for segment i. Their
    // bytes are zero. 0 for a whole burst. See Reassembler.
    uint8_t missingSegments = 0;
    int stream = 0;                 // received: the stream it was put together in
};

// Cuts a keying's bursts into payloads, in the order they go on the air. For
// a duet (voices == 2) the list is padded to an even length with a filler, and
// payloads 2k and 2k+1 share frame k. A filler carries fillerFrame when it
// fits one segment, and zeros otherwise.
std::vector<Payload> segmentBursts(const std::vector<LinkBurst>& bursts, int voices,
                                   const std::vector<uint8_t>& fillerFrame = {});

// Number of Glissando frames segmentBursts() would produce.
int framesForBursts(const std::vector<LinkBurst>& bursts, int voices);

// A payload read as a segment: its header and its nine bytes.
struct Segment
{
    bool text = false;
    int index = 0;
    bool last = false;
    std::array<uint8_t, SEGMENT_DATA_BYTES> bytes{};
};

Segment readSegment(const Payload& payload);

// Whether segmentBursts() could have produced this payload for chat frames
// of these sizes: an index the frame size allows, the last flag on the
// segment that must be last, zero bytes past the end of the frame, and a
// filler only as signalling. About 14 random payloads in 32 pass. For
// ModemSettings::payloadCheck and StreamingReceiver::setPayloadCheck().
bool isLinkSegment(const Payload& payload, int signallingBytes, int textBytes);

// The bits a receiver can count on in the first segment of a burst, given
// what it expects of the burst's first bytes: bits set in masks[i] are known
// to equal those bits of bytes[i] (count at most SEGMENT_DATA_BYTES). The
// segment index (0) is known too; the burst mode and the last flag are not,
// since a burst can be one segment long. For
// StreamingReceiver::setKnownBits().
KnownBits firstSegmentKnownBits(const uint8_t* bytes, const uint8_t* masks, int count);

// What Reassembler::add() made of one payload, for showing reception as it
// happens rather than only when a burst completes.
struct SegmentProgress
{
    bool duplicate = false;     // the same frame decoded again, and ignored
    bool filler = false;        // a duet's padding segment
    bool text = false;
    int index = 0;
    bool last = false;

    // The burst so far, at least up to this segment's SEGMENT_DATA_BYTES, so
    // it is at least (index + 1) * SEGMENT_DATA_BYTES long. Bytes before
    // knownFrom are not all known (a segment before this one went missing,
    // and its bytes are zero) and bytes from it to this segment are.
    std::vector<uint8_t> bytes;
    int knownFrom = 0;
    bool completed = false;     // add() returned a burst for this segment
};

// Puts segments back together. Feed it every decoded payload in the order
// the frames arrived (voice 0 before voice 1 within a duet frame).
//
// A burst that loses a segment to a fade is not thrown away: what arrived
// can still be read (each segment's text decodes on its own), as JTTY
// prints a message with "..." where a frame was lost. Once its first
// segment is in, a burst takes the segments after it as they come, gaps
// and all, and also a missing one decoded late (the receiver's list
// decoding and subtraction can report a frame after the one that followed
// it). When its last segment arrives it is returned with the gaps in
// LinkBurst::missingSegments, and is held a while longer in case they turn
// up, when it is returned again, whole. When the last segment never comes,
// the burst is cut short as the next burst in its stream begins or
// (expire()) when even its last segment would have arrived by now, and
// takeIncomplete() hands it over with everything not heard missing. A burst
// whose first segment was lost cannot be placed, since that segment says
// who sent it, and is not kept.
//
// Bursts from different stations can be heard interleaved: a station at
// Presto sends a whole burst between two frames of one at Adagio. Each is
// kept apart by its stream, which the caller gives as anything that tells
// the senders apart; the modem uses the tempo's symbol length and the scale.
// Without that, the faster burst abandoned the slower one half way, and an
// Adagio pong was never heard.
class Reassembler
{
public:
    // signallingBytes and textBytes are the padded sizes a completed burst
    // is restored to.
    Reassembler(int signallingBytes, int textBytes);

    // Returns true and fills burstOut when this payload completes a burst,
    // or is a filler that carries a frame (burstOut.filler), which leaves
    // any burst being put together alone.
    // startSample is where the frame began in the receiver's sample count;
    // the same payload decoded again within duplicateSamples of the last one
    // (by a second gear that shares its waveform, or by an overlapping
    // search) is ignored.
    //
    // frameSamples and voices are the length of the frame the payload came
    // in and how many payloads such a frame carries. With them a segment
    // that arrives after a gap is checked against the burst's timing, so
    // that one from a later burst whose first segment was lost is not taken
    // for part of this one, and expire() knows when a burst is over. 0 for
    // no timing.
    bool add(const Payload& payload, long long startSample, long long duplicateSamples,
             LinkBurst& burstOut, SegmentProgress* progressOut = nullptr, int stream = 0,
             long long frameSamples = 0, int voices = 1);

    // Cuts short every burst whose last segment would have arrived before
    // nowSample (in add()'s sample count), with a frame and a half to spare
    // for the receiver's search.
    void expire(long long nowSample);

    // The bursts cut short since the last call, oldest first.
    std::vector<LinkBurst> takeIncomplete();

    // Whether a burst in this stream has begun and not yet ended on the air.
    bool inProgress(int stream) const;

    void reset();

private:
    struct Partial
    {
        bool active = false;
        int nextIndex = 0;              // one past the highest segment heard
        unsigned heard = 0;             // bit i: segment i arrived
        std::vector<uint8_t> bytes;     // nextIndex segments, zero where one is missing
        int latestIndex = 0;            // the highest segment heard,
        long long latestStart = 0;      // where its frame began,
        long long frameSamples = 0;     // and the timing it came with
        int voices = 1;
        int lastIndex = -1;             // the segment flagged last, once heard
        int stream = 0;
    };

    int maxSegments(bool text) const;
    unsigned wholeMask(const Partial& partial) const;
    LinkBurst burstOf(const Partial& partial, bool text) const;
    void cutShort(Partial& partial, bool text);

    int signallingBytes_;
    int textBytes_;
    std::map<int, std::array<Partial, 2>> partials_;  // per stream: [0] signalling, [1] text
    std::vector<LinkBurst> incomplete_;

    bool haveLast_;
    Payload lastPayload_;
    long long lastStartSample_;
};

} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_LINK_H
