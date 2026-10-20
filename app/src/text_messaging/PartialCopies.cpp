//=========================================================================
// Name:            PartialCopies.cpp
// Purpose:         Merges copies of a text frame that lost segments.
//
// Written for Glissando; see PartialCopies.h.
//=========================================================================

#include "PartialCopies.h"

#include <algorithm>

#include "HamText.h"

namespace TextMessaging
{

namespace
{

constexpr int BLOCK_BYTES = HamText::TEXT_BLOCK_BITS / 8;

} // namespace

bool PartialCopies::sameFragment(const Frame& a, const Frame& b)
{
    return a.type == b.type && a.originCallsign == b.originCallsign && a.destinationCrc == b.destinationCrc &&
           a.airId == b.airId && a.fragmentIndex == b.fragmentIndex && a.fragmentCount == b.fragmentCount;
}

bool PartialCopies::add(const std::vector<uint8_t>& bytes, uint8_t missingBlocks, uint64_t nowMs, Frame& frameOut)
{
    copies_.erase(std::remove_if(copies_.begin(), copies_.end(),
                                 [&](const Copy& c) { return nowMs - c.heardMs > KEEP_MILLISECONDS; }),
                  copies_.end());

    Frame frame;
    if (!FrameCodec::decodePartial(bytes.data(), (int)bytes.size(), missingBlocks, frame)) return false;
    auto it = std::find_if(copies_.begin(), copies_.end(),
                           [&](const Copy& c) { return sameFragment(c.frame, frame); });

    if (missingBlocks == 0)
    {
        if (it != copies_.end()) copies_.erase(it);
        frameOut = frame;
        return true;
    }

    const int blocks = (int)(bytes.size() + BLOCK_BYTES - 1) / BLOCK_BYTES;
    auto has = [](uint8_t missing, int block) { return block >= 8 || !(missing & (1u << block)); };
    if (it != copies_.end())
    {
        // The first block carries what changes between retries (the bursts
        // still to come in that keying), so it is compared by the header it
        // decodes to, above; every other block must match where both have it.
        bool agree = it->bytes.size() == bytes.size();
        for (int block = 1; agree && block < blocks; block++)
        {
            if (!has(missingBlocks, block) || !has(it->missingBlocks, block)) continue;
            size_t from = (size_t)block * BLOCK_BYTES;
            size_t to = std::min(bytes.size(), from + BLOCK_BYTES);
            agree = std::equal(bytes.begin() + (long)from, bytes.begin() + (long)to, it->bytes.begin() + (long)from);
        }
        if (!agree)
        {
            copies_.erase(it);
            it = copies_.end();
        }
    }

    if (it == copies_.end())
    {
        if (copies_.size() >= MAX_COPIES) copies_.erase(copies_.begin());
        copies_.push_back({frame, bytes, missingBlocks, nowMs});
        frameOut = frame;
        return true;
    }

    // The newest copy's first block, and every block either copy has.
    std::vector<uint8_t> merged = bytes;
    for (int block = 1; block < blocks; block++)
    {
        if (has(missingBlocks, block) || !has(it->missingBlocks, block)) continue;
        size_t from = (size_t)block * BLOCK_BYTES;
        size_t to = std::min(bytes.size(), from + BLOCK_BYTES);
        std::copy(it->bytes.begin() + (long)from, it->bytes.begin() + (long)to, merged.begin() + (long)from);
    }
    uint8_t missing = (uint8_t)(missingBlocks & it->missingBlocks);
    Frame together;
    if (!FrameCodec::decodePartial(merged.data(), (int)merged.size(), missing, together)) return false;
    if (missing == 0)
    {
        copies_.erase(it);
    }
    else
    {
        it->frame = together;
        it->bytes = merged;
        it->missingBlocks = missing;
        it->heardMs = nowMs;
    }
    frameOut = together;
    return true;
}

} // namespace TextMessaging
