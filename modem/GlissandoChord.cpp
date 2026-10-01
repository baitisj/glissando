//=========================================================================
// Name:            GlissandoChord.cpp
// Purpose:         The opening chord and the listener that hears it.
//=========================================================================

#include "GlissandoChord.h"

#include <algorithm>
#include <cmath>


namespace Glissando
{

namespace
{

constexpr double PI = 3.14159265358979323846;

// One analysis window is exactly the chord; a new one every tenth of a
// second, so the best lines up with the chord to within 50 ms. Zero padded
// to 8192 points, about a hertz a bin, so the tuning search steps a bin at
// a time.
constexpr int WINDOW = (int)(OPENING_CHORD_SECONDS * SAMPLE_RATE_HZ);
constexpr int HOP = SAMPLE_RATE_HZ / 10;
constexpr int FFT_SIZE = 8192;
constexpr double BIN_HZ = (double)SAMPLE_RATE_HZ / FFT_SIZE;

// The noise level is the median bin across the band the scales use and
// some way either side, which a few loud notes cannot move.
constexpr double NOISE_LOW_HZ = 200.0;
constexpr double NOISE_HIGH_HZ = 3000.0;

// A note sounding in a chord whose two powers differ more than this is a
// carrier with noise beside it, not a chord.
constexpr double MAX_NOTE_RATIO = 30.0;

// E4 and D5, a minor seventh, in every scale (the pentatonic's lowest note
// and fifth, MIDI 64 and 74 in the others).
constexpr double CHORD_LOW_HZ = 329.63;
constexpr double CHORD_HIGH_HZ = 587.33;

// Kept one bin clear of either end, so a bin and its neighbours can always
// be read, whatever the tuning offset.
int bin(double hz)
{
    return std::min(std::max((int)std::lround(hz / BIN_HZ), 1), FFT_SIZE / 2 - 2);
}

} // namespace

std::vector<float> openingChord(const ModemSettings& settings)
{
    const double chord[2] = {CHORD_LOW_HZ + settings.tuningOffsetHz, CHORD_HIGH_HZ + settings.tuningOffsetHz};
    const size_t length = (size_t)WINDOW;

    // Newman's phases for two notes (0 and pi / 2), a swell and release of
    // an eighth each, and the whole scaled to peak amplitude 1, as chord().
    std::vector<double> sum(length, 0.0);
    for (int k = 0; k < 2; k++)
    {
        const double phase = PI * (double)(k * k) / 2.0;
        const double step = 2.0 * PI * chord[k] / SAMPLE_RATE_HZ;
        for (size_t n = 0; n < length; n++) sum[n] += std::sin(step * (double)n + phase);
    }
    const size_t ramp = length / 8;
    double peak = 0.0;
    for (size_t n = 0; n < length; n++)
    {
        double w = 1.0;
        if (n < ramp)
            w = 0.5 - 0.5 * std::cos(PI * (double)n / ramp);
        else if (n >= length - ramp)
            w = 0.5 - 0.5 * std::cos(PI * (double)(length - 1 - n) / ramp);
        sum[n] *= w;
        peak = std::max(peak, std::fabs(sum[n]));
    }
    std::vector<float> audio(length);
    for (size_t n = 0; n < length; n++) audio[n] = (float)(peak > 0.0 ? sum[n] / peak : 0.0);
    return audio;
}

ChordListener::ChordListener()
    : fft_(FFT_SIZE)
    , window_((size_t)WINDOW, 0.0f)
    , work_(2 * (size_t)FFT_SIZE, 0.0)
    , power_((size_t)FFT_SIZE / 2, 0.0)
{
}

void ChordListener::configure(double tuningOffsetHz, double maxOffsetHz)
{
    tuningOffsetHz_ = tuningOffsetHz;
    maxOffsetHz_ = std::max(0.0, maxOffsetHz);
    tracking_ = false;
    sounding_.store(false, std::memory_order_release);
}

void ChordListener::reset()
{
    std::fill(window_.begin(), window_.end(), 0.0f);
    fill_ = 0;
    sinceHop_ = 0;
    samples_ = 0;
    chordsHeard_ = 0;
    last_ = Heard();
    tracking_ = false;
    sounding_.store(false, std::memory_order_release);
}

void ChordListener::push(const short* samples, int numSamples)
{
    std::vector<float> scaled((size_t)std::max(numSamples, 0));
    for (int n = 0; n < numSamples; n++) scaled[(size_t)n] = samples[n] / 32768.0f;
    push(scaled.data(), numSamples);
}

void ChordListener::push(const float* samples, int numSamples)
{
    for (int n = 0; n < numSamples; n++)
    {
        samples_++;
        if (fill_ < WINDOW)
        {
            window_[(size_t)fill_++] = samples[n];
            if (fill_ < WINDOW) continue;
        }
        else
        {
            window_[(size_t)(WINDOW - HOP + sinceHop_++)] = samples[n];
            if (sinceHop_ < HOP) continue;
        }

        analyse();
        std::copy(window_.begin() + HOP, window_.end(), window_.begin());
        sinceHop_ = 0;
    }
}

double ChordListener::noiseLevel()
{
    const int lo = bin(NOISE_LOW_HZ);
    const int hi = bin(NOISE_HIGH_HZ);
    band_.assign(power_.begin() + lo, power_.begin() + hi);
    std::nth_element(band_.begin(), band_.begin() + (std::ptrdiff_t)(band_.size() / 2), band_.end());

    // A bin of noise alone has exponentially distributed power, whose median
    // is ln 2 of its mean.
    return band_[band_.size() / 2] / std::log(2.0);
}

// Every note of a scale at one tuning, each the loudest of its bin and the
// bins either side, which follows a station that drifts a little.
double ChordListener::combPower(const std::array<double, NOTES>& notes, double offsetHz) const
{
    double sum = 0.0;
    for (double hz : notes)
    {
        int k = bin(hz + offsetHz);
        sum += std::max(power_[(size_t)k], std::max(power_[(size_t)(k - 1)], power_[(size_t)(k + 1)]));
    }
    return sum;
}

void ChordListener::analyse()
{
    std::fill(work_.begin(), work_.end(), 0.0);
    for (int n = 0; n < WINDOW; n++) work_[2 * (size_t)n] = window_[(size_t)n];
    fft_.forward(work_.data());
    for (size_t k = 0; k < power_.size(); k++)
    {
        power_[k] = work_[2 * k] * work_[2 * k] + work_[2 * k + 1] * work_[2 * k + 1];
    }

    // Digital silence: nobody is on the air.
    const double noise = noiseLevel();
    if (noise <= 0.0)
    {
        tracking_ = false;
        sounding_.store(false, std::memory_order_release);
        return;
    }

    // The chord: E4 and D5, with the weaker of them over the threshold.
    // Every scale opens with the same two notes, so there is one chord to
    // look for at each tuning.
    const int steps = (int)std::floor(maxOffsetHz_ / BIN_HZ);
    Heard best;
    for (int step = -steps; step <= steps; step++)
    {
        const double offset = tuningOffsetHz_ + step * BIN_HZ;
        const double low = power_[(size_t)bin(CHORD_LOW_HZ + offset)] / noise;
        const double high = power_[(size_t)bin(CHORD_HIGH_HZ + offset)] / noise;
        const double weaker = std::min(low, high);
        if (std::max(low, high) > MAX_NOTE_RATIO * weaker) continue;
        if (weaker > best.strength)
        {
            best.offsetHz = offset;
            best.strength = weaker;
        }
    }

    if (best.strength >= CHORD_THRESHOLD)
    {
        best.endSample = samples_;
        last_ = best;
        chordsHeard_++;
        tracking_ = true;
        scaleEnergy_.fill(0.0);
        lastSoundedSample_ = samples_;
    }
    else if (tracking_)
    {
        // Every note of each scale at the chord's tuning, which the melody
        // sings one at a time. The scale singing is the one whose notes have
        // been loudest since the chord, and only it counts: the loudest of
        // four on noise alone would hold the channel for nobody. Eight bins
        // of noise, each the loudest of three, average about 15 times the
        // noise per bin.
        std::array<double, SCALE_COUNT> power{};
        size_t singing = 0;
        for (size_t s = 0; s < power.size(); s++)
        {
            power[s] = combPower(scaleNotes((Scale)s, 0), last_.offsetHz) / noise;
            scaleEnergy_[s] += power[s];
            if (scaleEnergy_[s] > scaleEnergy_[singing]) singing = s;
        }
        if (power[singing] >= SOUNDING_THRESHOLD) lastSoundedSample_ = samples_;
        if (samples_ - lastSoundedSample_ > (long long)(HOLD_SECONDS * SAMPLE_RATE_HZ)) tracking_ = false;
    }

    sounding_.store(tracking_, std::memory_order_release);
}

} // namespace Glissando
