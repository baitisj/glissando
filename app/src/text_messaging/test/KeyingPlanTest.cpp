//=========================================================================
// Name:            KeyingPlanTest.cpp
// Purpose:         Tests for splitKeying().
//=========================================================================

#include <cstdio>
#include <vector>

#include "KeyingPlan.h"

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

std::vector<size_t> evenFrames(int count, size_t frameSamples)
{
    std::vector<size_t> ends;
    for (int i = 1; i <= count; i++) ends.push_back((size_t)i * frameSamples);
    return ends;
}

} // namespace

int main()
{
    const size_t RATE = 8000;
    const size_t ADAGIO = 55 * RATE;        // close enough: 86 symbols of 0.64 s

    // No limit, or a keying that fits: left alone.
    {
        std::vector<size_t> ends = splitKeying(evenFrames(6, ADAGIO), 0);
        check(ends == std::vector<size_t>{6 * ADAGIO}, "no limit gives one keying");

        ends = splitKeying(evenFrames(3, ADAGIO), 175 * RATE);
        check(ends == std::vector<size_t>{3 * ADAGIO}, "three Adagio frames fit in 175 s");
    }

    // One full text fragment at Adagio is six frames: two keyings of three.
    {
        std::vector<size_t> ends = splitKeying(evenFrames(6, ADAGIO), 175 * RATE);
        check(ends == (std::vector<size_t>{3 * ADAGIO, 6 * ADAGIO}), "six Adagio frames make 3 + 3");
    }

    // A reply and a full message, 50 frames: every keying three frames but
    // the last, and every cut on a frame boundary.
    {
        std::vector<size_t> frames = evenFrames(50, ADAGIO);
        std::vector<size_t> ends = splitKeying(frames, 175 * RATE);
        check(ends.size() == 17, "fifty Adagio frames make seventeen keyings");
        size_t start = 0;
        for (size_t end : ends)
        {
            check(end % ADAGIO == 0, "cut falls between frames");
            check(end - start <= 175 * RATE, "each keying within the limit");
            start = end;
        }
        check(ends.back() == frames.back(), "the last keying ends with the burst");
    }

    // Exactly at the limit still fits.
    {
        std::vector<size_t> ends = splitKeying(evenFrames(4, 10), 20);
        check(ends == (std::vector<size_t>{20, 40}), "frames that exactly fill the limit");
    }

    // A frame longer than the limit on its own goes out whole, alone.
    {
        std::vector<size_t> ends = splitKeying(evenFrames(3, ADAGIO), 30 * RATE);
        check(ends == (std::vector<size_t>{ADAGIO, 2 * ADAGIO, 3 * ADAGIO}),
              "an oversized frame is never split");
    }

    // Uneven frames, as codec2 mixes signalling and text bursts.
    {
        std::vector<size_t> frames{5, 25, 30, 50, 55};
        std::vector<size_t> ends = splitKeying(frames, 25);
        check(ends == (std::vector<size_t>{25, 50, 55}), "uneven frames packed greedily");
    }

    check(splitKeying({}, 100).empty(), "nothing to send gives no keyings");

    if (failures == 0) std::printf("All keying plan tests passed\n");
    return failures == 0 ? 0 : 1;
}
