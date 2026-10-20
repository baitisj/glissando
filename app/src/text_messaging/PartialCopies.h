//=========================================================================
// Name:            PartialCopies.h
// Purpose:         Puts together a text frame from copies that each lost
//                  some of their segments.
//
// Written for Glissando, after JTTY, which prints what it heard of a
// message with "..." where a frame was lost. A Glissando text frame goes
// out as a burst of up to six segments, and a fade that takes one of them
// used to lose the frame until a retry got all six through. Now a burst with
// segments missing is read for what it has (FrameCodec::decodePartial()).
// The sender retries the whole frame under the same message ID, so what one
// copy lost another may have: the copies are merged here, block by block,
// and a frame whose blocks are all in from one copy or another is whole.
//=========================================================================

#ifndef TEXT_MESSAGING__PARTIAL_COPIES_H
#define TEXT_MESSAGING__PARTIAL_COPIES_H

#include <cstdint>
#include <string>
#include <vector>

#include "FrameCodec.h"

namespace TextMessaging
{

class PartialCopies
{
public:
    // A text frame as it arrived: its bytes, and the HamText blocks that
    // never came (bit i for block i, as FrameCodec::decodePartial() takes
    // them; 0 for a whole frame). Fills frameOut with what to hand the
    // protocol and returns true, or returns false when there is nothing to
    // read. A whole frame goes through as it is and forgets any copies of
    // it. A frame with blocks missing is merged with the copies of it heard
    // in the last KEEP_MILLISECONDS, the same message from the same sender
    // and the same fragment, and comes out whole when they cover it, or
    // partial with what they have between them. A copy that disagrees with
    // the ones before it where both have a block is a different message
    // (the sender's IDs started over) and replaces them.
    bool add(const std::vector<uint8_t>& bytes, uint8_t missingBlocks, uint64_t nowMs, Frame& frameOut);

    void clear() { copies_.clear(); }

    // Longer than a sender keeps retrying at the slowest tempo.
    static constexpr uint64_t KEEP_MILLISECONDS = 30 * 60 * 1000;
    static constexpr size_t MAX_COPIES = 32;

private:
    struct Copy
    {
        Frame frame;                // as last decoded
        std::vector<uint8_t> bytes; // the blocks heard so far, zero elsewhere
        uint8_t missingBlocks = 0;
        uint64_t heardMs = 0;
    };

    static bool sameFragment(const Frame& a, const Frame& b);

    std::vector<Copy> copies_;
};

} // namespace TextMessaging

#endif // TEXT_MESSAGING__PARTIAL_COPIES_H
