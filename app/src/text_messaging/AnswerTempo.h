//=========================================================================
// Name:            AnswerTempo.h
// Purpose:         Remembers the tempo each station was last heard in, so
//                  a reply goes back in the tempo it was asked in and the
//                  waits for an answer need only cover tempos in use.
//
// Written for Glissando. Before this a station answered in whatever tempo
// it sent at, so a station waiting on an answer had to allow for the
// slowest tempo it listens for: at Presto an unanswered ping held the
// queue for 111 s and gave up after 120 s, for an answer that at Presto
// would have been heard within 30 s. Answering in the asker's tempo makes
// the wait the asker's own, and the path has just shown it carries it.
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

    // The tempo a keying should go out in if it opens with a reply (an
    // acknowledgement, partial acknowledgement or pong): the one its
    // station was last heard in. Anything riding behind the reply goes in
    // the same keying and so the same tempo. Zero for a keying that opens
    // with anything else, or a reply to a station not heard lately.
    int answerTempo(const std::vector<OutgoingBurst>& keying, uint64_t nowMs) const;

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
