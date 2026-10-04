//=========================================================================
// Name:            AnswerTempo.cpp
// Purpose:         Remembers the tempo each station was last heard in.
//=========================================================================

#include "AnswerTempo.h"

#include <algorithm>

#include "FrameCodec.h"

namespace TextMessaging
{

void StationTempos::heard(const std::string& callsign, int tempo, uint64_t nowMs)
{
    if (tempo <= 0) return;
    std::string normalized = FrameCodec::normalizeCallsign(callsign);
    if (normalized.empty()) return;
    byCallsignCrc_[FrameCodec::callsignHash(normalized)] = {tempo, nowMs};
}

bool StationTempos::fresh(const Heard& heard, uint64_t nowMs) const
{
    return nowMs >= heard.atMs && nowMs - heard.atMs < lifetimeMs_;
}

std::vector<int> StationTempos::recentTempos(uint64_t nowMs) const
{
    std::vector<int> tempos;
    for (const auto& entry : byCallsignCrc_)
    {
        if (!fresh(entry.second, nowMs)) continue;
        if (std::find(tempos.begin(), tempos.end(), entry.second.tempo) == tempos.end())
        {
            tempos.push_back(entry.second.tempo);
        }
    }
    std::sort(tempos.begin(), tempos.end());
    return tempos;
}

} // namespace TextMessaging
