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
// keying has an odd number of segments.
//
// Nothing here depends on wxWidgets or codec2.
//=========================================================================

#ifndef GLISSANDO__GLISSANDO_LINK_H
#define GLISSANDO__GLISSANDO_LINK_H

#include <cstdint>
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
};

// Cuts a keying's bursts into payloads, in the order they go on the air. For
// a duet (voices == 2) the list is padded to an even length with a filler, and
// payloads 2k and 2k+1 share frame k.
std::vector<Payload> segmentBursts(const std::vector<LinkBurst>& bursts, int voices);

// Number of Glissando frames segmentBursts() would produce.
int framesForBursts(const std::vector<LinkBurst>& bursts, int voices);

// Puts segments back together. Feed it every decoded payload in the order
// the frames arrived (voice 0 before voice 1 within a duet frame). A
// segment that does not follow on from the one before it in the same mode
// abandons the partial burst: the chat protocol retries whole bursts, so
// there is nothing to gain from holding on to half of one.
class Reassembler
{
public:
    // signallingBytes and textBytes are the padded sizes a completed burst
    // is restored to.
    Reassembler(int signallingBytes, int textBytes);

    // Returns true and fills burstOut when this payload completes a burst.
    // startSample is where the frame began in the receiver's sample count;
    // the same payload decoded again within duplicateSamples of the last one
    // (by a second gear that shares its waveform, or by an overlapping
    // search) is ignored.
    bool add(const Payload& payload, long long startSample, long long duplicateSamples,
             LinkBurst& burstOut);

    void reset();

private:
    struct Partial
    {
        bool active = false;
        int nextIndex = 0;
        std::vector<uint8_t> bytes;
    };

    int signallingBytes_;
    int textBytes_;
    Partial partial_[2];            // [0] signalling, [1] text

    bool haveLast_;
    Payload lastPayload_;
    long long lastStartSample_;
};

} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_LINK_H
