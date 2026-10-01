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

// Windows back to the one that ends where the current one starts.
constexpr size_t WINDOWS_BEFORE = (size_t)(WINDOW / HOP);

// Windows of each scale's comb kept, for what was usual before a chord.
constexpr size_t COMB_WINDOWS = (size_t)(ChordListener::BEFORE_SECONDS * SAMPLE_RATE_HZ / HOP);
constexpr int FFT_SIZE = 8192;
constexpr double BIN_HZ = (double)SAMPLE_RATE_HZ / FFT_SIZE;

// The noise level is the median bin across the band the scales use and
// some way either side, which a few loud notes cannot move.
constexpr double NOISE_LOW_HZ = 200.0;
constexpr double NOISE_HIGH_HZ = 3000.0;

// A note sounding in a chord whose two powers differ more than this is a
// carrier with noise beside it, not a chord.
constexpr double MAX_NOTE_RATIO = 10.0;

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
    , history_(WINDOWS_BEFORE, std::vector<double>((size_t)FFT_SIZE / 2, 0.0))
    , normalized_((size_t)FFT_SIZE / 2, 0.0)
{
    configure(0.0, 25.0);
}

void ChordListener::configure(double tuningOffsetHz, double maxOffsetHz)
{
    tuningOffsetHz_ = tuningOffsetHz;
    maxOffsetHz_ = std::max(0.0, maxOffsetHz);
    tracking_ = false;
    sounding_.store(false, std::memory_order_release);

    // What was usual at the old tuning says nothing about the new one.
    steps_ = (int)std::floor(maxOffsetHz_ / BIN_HZ);
    combs_.assign(COMB_WINDOWS * SCALE_COUNT * (size_t)(2 * steps_ + 1), 0.0f);
    combsNext_ = 0;
    combsCount_ = 0;
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
    historyNext_ = 0;
    historyCount_ = 0;
    combsNext_ = 0;
    combsCount_ = 0;
}

void ChordListener::heardFrame()
{
    if (tracking_) chordStartSample_ = samples_;
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
double ChordListener::combPower(const std::vector<double>& power, const std::array<double, NOTES>& notes,
                                double offsetHz)
{
    double sum = 0.0;
    for (double hz : notes)
    {
        int k = bin(hz + offsetHz);
        sum += std::max(power[(size_t)k], std::max(power[(size_t)(k - 1)], power[(size_t)(k + 1)]));
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
    const std::vector<double>& before = history_[historyNext_];
    const bool beforeKnown = historyCount_ >= WINDOWS_BEFORE;
    if (noise <= 0.0)
    {
        tracking_ = false;
        sounding_.store(false, std::memory_order_release);
        remember(true);
        rememberCombs(true);
        return;
    }
    for (size_t k = 0; k < power_.size(); k++) normalized_[k] = power_[k] / noise;

    // The chord: E4 and D5, with the weaker of them over the threshold.
    // Every scale opens with the same two notes, so there is one chord to
    // look for at each tuning. Both notes have to be new, as they are when
    // a station keys: the window before this one is all before the chord.
    // On a busy band signals that were already there, a carrier, RTTY or
    // the tones of FT8 wandering across the two notes, otherwise made a
    // chord ten times a second and held the channel for ever.
    const int steps = steps_;
    Heard best;
    int bestStep = 0;
    for (int step = -steps; step <= steps; step++)
    {
        const double offset = tuningOffsetHz_ + step * BIN_HZ;
        const size_t lowBin = (size_t)bin(CHORD_LOW_HZ + offset);
        const size_t highBin = (size_t)bin(CHORD_HIGH_HZ + offset);
        const double low = normalized_[lowBin];
        const double high = normalized_[highBin];
        const double weaker = std::min(low, high);
        if (weaker < CHORD_THRESHOLD || weaker <= best.strength) continue;
        if (std::max(low, high) > MAX_NOTE_RATIO * weaker) continue;
        if (beforeKnown && std::max(before[lowBin], before[highBin]) > ONSET_RATIO * weaker) continue;
        if (std::min(purity(lowBin), purity(highBin)) < PURITY) continue;
        if (!steady(lowBin) || !steady(highBin)) continue;

        best.offsetHz = offset;
        best.strength = weaker;
        bestStep = step;
    }

    if (best.strength >= CHORD_THRESHOLD)
    {
        best.endSample = samples_;
        last_ = best;
        chordsHeard_++;
        lastSoundedSample_ = samples_;

        // A chord heard again in the next few windows is the same chord;
        // what was there before it stays as first measured.
        if (!tracking_)
        {
            tracking_ = true;
            chordStartSample_ = samples_;
            scaleEnergy_.fill(0.0);
            for (size_t s = 0; s < before_.size(); s++) before_[s] = usualComb(s, bestStep);
        }
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
            power[s] = combPower(normalized_, scaleNotes((Scale)s, 0), last_.offsetHz);
            scaleEnergy_[s] += power[s];
            if (scaleEnergy_[s] > scaleEnergy_[singing]) singing = s;
        }

        // Only notes over what was on the channel before the chord count:
        // a signal already sitting on them is not the melody.
        double needed = std::max(SOUNDING_THRESHOLD, OVER_BEFORE * before_[singing]);
        if (power[singing] >= needed) lastSoundedSample_ = samples_;
        if (samples_ - lastSoundedSample_ > (long long)(HOLD_SECONDS * SAMPLE_RATE_HZ)) tracking_ = false;
    }

    if (tracking_ && samples_ - chordStartSample_ > (long long)(MAX_TRACK_SECONDS * SAMPLE_RATE_HZ))
    {
        tracking_ = false;
    }

    sounding_.store(tracking_, std::memory_order_release);
    remember(false);
    rememberCombs(false);
}

// Whether a note sounds in both halves of the window, as the chord's do. A
// tone held the whole window puts a quarter of its power in each half; two
// signals that happen to sit on the chord's notes for part of it do not.
bool ChordListener::steady(size_t k) const
{
    const double w = 2.0 * PI * (double)k / FFT_SIZE;
    const double full = power_[k];
    const int half = WINDOW / 2;
    for (int start : {0, half})
    {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < half; n++)
        {
            const double x = window_[(size_t)(start + n)];
            re += x * std::cos(w * (start + n));
            im -= x * std::sin(w * (start + n));
        }
        if (re * re + im * im < STEADY_SHARE * full / 4.0) return false;
    }
    return true;
}

// Every scale's comb at every tuning step of this window, for what is usual
// on the channel when a chord comes.
void ChordListener::rememberCombs(bool silence)
{
    const size_t perWindow = SCALE_COUNT * (size_t)(2 * steps_ + 1);
    float* slot = &combs_[combsNext_ * perWindow];
    for (size_t s = 0; s < SCALE_COUNT; s++)
    {
        const std::array<double, NOTES> notes = scaleNotes((Scale)s, 0);
        for (int step = -steps_; step <= steps_; step++)
        {
            slot[s * (size_t)(2 * steps_ + 1) + (size_t)(step + steps_)] =
                silence ? 0.0f : (float)combPower(normalized_, notes, tuningOffsetHz_ + step * BIN_HZ);
        }
    }
    combsNext_ = (combsNext_ + 1) % COMB_WINDOWS;
    combsCount_++;
}

// The 90th percentile of a scale's comb at one tuning step over the
// windows kept, leaving out the last window's worth, which may hold the
// chord. 0 with too little kept to say.
double ChordListener::usualComb(size_t scale, int step)
{
    const size_t kept = std::min(combsCount_, COMB_WINDOWS);
    if (kept <= WINDOWS_BEFORE) return 0.0;

    const size_t perWindow = SCALE_COUNT * (size_t)(2 * steps_ + 1);
    const size_t index = scale * (size_t)(2 * steps_ + 1) + (size_t)(step + steps_);
    sorted_.clear();
    for (size_t back = WINDOWS_BEFORE + 1; back <= kept; back++)
    {
        size_t window = (combsNext_ + COMB_WINDOWS - back) % COMB_WINDOWS;
        sorted_.push_back(combs_[window * perWindow + index]);
    }
    size_t at = std::min(sorted_.size() - 1, (size_t)(BEFORE_PERCENTILE * (double)sorted_.size()));
    std::nth_element(sorted_.begin(), sorted_.begin() + (std::ptrdiff_t)at, sorted_.end());
    return sorted_[at];
}

// A note's power over the average a few hertz either side of it. The
// chord's notes are steady for the whole window, so their power sits in a
// line a couple of bins wide; a signal that wanders, the tones of FT8 or a
// voice, spreads its own across the bins around it.
double ChordListener::purity(size_t k) const
{
    const int from = (int)std::lround(PURITY_FROM_HZ / BIN_HZ);
    const int to = (int)std::lround(PURITY_TO_HZ / BIN_HZ);
    double sum = 0.0;
    int count = 0;
    for (int d = from; d <= to; d++)
    {
        for (int side : {-1, 1})
        {
            long long j = (long long)k + side * d;
            if (j < 0 || j >= (long long)normalized_.size()) continue;
            sum += normalized_[(size_t)j];
            count++;
        }
    }
    double around = count > 0 ? sum / count : 0.0;
    return around > 0.0 ? normalized_[k] / around : 0.0;
}

// Keeps this window's spectrum, over the noise, for the chord a window from
// now.
void ChordListener::remember(bool silence)
{
    std::vector<double>& slot = history_[historyNext_];
    if (silence)
        std::fill(slot.begin(), slot.end(), 0.0);
    else
        slot = normalized_;
    historyNext_ = (historyNext_ + 1) % history_.size();
    historyCount_++;
}

} // namespace Glissando
