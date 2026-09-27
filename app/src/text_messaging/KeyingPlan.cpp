//=========================================================================
// Name:            KeyingPlan.cpp
// Purpose:         Cuts a long chat keying between whole modem frames.
//
// Written for Glissando; see KeyingPlan.h.
//=========================================================================

#include "KeyingPlan.h"

namespace TextMessaging
{

std::vector<size_t> splitKeying(const std::vector<size_t>& frameEnds, size_t maxSamples)
{
    std::vector<size_t> keyingEnds;
    if (frameEnds.empty()) return keyingEnds;

    if (maxSamples == 0)
    {
        keyingEnds.push_back(frameEnds.back());
        return keyingEnds;
    }

    size_t start = 0;
    size_t end = 0;             // end of the last whole frame taken so far
    for (size_t frameEnd : frameEnds)
    {
        if (frameEnd - start > maxSamples && end > start)
        {
            keyingEnds.push_back(end);
            start = end;
        }
        end = frameEnd;
    }

    keyingEnds.push_back(end);
    return keyingEnds;
}

} // namespace TextMessaging
