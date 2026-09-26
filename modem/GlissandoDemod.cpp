//=========================================================================
// Name:            GlissandoDemod.cpp
// Purpose:         The Glissando receive chain (prototype/glissando.py):
//                  analytic signal, sync search, sync refinement, soft
//                  demodulation with BCJR over the note trellis, channel
//                  report and decision directed channel sounding.
//
// The prototype is written for NumPy; this port keeps its arithmetic but
// restructures the heavy correlations so a desktop core decodes every gear
// in a small fraction of real time:
//
// - the coarse sync search dechirps each sync symbol and integrates it in
//   short blocks before the FFT, so the FFT is a few hundred points long
//   rather than 4L (see SyncSearch);
// - every glide template is a glide followed by a sustain on the target
//   note, so the 64 (from, to) correlations of a symbol share 8 sustain
//   correlations (see VoiceTemplates in GlissandoInternal.h);
// - frequency offsets are removed from the templates, not the audio, where
//   only the power of a correlation matters.
//
// It also adds what a receiver listening to a live sound card needs and the
// prototype's simulations never did: floors on the sync score and the
// measured Es/N0, and a hard limit on the frequency offset, so digital
// silence and hum decode nothing (see MIN_SYNC_SCORE).
//=========================================================================

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>

#include "GlissandoFft.h"
#include "GlissandoInternal.h"

namespace Glissando
{
namespace detail
{

namespace
{

constexpr double PI = 3.14159265358979323846;
constexpr double LN2 = 0.69314718055994530942;
constexpr double NEG_INF = -std::numeric_limits<double>::infinity();

// Floors that keep silence and hum from decoding. The prototype has
// neither: its searches always hold a signal or noise.
//
// Sync peaks are scored against the median score of the search (DESIGN
// 3.4). Measured on streaming-sized searches: the best peak in noise alone
// scores 1.8 to 2.5, frames that decode 1 dB below the 50 % threshold 2.0
// to 3.7. The two overlap, so the floor sits under both and only rejects
// input with no noise in it: silence (0 / 0) and constant input, which
// scores the same at every start.
constexpr double MIN_SYNC_SCORE = 1.5;

// Per-symbol Es/N0 measured on the sync symbols. At every gear's 50 %
// threshold it is about 3.6 (5.6 dB; -26.5 dB in 2500 Hz at 0.64 s symbols
// is 3.6), and frames decoded 1 dB below that measure 1.7 or more. Below
// 1 (0 dB) nothing decodes, so a CRC passing there is luck: a noise peak
// (half of them measure below 1) or silence.
constexpr double MIN_ES_OVER_N0 = 1.0;

// ---------------------------------------------------------------- helpers

// sum a[n] * conj(b[n]). Eight independent partial sums let the compiler
// use SIMD without being allowed to reorder floating point additions.
void dotConj(const float* ar, const float* ai, const float* br, const float* bi, int n, double& outRe,
             double& outIm)
{
    constexpr int LANES = 8;
    float accRe[LANES] = {};
    float accIm[LANES] = {};
    int i = 0;
    for (; i + LANES <= n; i += LANES)
    {
        for (int j = 0; j < LANES; j++)
        {
            accRe[j] += ar[i + j] * br[i + j] + ai[i + j] * bi[i + j];
            accIm[j] += ai[i + j] * br[i + j] - ar[i + j] * bi[i + j];
        }
    }
    float re = 0.0f;
    float im = 0.0f;
    for (; i < n; i++)
    {
        re += ar[i] * br[i] + ai[i] * bi[i];
        im += ai[i] * br[i] - ar[i] * bi[i];
    }
    for (int j = 0; j < LANES; j++)
    {
        re += accRe[j];
        im += accIm[j];
    }
    outRe = re;
    outIm = im;
}

double power(double re, double im)
{
    return re * re + im * im;
}

// exp(i 2 pi f n / FS) for n = 0..count-1, by recurrence, re-seeded every
// 256 samples so rounding never builds up.
template <typename Fn>
void forEachPhasor(double freqHz, size_t count, Fn fn)
{
    const size_t RESEED = 256;
    double stepAngle = 2.0 * PI * freqHz / SAMPLE_RATE_HZ;
    double stepRe = std::cos(stepAngle);
    double stepIm = std::sin(stepAngle);
    double re = 1.0;
    double im = 0.0;
    for (size_t n = 0; n < count; n++)
    {
        if (n % RESEED == 0)
        {
            // Reduce the cycle count before the multiply by 2 pi, which keeps
            // the angle accurate however long the buffer.
            double cycles = std::fmod(freqHz * (double)n / SAMPLE_RATE_HZ, 1.0);
            re = std::cos(2.0 * PI * cycles);
            im = std::sin(2.0 * PI * cycles);
        }
        fn(n, re, im);
        double nextRe = re * stepRe - im * stepIm;
        im = re * stepIm + im * stepRe;
        re = nextRe;
    }
}

// out[n] = z[start + n] * exp(-i 2 pi df n / FS): the audio with a
// frequency offset removed. The phase reference is the segment's first
// sample rather than the buffer's (as in the prototype); nothing downstream
// depends on the absolute phase.
void derotate(const ComplexSignal& z, long long start, size_t count, double dfHz, ComplexSignal& out)
{
    out.resize(count);
    const float* zr = z.re.data() + start;
    const float* zi = z.im.data() + start;
    forEachPhasor(-dfHz, count, [&](size_t n, double pr, double pi) {
        out.re[n] = (float)(zr[n] * pr - zi[n] * pi);
        out.im[n] = (float)(zr[n] * pi + zi[n] * pr);
    });
}

// NumPy's median: the mean of the two middle values for an even count.
double median(std::vector<double>& v)
{
    if (v.empty()) return 0.0;
    size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (long)mid, v.end());
    double upper = v[mid];
    if (v.size() % 2 == 1) return upper;
    double lower = *std::max_element(v.begin(), v.begin() + (long)mid);
    return 0.5 * (lower + upper);
}

// log I0(x) for x >= 0 (prototype _logi0()). Below 30 it uses the
// Abramowitz and Stegun 9.8.1 / 9.8.2 polynomials (relative error under
// 2e-7) instead of NumPy's i0; from 30 up the same asymptotic form as the
// prototype.
double logI0(double x)
{
    if (x < 3.75)
    {
        double t = (x / 3.75) * (x / 3.75);
        return std::log(1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 +
                        t * (0.2659732 + t * (0.0360768 + t * 0.0045813))))));
    }
    if (x < 30.0)
    {
        double t = 3.75 / x;
        double poly = 0.39894228 + t * (0.01328592 + t * (0.00225319 + t * (-0.00157565 + t * (0.00916281 +
                      t * (-0.02057706 + t * (0.02635537 + t * (-0.01647633 + t * 0.00392377)))))));
        return x - 0.5 * std::log(x) + std::log(poly);
    }
    return x - 0.5 * std::log(2.0 * PI * x);
}

double logSumExp(const double* v, int n)
{
    double m = NEG_INF;
    for (int i = 0; i < n; i++) m = std::max(m, v[i]);
    if (m == NEG_INF) return NEG_INF;
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += std::exp(v[i] - m);
    return m + std::log(sum);
}

std::shared_ptr<const Fft> fftOfSize(size_t size)
{
    static std::mutex mutex;
    static std::map<size_t, std::shared_ptr<const Fft>> plans;
    std::lock_guard<std::mutex> lock(mutex);
    auto& plan = plans[size];
    if (!plan) plan = std::make_shared<const Fft>(size);
    return plan;
}

// ---------------------------------------------------------------- templates

std::shared_ptr<const VoiceTemplates> buildTemplates(const std::array<double, NOTES>& freqs, const GearInfo& gear)
{
    auto v = std::make_shared<VoiceTemplates>();
    const int L = gear.samplesPerSymbol();
    const int n0 = std::min(L, (int)std::ceil(gear.glide * L));
    const int holdLength = L - n0;
    v->samplesPerSymbol = L;
    v->glideSamples = n0;
    v->holdSamples = holdLength;
    v->notes = freqs;

    // Phase of every template, in double, as the prototype's _template():
    // phase[0] = 0 and phase[n] = 2 pi sum(f[0..n-1]) / FS.
    std::vector<double> phase((size_t)NOTES * NOTES * L);
    std::vector<double> f((size_t)L);
    for (int a = 0; a < NOTES; a++)
    {
        for (int b = 0; b < NOTES; b++)
        {
            pitchTrack(freqs[a], freqs[b], L, gear.glide, f.data());
            double* ph = phase.data() + ((size_t)a * NOTES + b) * L;
            double sum = 0.0;
            ph[0] = 0.0;
            for (int n = 0; n < L; n++)
            {
                sum += f[n];
                if (n + 1 < L) ph[n + 1] = 2.0 * PI * sum / SAMPLE_RATE_HZ;
            }
            v->advance[a][b] = 2.0 * PI * sum / SAMPLE_RATE_HZ;
        }
    }
    auto phaseOf = [&](int a, int b, int n) { return phase[((size_t)a * NOTES + b) * L + n]; };

    v->glideRe.resize((size_t)NOTES * NOTES * n0);
    v->glideIm.resize((size_t)NOTES * NOTES * n0);
    for (int a = 0; a < NOTES; a++)
    {
        for (int b = 0; b < NOTES; b++)
        {
            size_t base = ((size_t)a * NOTES + b) * n0;
            for (int n = 0; n < n0; n++)
            {
                v->glideRe[base + n] = (float)std::cos(phaseOf(a, b, n));
                v->glideIm[base + n] = (float)std::sin(phaseOf(a, b, n));
            }
            // From n0 on t_ab and the sustain of b differ by a constant phase.
            double offset = n0 < L ? phaseOf(a, b, n0) - phaseOf(b, b, n0) : 0.0;
            v->rotation[a][b] = std::complex<double>(std::cos(offset), std::sin(offset));
        }
    }

    // Sustain templates: the prototype's Voice.hold, which is t_bb with the
    // glide part zeroed. Only the non-zero part is stored.
    v->holdRe.resize((size_t)NOTES * holdLength);
    v->holdIm.resize((size_t)NOTES * holdLength);
    for (int b = 0; b < NOTES; b++)
    {
        for (int n = 0; n < holdLength; n++)
        {
            v->holdRe[(size_t)b * holdLength + n] = (float)std::cos(phaseOf(b, b, n0 + n));
            v->holdIm[(size_t)b * holdLength + n] = (float)std::sin(phaseOf(b, b, n0 + n));
        }
    }

    // Sync templates (prototype _sync_templates()). The first note of the
    // first motif is preceded by itself (the transmission starts on it); the
    // first notes of the other motifs follow data, so only their sustain is
    // known.
    v->syncRe.assign((size_t)SYNC_SYMBOLS * L, 0.0f);
    v->syncIm.assign((size_t)SYNC_SYMBOLS * L, 0.0f);
    for (int k = 0; k < SYNC_SYMBOLS; k++)
    {
        int j = k % MOTIF_LENGTH;
        int note = COSTAS7[j];
        int from = j != 0 ? COSTAS7[j - 1] : note;
        int first = (j == 0 && syncPosition(k) != 0) ? n0 : 0;
        for (int n = first; n < L; n++)
        {
            v->syncRe[(size_t)k * L + n] = (float)std::cos(phaseOf(from, note, n));
            v->syncIm[(size_t)k * L + n] = (float)std::sin(phaseOf(from, note, n));
        }
    }

    // Sustain templates at each trial offset of refineSync()'s frequency
    // search: steps of 1/16 of a symbol's frequency resolution, +/- 6 steps.
    v->freqStepHz = 0.0625 / gear.symbolSeconds;
    v->holdShiftRe.resize((size_t)VoiceTemplates::FREQ_STEPS * NOTES * holdLength);
    v->holdShiftIm.resize(v->holdShiftRe.size());
    for (int i = 0; i < VoiceTemplates::FREQ_STEPS; i++)
    {
        double shift = (i - VoiceTemplates::FREQ_STEPS / 2) * v->freqStepHz;
        for (int b = 0; b < NOTES; b++)
        {
            size_t base = ((size_t)i * NOTES + b) * holdLength;
            for (int n = 0; n < holdLength; n++)
            {
                double angle = phaseOf(b, b, n0 + n) + 2.0 * PI * shift * (n0 + n) / SAMPLE_RATE_HZ;
                v->holdShiftRe[base + n] = (float)std::cos(angle);
                v->holdShiftIm[base + n] = (float)std::sin(angle);
            }
        }
    }
    return v;
}

// ---------------------------------------------------------------- sync search

struct SyncCandidate
{
    long long start = 0;    // sample index of symbol 0
    double df = 0.0;        // frequency offset, Hz
    double score = 0.0;
};

// Scores sync hypotheses (prototype sync_search()). Each sync symbol is
// multiplied by the conjugate of its known glide ("dechirping"): a correctly
// timed symbol collapses to a tone at the frequency offset, so one
// zero-padded FFT scores every offset at once, and powers are summed
// non-coherently over the 21 sync symbols.
//
// The prototype takes a 4L point FFT of each dechirped symbol and keeps only
// the bins within +/- maxOffsetHz. Here the dechirped symbol is first summed
// in blocks of D samples, which decimates it to FS / D while it is still at
// the (small) frequency offset, and the FFT is 4L / D points (rounded up to a
// power of two) on the same bin spacing. D is the largest power of two with
// maxOffsetHz * D / FS <= 0.1, so a signal at the edge of the search range
// loses at most 1 - sinc^2(0.1) = 0.14 dB of score; the noise per bin is
// unchanged.
class SyncSearch
{
public:
    SyncSearch(const ComplexSignal& z, const VoiceTemplates& voice, double maxOffsetHz)
        : z_(z)
        , voice_(voice)
        , L_(voice.samplesPerSymbol)
    {
        maxOffsetHz = std::max(maxOffsetHz, 1.0);
        block_ = 1;
        while (block_ < 64 && block_ * 2 <= L_ / 4 && maxOffsetHz * (block_ * 2) / SAMPLE_RATE_HZ <= 0.1) block_ *= 2;
        blocks_ = (L_ + block_ - 1) / block_;
        size_t nfft = nextPowerOfTwo((size_t)(4 * L_));
        points_ = std::max<size_t>(nfft / (size_t)block_, nextPowerOfTwo((size_t)blocks_));
        fft_ = fftOfSize(points_);
        binHz_ = (double)SAMPLE_RATE_HZ / ((double)block_ * (double)points_);

        // Bins within the search range, lowest frequency first (the
        // prototype keeps them in FFT order, which leaves the 0 Hz bin
        // without neighbours for the peak interpolation).
        int half = (int)std::floor(maxOffsetHz / binHz_);
        half = std::min(half, (int)points_ / 2 - 1);
        for (int q = -half; q <= half; q++)
        {
            bins_.push_back(q >= 0 ? (size_t)q : points_ + (size_t)q);
            freqs_.push_back(q * binHz_);
        }
        buffer_.resize(2 * points_);
    }

    size_t numBins() const { return bins_.size(); }
    double binFrequency(size_t j) const { return freqs_[j]; }
    double binHz() const { return binHz_; }

    // Adds the sync power at every kept bin for a frame starting at `start`.
    void score(long long start, double* out)
    {
        std::fill(out, out + bins_.size(), 0.0);
        for (int k = 0; k < SYNC_SYMBOLS; k++)
        {
            size_t base = (size_t)start + (size_t)syncPosition(k) * L_;
            const float* tr = voice_.syncRe.data() + (size_t)k * L_;
            const float* ti = voice_.syncIm.data() + (size_t)k * L_;
            std::fill(buffer_.begin(), buffer_.end(), 0.0);
            for (int m = 0; m < blocks_; m++)
            {
                int n = m * block_;
                int count = std::min(block_, L_ - n);
                dotConj(z_.re.data() + base + n, z_.im.data() + base + n, tr + n, ti + n, count, buffer_[2 * m],
                        buffer_[2 * m + 1]);
            }
            fft_->forward(buffer_.data());
            for (size_t j = 0; j < bins_.size(); j++)
            {
                out[j] += power(buffer_[2 * bins_[j]], buffer_[2 * bins_[j] + 1]);
            }
        }
    }

private:
    const ComplexSignal& z_;
    const VoiceTemplates& voice_;
    int L_;
    int block_;
    int blocks_;
    size_t points_;
    double binHz_;
    std::shared_ptr<const Fft> fft_;
    std::vector<size_t> bins_;
    std::vector<double> freqs_;
    std::vector<double> buffer_;
};

// Coarse-to-fine search over start time and frequency offset (prototype
// sync_search()): starts every L/8 samples, the best `top` well separated
// peaks, then each re-searched every L/256 samples around its start with a
// parabolic fit to the frequency peak.
std::vector<SyncCandidate> syncSearch(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice,
                                      long long t0, long long t1, double maxOffsetHz, int top)
{
    std::vector<SyncCandidate> out;
    const int L = voice.samplesPerSymbol;
    const long long frame = (long long)SYMBOLS_PER_FRAME * L;
    const long long step = std::max(1, L / 8);
    t0 = std::max(0LL, t0);
    t1 = std::min(t1, (long long)z.size() - frame);
    if (t1 <= t0 || top <= 0) return out;

    SyncSearch search(z, voice, maxOffsetHz);
    const size_t bins = search.numBins();
    std::vector<long long> starts;
    for (long long s = t0; s < t1; s += step) starts.push_back(s);

    std::vector<double> S(starts.size() * bins);
    for (size_t i = 0; i < starts.size(); i++) search.score(starts[i], S.data() + i * bins);

    // Noise-normalise by the typical (median) score across the search space.
    // Silence has no typical score, and nothing to find.
    std::vector<double> sorted = S;
    double typical = median(sorted);
    if (!(typical > 0.0) || !std::isfinite(typical)) return out;
    for (double& s : S) s /= typical;

    // Best peaks first, skipping any within a symbol and 4 bins of symbol
    // resolution of one already taken.
    std::vector<size_t> order(S.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return S[a] > S[b]; });
    std::vector<SyncCandidate> coarse;
    for (size_t idx : order)
    {
        long long s = starts[idx / bins];
        double f = search.binFrequency(idx % bins);
        bool near = false;
        for (const SyncCandidate& c : coarse)
        {
            if (std::llabs(s - c.start) < L && std::fabs(f - c.df) < 4.0 / gear.symbolSeconds) near = true;
        }
        if (near) continue;
        if (!(S[idx] >= MIN_SYNC_SCORE)) break; // the rest are weaker still
        coarse.push_back({s, f, S[idx]});
        if ((int)coarse.size() >= top) break;
    }

    const long long fineStep = std::max(1, L / 256);
    std::vector<double> row(bins);
    for (const SyncCandidate& c : coarse)
    {
        double best = -1.0;
        long long bestStart = c.start;
        size_t bestBin = 0;
        std::vector<double> bestRow(bins);
        for (long long s = std::max(0LL, c.start - step); s < std::min(t1, c.start + step + 1); s += fineStep)
        {
            search.score(s, row.data());
            for (size_t j = 0; j < bins; j++)
            {
                if (row[j] > best)
                {
                    best = row[j];
                    bestStart = s;
                    bestBin = j;
                    bestRow = row;
                }
            }
        }
        double df = search.binFrequency(bestBin);
        if (bestBin > 0 && bestBin + 1 < bins)
        {
            // Parabolic peak interpolation on the log power.
            double a = std::log(bestRow[bestBin - 1] + 1e-30);
            double b = std::log(bestRow[bestBin] + 1e-30);
            double cc = std::log(bestRow[bestBin + 1] + 1e-30);
            double denominator = a - 2 * b + cc;
            if (denominator != 0.0) df += 0.5 * (a - cc) / denominator * search.binHz();
        }
        out.push_back({bestStart, df, c.score});
    }
    return out;
}

// ---------------------------------------------------------------- refinement

// Sum over the sync symbols of |correlation|^2 for a frame at `start`, with
// the sync templates already shifted to the frequency offset.
double syncEnergy(const ComplexSignal& z, const ComplexSignal& shifted, int L, long long start)
{
    double e = 0.0;
    for (int k = 0; k < SYNC_SYMBOLS; k++)
    {
        size_t base = (size_t)start + (size_t)syncPosition(k) * L;
        double re, im;
        dotConj(z.re.data() + base, z.im.data() + base, shifted.re.data() + (size_t)k * L,
                shifted.im.data() + (size_t)k * L, L, re, im);
        e += power(re, im);
    }
    return e;
}

// Sync templates times exp(i 2 pi df n / FS): correlating against them is
// correlating the audio with the offset removed.
void shiftSyncTemplates(const VoiceTemplates& voice, double df, ComplexSignal& out)
{
    const int L = voice.samplesPerSymbol;
    out.resize((size_t)SYNC_SYMBOLS * L);
    std::vector<double> pr((size_t)L), pi((size_t)L);
    forEachPhasor(df, (size_t)L, [&](size_t n, double re, double im) {
        pr[n] = re;
        pi[n] = im;
    });
    for (int k = 0; k < SYNC_SYMBOLS; k++)
    {
        for (int n = 0; n < L; n++)
        {
            size_t i = (size_t)k * L + n;
            double tr = voice.syncRe[i];
            double ti = voice.syncIm[i];
            out.re[i] = (float)(tr * pr[n] - ti * pi[n]);
            out.im[i] = (float)(tr * pi[n] + ti * pr[n]);
        }
    }
}

// Polish time and frequency after the coarse search (prototype
// refine_sync()). A glide is a chirp, and a chirp compresses in time: a
// glide sweeping a few hundred Hz has a timing resolution of a few
// milliseconds, and at G1 a 1.25 ms timing error already costs 1.5 dB. So
// time is refined to the sample on the known sync glides. Frequency is
// refined on the sustained part of every note over all 86 symbols, which
// has no time-frequency coupling.
SyncCandidate refineSync(const ComplexSignal& z, const VoiceTemplates& voice, const SyncCandidate& sync)
{
    const int L = voice.samplesPerSymbol;
    const long long frame = (long long)SYMBOLS_PER_FRAME * L;
    const long long lo = 0;
    const long long hi = (long long)z.size() - frame;
    ComplexSignal shifted;

    auto bestTime = [&](long long start, long long half, long long step) {
        long long best = start;
        double bestEnergy = -1.0;
        for (long long t = start - half; t <= start + half; t += step)
        {
            if (t < lo || t > hi) continue;
            double e = syncEnergy(z, shifted, L, t);
            if (e > bestEnergy)
            {
                bestEnergy = e;
                best = t;
            }
        }
        return best;
    };

    shiftSyncTemplates(voice, sync.df, shifted);
    long long start = bestTime(sync.start, std::max(1, L / 256) + 2, std::max(1, L / 512));
    start = bestTime(start, std::max(1, L / 512), 1);

    // Frequency: the sustain energy of the best note of every symbol, at
    // offsets of 1/16 of the symbol rate either side of the coarse estimate.
    ComplexSignal segment;
    derotate(z, start, (size_t)frame, sync.df, segment);
    const int n0 = voice.glideSamples;
    const int holdLength = voice.holdSamples;
    double bestEnergy = -1.0;
    double df = sync.df;
    for (int i = 0; i < VoiceTemplates::FREQ_STEPS; i++)
    {
        double e = 0.0;
        for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
        {
            size_t base = (size_t)k * L + n0;
            double symbolBest = 0.0;
            for (int b = 0; b < NOTES; b++)
            {
                size_t t = ((size_t)i * NOTES + b) * holdLength;
                double re, im;
                dotConj(segment.re.data() + base, segment.im.data() + base, voice.holdShiftRe.data() + t,
                        voice.holdShiftIm.data() + t, holdLength, re, im);
                symbolBest = std::max(symbolBest, power(re, im));
            }
            e += symbolBest;
        }
        if (e > bestEnergy)
        {
            bestEnergy = e;
            df = sync.df + (i - VoiceTemplates::FREQ_STEPS / 2) * voice.freqStepHz;
        }
    }

    shiftSyncTemplates(voice, df, shifted);
    start = bestTime(start, 4, 1);
    return {start, df, sync.score};
}

// ---------------------------------------------------------------- demodulation

// Correlations of one frame against every glide.
struct Correlations
{
    std::complex<double> y[SYMBOLS_PER_FRAME][NOTES][NOTES];   // <r_k, t_ab>
    std::complex<double> hold[SYMBOLS_PER_FRAME][NOTES];       // <r_k, hold_b>
};

void correlate(const ComplexSignal& z, const VoiceTemplates& voice, const SyncCandidate& sync, Correlations& c)
{
    const int L = voice.samplesPerSymbol;
    const int n0 = voice.glideSamples;
    const int holdLength = voice.holdSamples;
    ComplexSignal segment;
    derotate(z, sync.start, (size_t)SYMBOLS_PER_FRAME * L, sync.df, segment);

    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
    {
        const float* sr = segment.re.data() + (size_t)k * L;
        const float* si = segment.im.data() + (size_t)k * L;
        for (int b = 0; b < NOTES; b++)
        {
            double re, im;
            dotConj(sr + n0, si + n0, voice.holdRe.data() + (size_t)b * holdLength,
                    voice.holdIm.data() + (size_t)b * holdLength, holdLength, re, im);
            c.hold[k][b] = std::complex<double>(re, im);
        }
        for (int a = 0; a < NOTES; a++)
        {
            for (int b = 0; b < NOTES; b++)
            {
                size_t t = ((size_t)a * NOTES + b) * n0;
                double re, im;
                dotConj(sr, si, voice.glideRe.data() + t, voice.glideIm.data() + t, n0, re, im);
                c.y[k][a][b] = std::complex<double>(re, im) + std::conj(voice.rotation[a][b]) * c.hold[k][b];
            }
        }
    }
}

// SNR and Doppler spread measured on the three signature motifs (prototype
// channel_report()). Within a motif the waveform is phase continuous and
// every note is known, so after removing the known phase advance of each
// glide, successive symbol correlations should have the same phase on a
// static channel. Their normalised correlation at lag T is
// exp(-2 pi^2 sigma^2 T^2) for a Gaussian Doppler spectrum; the Doppler
// spread is 2 sigma.
ChannelReport channelReport(const std::complex<double>* syncC, double es, double noise, const GearInfo& gear,
                            const VoiceTemplates& voice)
{
    ChannelReport report;
    const int L = voice.samplesPerSymbol;
    // SNR in 2500 Hz: per-sample signal power over per-sample noise power
    // (analytic noise is white over FS), rescaled to 2500 Hz.
    report.snrDb = 10.0 * std::log10(es / noise * SAMPLE_RATE_HZ / (L * 2500.0));

    // Symbols 1..6 of each motif: their predecessors are known notes, so
    // their correlations carry phase. Symbol k glides COSTAS7[k-1] -> COSTAS7[k].
    std::complex<double> num = 0.0;
    double den = 0.0;
    int lags = 0;
    for (int m = 0; m < 3; m++)
    {
        std::complex<double> c[MOTIF_LENGTH - 1];
        for (int i = 0; i < MOTIF_LENGTH - 1; i++) c[i] = syncC[MOTIF_LENGTH * m + 1 + i];
        for (int k = 1; k < MOTIF_LENGTH - 1; k++)
        {
            // Undo the known phase ramp of the earlier symbol's glide.
            std::complex<double> undo = std::polar(1.0, -voice.advance[COSTAS7[k - 1]][COSTAS7[k]]);
            for (int i = k; i < MOTIF_LENGTH - 1; i++) c[i] *= undo;
        }
        for (int i = 1; i < MOTIF_LENGTH - 1; i++)
        {
            num += c[i] * std::conj(c[i - 1]);
            den += std::norm(c[i - 1]);
        }
        lags += MOTIF_LENGTH - 2;
    }
    double rho = std::abs(num) / std::max(den - lags * noise, 1e-9);
    rho = std::min(std::max(rho, 1e-6), 1.0);
    double sigma = rho < 1.0 ? std::sqrt(-std::log(rho) / (2.0 * PI * PI * gear.symbolSeconds * gear.symbolSeconds))
                             : 0.0;
    report.dopplerHz = 2.0 * sigma;
    report.coherence = rho;
    return report;
}

// Soft demodulation (prototype demod_voice()): bit LLRs and a channel report.
// Returns the signal to noise ratio per symbol, Es/N0, measured on the sync
// symbols; NaN when the audio holds no usable signal at all (silence).
double demodulate(const Correlations& c, const GearInfo& gear, const VoiceTemplates& voice, FrameLlrs& llrs,
                  ChannelReport& report)
{
    std::vector<double> powers((size_t)SYMBOLS_PER_FRAME * NOTES * NOTES);
    auto P = [&](int k, int a, int b) -> double& { return powers[((size_t)k * NOTES + a) * NOTES + b]; };
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
        for (int a = 0; a < NOTES; a++)
            for (int b = 0; b < NOTES; b++) P(k, a, b) = std::norm(c.y[k][a][b]);

    // Noise level: the median of |y|^2 over wrong-target templates is about
    // ln 2 times the noise (the median of an exponential variable). Leave out
    // each symbol's most likely target note; what is left is mostly noise.
    std::vector<double> rest;
    rest.reserve((size_t)SYMBOLS_PER_FRAME * NOTES * (NOTES - 1));
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
    {
        int bestNote = 0;
        double bestPower = -1.0;
        for (int b = 0; b < NOTES; b++)
        {
            for (int a = 0; a < NOTES; a++)
            {
                if (P(k, a, b) > bestPower)
                {
                    bestPower = P(k, a, b);
                    bestNote = b;
                }
            }
        }
        for (int a = 0; a < NOTES; a++)
            for (int b = 0; b < NOTES; b++)
                if (b != bestNote) rest.push_back(P(k, a, b));
    }
    double noise = median(rest) / LN2;
    // Digital silence correlates to exactly nothing; there is no signal to
    // measure and no LLR worth decoding (zero LLRs decode to the all-zero
    // word, whose CRC is zero too).
    if (!(noise > 0.0) || !std::isfinite(noise)) return NAN;

    // Signal amplitude from the sync symbols, whose targets are known. The
    // first note of the second and third motifs follows data, so its glide
    // is unknown: take the strongest.
    std::complex<double> syncC[SYNC_SYMBOLS];
    double sum = 0.0;
    for (int k = 0; k < SYNC_SYMBOLS; k++)
    {
        int pos = syncPosition(k);
        int j = k % MOTIF_LENGTH;
        int b = COSTAS7[j];
        if (pos == 0)
            syncC[k] = c.y[pos][b][b];
        else if (j != 0)
            syncC[k] = c.y[pos][COSTAS7[j - 1]][b];
        else
        {
            double m = 0.0;
            for (int a = 0; a < NOTES; a++) m = std::max(m, std::abs(c.y[pos][a][b]));
            syncC[k] = m;
        }
        sum += std::norm(syncC[k]);
    }
    double esMeasured = sum / SYNC_SYMBOLS - noise;
    double es = std::max(esMeasured, 1e-3 * noise);
    double amp = std::sqrt(es);

    // Log-likelihood of each glide: non-coherent detection of a known
    // waveform of energy es in noise, log I0(2 A |y| / N).
    std::vector<double> llm((size_t)SYMBOLS_PER_FRAME * NOTES * NOTES);
    auto G = [&](int k, int a, int b) -> double& { return llm[((size_t)k * NOTES + a) * NOTES + b]; };
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
        for (int a = 0; a < NOTES; a++)
            for (int b = 0; b < NOTES; b++) G(k, a, b) = logI0(2.0 * amp * std::sqrt(P(k, a, b)) / noise);

    // BCJR over the note trellis for each data block. The block starts in a
    // known state (the last motif note) and is followed by a known target
    // note (the first note of the next motif), which is extra evidence for
    // the last data note because that glide starts from it.
    double notePost[DATA_SYMBOLS][NOTES];
    int d = 0;
    int pos = MOTIF_LENGTH;
    for (int blk = 0; blk < 2; blk++)
    {
        const int nb = DATA_BLOCK_LENGTH[blk];
        std::vector<std::array<double, NOTES>> alpha((size_t)nb + 1), beta((size_t)nb + 1);
        for (int b = 0; b < NOTES; b++) alpha[0][b] = b == COSTAS7[MOTIF_LENGTH - 1] ? 0.0 : NEG_INF;
        double v[NOTES];
        for (int t = 0; t < nb; t++)
        {
            for (int b = 0; b < NOTES; b++)
            {
                for (int a = 0; a < NOTES; a++) v[a] = alpha[t][a] + G(pos + t, a, b);
                alpha[t + 1][b] = logSumExp(v, NOTES);
            }
        }
        for (int a = 0; a < NOTES; a++) beta[nb][a] = G(pos + nb, a, COSTAS7[0]);
        for (int t = nb - 1; t >= 0; t--)
        {
            for (int a = 0; a < NOTES; a++)
            {
                for (int b = 0; b < NOTES; b++) v[b] = G(pos + t, a, b) + beta[t + 1][b];
                beta[t][a] = logSumExp(v, NOTES);
            }
        }
        for (int t = 0; t < nb; t++, d++)
        {
            for (int b = 0; b < NOTES; b++) v[b] = alpha[t + 1][b] + beta[t + 1][b];
            double norm = logSumExp(v, NOTES);
            for (int b = 0; b < NOTES; b++) notePost[d][b] = v[b] - norm;
        }
        pos += nb + MOTIF_LENGTH;
    }

    // Bit LLRs through the Gray labels, clipped as the prototype does.
    for (int s = 0; s < DATA_SYMBOLS; s++)
    {
        for (int i = 0; i < BITS_PER_SYMBOL; i++)
        {
            double zero[NOTES], one[NOTES];
            int nz = 0, no = 0;
            for (int note = 0; note < NOTES; note++)
            {
                if ((GRAY[note] >> (BITS_PER_SYMBOL - 1 - i)) & 1)
                    one[no++] = notePost[s][note];
                else
                    zero[nz++] = notePost[s][note];
            }
            double llr = logSumExp(zero, nz) - logSumExp(one, no);
            llrs[(size_t)s * BITS_PER_SYMBOL + i] = (float)std::min(std::max(llr, -30.0), 30.0);
        }
    }

    report = channelReport(syncC, es, noise, gear, voice);
    // Es / N0: |y|^2 of a symbol is Es L^2 A^2 in noise of variance N0 L.
    return std::isfinite(esMeasured) ? esMeasured / noise : NAN;
}

// Decision-directed channel sounding after a good decode (prototype
// sound_channel()). With the payload known, every note of the melody is
// known, and so is the waveform's running phase. Each symbol's correlation
// is then a sample of the channel gain at that note's pitch. Pairs of
// symbols on the same note at a lag of d symbols estimate the time
// correlation rho(d T) = exp(-2 pi^2 sigma^2 (d T)^2) of a Gaussian scatter
// path; the fit gives the Doppler spread 2 sigma. Using the same note
// removes the frequency selectivity that multipath delay adds between notes.
// Returns a negative value when the signal is too weak to say.
double soundChannel(const Correlations& corr, const GearInfo& gear, const VoiceTemplates& voice,
                    const Payload& payload)
{
    const double T = gear.symbolSeconds;
    std::array<int, SYMBOLS_PER_FRAME> notes = frameNotes(encodeFrame(payload));

    std::complex<double> c[SYMBOLS_PER_FRAME];
    double advance = 0.0;
    double powerSum = 0.0;
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
    {
        int prev = k > 0 ? notes[k - 1] : notes[0];
        c[k] = corr.y[k][prev][notes[k]] * std::polar(1.0, -advance);
        advance += voice.advance[prev][notes[k]];
        powerSum += std::norm(c[k]);
    }

    std::vector<double> holdPowers;
    holdPowers.reserve((size_t)SYMBOLS_PER_FRAME * NOTES);
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
        for (int b = 0; b < NOTES; b++) holdPowers.push_back(std::norm(corr.hold[k][b]));
    // The sustain templates are shorter than the full ones by the glide.
    double noise = median(holdPowers) / LN2 / (1.0 - gear.glide);
    double power = powerSum / SYMBOLS_PER_FRAME;
    if (power <= noise) return -1.0;

    // |E[c(t + tau) c*(t)]|^2 via the unbiased pair statistic, so lags with
    // few same-note pairs do not read as falsely coherent.
    std::vector<double> lags, rhos, weights;
    int maxLag = std::max(2, (int)(2.0 / T));
    for (int d = 1; d <= maxLag; d++)
    {
        std::complex<double> total = 0.0;
        double squares = 0.0;
        int count = 0;
        for (int k = 0; k + d < SYMBOLS_PER_FRAME; k++)
        {
            if (notes[k] != notes[k + d]) continue;
            std::complex<double> zk = c[k + d] * std::conj(c[k]);
            total += zk;
            squares += std::norm(zk);
            count++;
        }
        if (count < 3) continue;
        double r2 = (std::norm(total) - squares) / ((double)count * (count - 1));
        double rho = std::sqrt(std::max(r2, 0.0)) / (power - noise);
        lags.push_back(d * T);
        rhos.push_back(std::min(std::max(rho, 0.0), 0.99));
        weights.push_back(count);
    }

    // Fit only the first run of lags that still show correlation; single
    // frames have few same-note pairs, so the noisy tail is ignored.
    const double floor = 0.15;
    size_t run = 0;
    while (run < rhos.size() && rhos[run] > floor) run++;
    if (run == 0)
    {
        // Decorrelated within one symbol: report the bound, i.e. "fast".
        return 2.0 * std::sqrt(-std::log(floor) / (2.0 * PI * PI)) / T;
    }
    double sxy = 0.0, sxx = 0.0;
    for (size_t i = 0; i < run; i++)
    {
        double x = lags[i] * lags[i];
        double y = -std::log(rhos[i]);
        sxy += weights[i] * x * y;
        sxx += weights[i] * x * x;
    }
    double s2 = sxy / sxx / (2.0 * PI * PI);
    return 2.0 * std::sqrt(std::max(s2, 0.0));
}

} // namespace

// ---------------------------------------------------------------- public (detail)

void analyticSignal(const float* x, size_t n, ComplexSignal& out)
{
    out.resize(n);
    if (n == 0) return;
    size_t N = nextPowerOfTwo(n);
    std::shared_ptr<const Fft> fft = fftOfSize(N);
    std::vector<double> buffer(2 * N, 0.0);
    for (size_t i = 0; i < n; i++) buffer[2 * i] = x[i];
    fft->forward(buffer.data());

    // Keep DC and Nyquist, double the positive frequencies, drop the negative.
    for (size_t k = 1; k < N / 2; k++)
    {
        buffer[2 * k] *= 2.0;
        buffer[2 * k + 1] *= 2.0;
    }
    for (size_t k = N / 2 + 1; k < N; k++)
    {
        buffer[2 * k] = 0.0;
        buffer[2 * k + 1] = 0.0;
    }
    fft->inverse(buffer.data());
    double scale = 1.0 / (double)N;
    for (size_t i = 0; i < n; i++)
    {
        out.re[i] = (float)(buffer[2 * i] * scale);
        out.im[i] = (float)(buffer[2 * i + 1] * scale);
    }
}

std::shared_ptr<const VoiceTemplates> voiceTemplates(Scale scale, int voice, int gear, double tuningOffsetHz)
{
    // G4 and voice 0 of G5 share their templates: the key is what the
    // waveform depends on, not the gear number.
    const GearInfo& info = gearInfo(gear);
    using Key = std::tuple<int, int, int, double, double>;
    Key key((int)scale, voice, info.samplesPerSymbol(), info.glide, tuningOffsetHz);

    static std::mutex mutex;
    static std::map<Key, std::shared_ptr<const VoiceTemplates>> cache;
    static std::vector<Key> age; // oldest first
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    // Built outside the lock: a G1 set takes a moment and other threads may
    // want other entries meanwhile. Two threads building the same entry at
    // once is harmless.
    std::shared_ptr<const VoiceTemplates> built = buildTemplates(voiceFrequencies(scale, voice, tuningOffsetHz), info);

    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    // A few MB per G1 entry; keep enough for every gear and voice of two
    // tunings, and forget the oldest beyond that.
    const size_t MAX_ENTRIES = 16;
    while (cache.size() >= MAX_ENTRIES && !age.empty())
    {
        cache.erase(age.front());
        age.erase(age.begin());
    }
    cache[key] = built;
    age.push_back(key);
    return built;
}

// The all-zero word passes the CRC (its CRC is zero), and it is what a frame
// decodes to when much of it is digital silence: symbols with no energy give
// zero LLRs, which lean on nothing, and a noiseless tail of a real frame
// then measures a huge Es/N0. So, as FT8 decoders do, never report it.
// GlissandoLink never sends it: the first segment of a burst starts with
// the chat frame's type byte, which is never zero.
static bool allZero(const Payload& payload)
{
    for (uint8_t bit : payload)
        if (bit) return false;
    return true;
}

VoiceDecode receiveVoice(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice,
                         long long searchFrom, long long searchTo, double maxOffsetHz, int candidates)
{
    VoiceDecode result;
    std::unique_ptr<Correlations> corr(new Correlations);
    for (const SyncCandidate& found : syncSearch(z, gear, voice, searchFrom, searchTo, maxOffsetHz, candidates))
    {
        SyncCandidate sync = refineSync(z, voice, found);
        // Refinement may walk a peak at the edge of the search range out of
        // it; the range is a promise to the caller, so such a peak is not
        // a candidate.
        if (std::fabs(sync.df) > maxOffsetHz) continue;

        correlate(z, voice, sync, *corr);
        FrameLlrs llrs;
        ChannelReport report;
        double esOverN0 = demodulate(*corr, gear, voice, llrs, report);
        if (std::isnan(esOverN0)) continue;

        // Too weak to decode by a wide margin: no Viterbi, so no chance of
        // a CRC passing by luck (see MIN_ES_OVER_N0).
        Payload payload{};
        bool ok = esOverN0 >= MIN_ES_OVER_N0 && decodeFrame(llrs, payload) && !allZero(payload);

        if (!result.haveCandidate)
        {
            // Until something decodes, report the best sync candidate.
            result.haveCandidate = true;
            result.decode.startSample = sync.start;
            result.decode.frequencyOffsetHz = sync.df;
            result.decode.report = report;
            result.syncScore = sync.score;
            result.esOverN0 = esOverN0;
        }
        if (ok)
        {
            double doppler = soundChannel(*corr, gear, voice, payload);
            if (doppler >= 0) report.dopplerHz = doppler;
            result.decode.ok = true;
            result.decode.payload = payload;
            result.decode.startSample = sync.start;
            result.decode.frequencyOffsetHz = sync.df;
            result.decode.report = report;
            result.syncScore = sync.score;
            result.esOverN0 = esOverN0;
            break;
        }
    }
    return result;
}

} // namespace detail
} // namespace Glissando
