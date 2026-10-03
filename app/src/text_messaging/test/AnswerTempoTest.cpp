//=========================================================================
// Name:            AnswerTempoTest.cpp
// Purpose:         Tests for StationTempos.
//=========================================================================

#include <cstdio>
#include <vector>

#include "AnswerTempo.h"

using namespace TextMessaging;

namespace
{

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

} // namespace

int main()
{
    const uint64_t LIFETIME = 15 * 60 * 1000;
    const int ADAGIO = 1;
    const int PRESTO = 4;

    StationTempos tempos(LIFETIME);
    tempos.heard("vk3abc", PRESTO, 1000);
    tempos.heard("K1ABC", ADAGIO, 2000);

    // Heard again, in another tempo: the latest counts.
    tempos.heard("VK3ABC", ADAGIO, 6000);
    check(tempos.recentTempos(3000 + LIFETIME) == std::vector<int>({ADAGIO}), "latest tempo replaces the old");

    // Tempos heard lately, each once, until they age out.
    tempos.heard("G4XYZ", PRESTO, 8000);
    std::vector<int> recent = tempos.recentTempos(9000);
    check(recent == std::vector<int>({ADAGIO, PRESTO}), "recent tempos");
    check(tempos.recentTempos(8000 + LIFETIME) == std::vector<int>(), "every tempo ages out");
    check(tempos.recentTempos(6000 + LIFETIME) == std::vector<int>({PRESTO}), "the oldest ages out first");

    // Nothing is remembered for a tempo of zero or an empty callsign.
    StationTempos empty(LIFETIME);
    empty.heard("", PRESTO, 0);
    empty.heard("W1AW", 0, 0);
    check(empty.recentTempos(1).empty(), "nothing usable heard");

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}
