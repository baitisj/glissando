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
    byCallsignCrc_[FrameCodec::callsignCrc24(normalized)] = {tempo, nowMs};
}

bool StationTempos::fresh(const Heard& heard, uint64_t nowMs) const
{
    return nowMs >= heard.atMs && nowMs - heard.atMs < lifetimeMs_;
}

int StationTempos::answerTempo(const std::vector<OutgoingBurst>& keying, uint64_t nowMs) const
{
    if (keying.empty()) return 0;

    const OutgoingBurst& first = keying.front();
    Frame frame;
    if (!FrameCodec::decode(first.frame.data(), (int)first.frame.size(), frame)) return 0;
    if (frame.type != FrameType::PingAck && frame.type != FrameType::MessageAck &&
        frame.type != FrameType::MessagePartialAck)
    {
        return 0;
    }

    auto it = byCallsignCrc_.find(frame.destinationCrc);
    if (it == byCallsignCrc_.end() || !fresh(it->second, nowMs)) return 0;
    return it->second.tempo;
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
