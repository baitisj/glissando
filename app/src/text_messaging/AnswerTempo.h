//=========================================================================
// Name:            AnswerTempo.h
// Purpose:         Remembers the tempo each station was last heard in, so
//                  the waits for an answer need only cover tempos in use.
//
// Written for Glissando. Every station answers in the tempo it has chosen
// to send at: a QRP station on a slow tempo is heard where it would not be
// at a fast station's tempo (Jeff, 2026-10-03). So an answer can come back
// in any tempo its station sends at. The waits used to allow for the
// slowest tempo the receiver listens for, and at Presto an unanswered ping
// held the queue for 111 s and gave up after 120 s; now they allow for the
// tempos actually heard lately.
//=========================================================================

#ifndef TEXT_MESSAGING__ANSWER_TEMPO_H
#define TEXT_MESSAGING__ANSWER_TEMPO_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "TextMessagingTypes.h"

namespace TextMessaging
{

// Tempos are whatever small positive integers the modem numbers them by
// (Glissando's gears); zero means none. Not thread safe.
class StationTempos
{
public:
    // How long a station's tempo is remembered after it was last heard.
    explicit StationTempos(uint64_t lifetimeMs) : lifetimeMs_(lifetimeMs) {}

    void heard(const std::string& callsign, int tempo, uint64_t nowMs);

    // Every tempo heard from anybody lately, each once.
    std::vector<int> recentTempos(uint64_t nowMs) const;

private:
    struct Heard
    {
        int tempo = 0;
        uint64_t atMs = 0;
    };

    bool fresh(const Heard& heard, uint64_t nowMs) const;

    uint64_t lifetimeMs_;
    std::map<uint32_t, Heard> byCallsignCrc_;
};

} // namespace TextMessaging

#endif // TEXT_MESSAGING__ANSWER_TEMPO_H
