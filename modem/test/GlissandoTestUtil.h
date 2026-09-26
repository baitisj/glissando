//=========================================================================
// Name:            GlissandoTestUtil.h
// Purpose:         Checks, a portable random number generator and the
//                  prototype's channel model (prototype/channel.py) for the
//                  Glissando tests.
//=========================================================================

#ifndef GLISSANDO__TEST_UTIL_H
#define GLISSANDO__TEST_UTIL_H

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../GlissandoInternal.h"
#include "../GlissandoModem.h"

namespace GlissandoTest
{

inline int failures = 0;

inline void check(bool condition, const char* what, int line)
{
    if (!condition)
    {
        failures++;
        fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// SplitMix64 and Box-Muller: the same noise on every platform and standard
// library, so a fixed seed means the same test everywhere.
class Random
{
public:
    explicit Random(uint64_t seed)
        : state_(seed)
    {
    }

    uint64_t next()
    {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [0, 1).
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }

    double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

    double gaussian()
    {
        if (haveSpare_)
        {
            haveSpare_ = false;
            return spare_;
        }
        double u1 = uniform();
        double u2 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;
        double r = std::sqrt(-2.0 * std::log(u1));
        spare_ = r * std::sin(2.0 * 3.14159265358979323846 * u2);
        haveSpare_ = true;
        return r * std::cos(2.0 * 3.14159265358979323846 * u2);
    }

    Glissando::Payload payload()
    {
        Glissando::Payload p{};
        for (auto& bit : p) bit = (uint8_t)(next() >> 63);
        return p;
    }

private:
    uint64_t state_;
    bool haveSpare_ = false;
    double spare_ = 0.0;
};

inline double meanSquare(const std::vector<float>& x)
{
    double sum = 0.0;
    for (float v : x) sum += (double)v * v;
    return x.empty() ? 0.0 : sum / (double)x.size();
}

// channel.add_noise(): SNR is signal power over noise power in 2500 Hz (the
// WSJT-X convention), signalPower the average power of the transmission.
inline void addNoise(std::vector<float>& x, double snrDb, double signalPower, Random& rng)
{
    double n0 = signalPower / std::pow(10.0, snrDb / 10.0) / 2500.0; // one-sided noise PSD per Hz
    double sigma = std::sqrt(n0 * Glissando::SAMPLE_RATE_HZ / 2.0);
    for (float& v : x) v = (float)(v + sigma * rng.gaussian());
}

// channel.freq_shift(): moves the whole spectrum by df Hz. Not the same
// as transmitting with a tuning offset, which glides between shifted notes
// (a glide is linear in log frequency, so shifting it is not re-tuning it).
inline void frequencyShift(std::vector<float>& x, double dfHz)
{
    Glissando::detail::ComplexSignal z;
    Glissando::detail::analyticSignal(x.data(), x.size(), z);
    for (size_t n = 0; n < x.size(); n++)
    {
        double phase = 2.0 * 3.14159265358979323846 * std::fmod(dfHz * (double)n / Glissando::SAMPLE_RATE_HZ, 1.0);
        x[n] = (float)(z.re[n] * std::cos(phase) - z.im[n] * std::sin(phase));
    }
}

// Silence before and after a transmission.
inline std::vector<float> pad(const std::vector<float>& x, size_t before, size_t after)
{
    std::vector<float> out(before, 0.0f);
    out.insert(out.end(), x.begin(), x.end());
    out.resize(out.size() + after, 0.0f);
    return out;
}

inline double secondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace GlissandoTest

#endif // GLISSANDO__TEST_UTIL_H
