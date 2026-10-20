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

// A frame that fails to decode is tried again on the guess that it ends in
// zero padding, longest guess first. GlissandoLink pads a burst's last
// segment out with zero bytes, and told which bits are zero the decoder
// needs less signal: at Presto in AWGN the 50 % point drops 0.8 dB for a
// frame with 64 bits of padding (one byte of data in the segment) and
// 0.45 dB for 48. Nothing changes on the air, so this works with every
// sender. Each guess is another chance for noise to pass the CRC (noise
// alone, 10000 Presto searches: one false decode, none without the guesses),
// so there are only two; a guess of 32 bits bought only 0.2 dB.
constexpr int PADDING_HYPOTHESES[] = {64, 48};

// A payload must also look like what was heard: re-encoded, it may
// disagree with the sign of at most MAX_HARD_ERRORS of the 195 soft bits
// (FT8 decoders make the same check). Every path the decoder tries is a
// chance for noise to pass the 14-bit CRC. Through noise the best path as
// heard disagrees in about 25 to 50 bits, and with a zero padding guess
// forcing it, in 55 to 100; frames that decode near threshold disagree in
// 0 to 44, mostly under 35, in AWGN and in fading alike, at every tempo.
// So the ceiling turns away most of the padding guesses' paths through
// noise, which were most of the chances, and none of the right decodes
// measured.
constexpr int MAX_HARD_ERRORS = 44;

// List decoding, after JTTY's decoder. When the most likely payload of
// every guess fails, the right one is often a runner-up: the decoder keeps
// the LIST_SIZE most likely paths of each guess and tries the rest in turn.
// In one frame trials at the 50 % point it decodes 28 % more frames at
// Allegro in AWGN and 49 % more in CCIR poor fading, and about 30 % more at
// Adagio, Andante and Presto in fading.
//
// A runner-up must agree with what was heard much more closely than noise
// does: the soft bits it disagrees with may hold at most
// MAX_LIST_DISAGREEMENT of the frame's soft weight (disagreement()). Right
// runners-up measure 0.02 to 0.09, under 0.075 in fading, which piles the
// evidence onto the frame's strong stretches; paths through noise measure
// 0.045 to 0.12, mostly over 0.08. Hard errors tell the two apart far less
// well. In 160 minutes of busy traffic at every tempo about 7000
// runners-up an hour got under the ceiling, each a 1 in 16384 chance for
// noise: a false decode every two hours or so before the caller's payload
// check, where a ceiling of 40 hard errors let in 89000 an hour, and in
// fading fewer right ones.
constexpr int LIST_SIZE = 16;
constexpr double MAX_LIST_DISAGREEMENT = 0.075;

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
    int hypothesis = 0;     // which of the voices searched (one per scale)
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

// Coarse-to-fine search over start time, frequency offset and scale
// (prototype sync_search()): starts every L/8 samples, the best `top` well
// separated peaks, then each re-searched every L/256 samples around its
// start with a parabolic fit to the frequency peak.
//
// `voices` holds one hypothesis per scale listened for. Each hypothesis's
// scores are normalised by their own median, which makes them comparable,
// and the best `top` peaks are taken from all of them together. So
// listening for four scales costs four coarse searches (a small part of a
// search) but no more candidates to refine and decode, and no more chances
// for a CRC to pass by luck, than listening for one.
std::vector<SyncCandidate> syncSearch(const ComplexSignal& z, const GearInfo& gear,
                                      const std::vector<const VoiceTemplates*>& voices, long long t0, long long t1,
                                      double maxOffsetHz, int top)
{
    std::vector<SyncCandidate> out;
    if (voices.empty()) return out;
    const int L = voices[0]->samplesPerSymbol;
    const long long frame = (long long)SYMBOLS_PER_FRAME * L;
    const long long step = std::max(1, L / 8);
    t0 = std::max(0LL, t0);
    t1 = std::min(t1, (long long)z.size() - frame);
    if (t1 <= t0 || top <= 0) return out;

    std::vector<std::unique_ptr<SyncSearch>> searches;
    for (const VoiceTemplates* voice : voices) searches.emplace_back(new SyncSearch(z, *voice, maxOffsetHz));
    // Every hypothesis has the same symbol length and search range, so the
    // same bins.
    const size_t bins = searches[0]->numBins();
    std::vector<long long> starts;
    for (long long s = t0; s < t1; s += step) starts.push_back(s);
    const size_t cells = starts.size() * bins;

    std::vector<double> S(voices.size() * cells);
    std::vector<double> sorted;
    for (size_t h = 0; h < voices.size(); h++)
    {
        double* grid = S.data() + h * cells;
        for (size_t i = 0; i < starts.size(); i++) searches[h]->score(starts[i], grid + i * bins);

        // Noise-normalise by the typical (median) score across the search
        // space. Silence has no typical score, and nothing to find.
        sorted.assign(grid, grid + cells);
        double typical = median(sorted);
        if (!(typical > 0.0) || !std::isfinite(typical)) return out;
        for (size_t i = 0; i < cells; i++) grid[i] /= typical;
    }

    // Best peaks first, skipping any within a symbol and 4 bins of symbol
    // resolution of one already taken, in any scale: there it is a sidelobe
    // of the same frame heard through the wrong scale's templates.
    std::vector<size_t> order(S.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return S[a] > S[b]; });
    const SyncSearch& any = *searches[0];
    std::vector<SyncCandidate> coarse;
    for (size_t idx : order)
    {
        size_t cell = idx % cells;
        long long s = starts[cell / bins];
        double f = any.binFrequency(cell % bins);
        bool near = false;
        for (const SyncCandidate& c : coarse)
        {
            if (std::llabs(s - c.start) < L && std::fabs(f - c.df) < 4.0 / gear.symbolSeconds) near = true;
        }
        if (near) continue;
        if (!(S[idx] >= MIN_SYNC_SCORE)) break; // the rest are weaker still
        coarse.push_back({s, f, S[idx], (int)(idx / cells)});
        if ((int)coarse.size() >= top) break;
    }

    const long long fineStep = std::max(1, L / 256);
    std::vector<double> row(bins);
    for (const SyncCandidate& c : coarse)
    {
        SyncSearch& search = *searches[(size_t)c.hypothesis];
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
        out.push_back({bestStart, df, c.score, c.hypothesis});
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
    return {start, df, sync.score, sync.hypothesis};
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
    // A few MB per G1 entry; keep enough for every gear and voice of every
    // scale (a receiver listening for all of them) at two tunings, and
    // forget the oldest beyond that.
    const size_t MAX_ENTRIES = 2 * SCALE_COUNT * 6;
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

int hardErrors(const FrameLlrs& llrs, const Payload& payload)
{
    CodedFrame coded = encodeFrame(payload);
    int errors = 0;
    for (int i = 0; i < FRAME_BITS; i++)
    {
        float llr = llrs[(size_t)i];
        if (llr != 0.0f && (llr < 0.0f ? 1 : 0) != coded[(size_t)i]) errors++;
    }
    return errors;
}

double disagreement(const FrameLlrs& llrs, const Payload& payload)
{
    CodedFrame coded = encodeFrame(payload);
    double against = 0.0;
    double total = 0.0;
    for (int i = 0; i < FRAME_BITS; i++)
    {
        float llr = llrs[(size_t)i];
        total += std::fabs(llr);
        if (llr != 0.0f && (llr < 0.0f ? 1 : 0) != coded[(size_t)i]) against += std::fabs(llr);
    }
    return total > 0.0 ? against / total : 1.0;
}

namespace
{

// The decoder's guesses, in the order it tries them: as heard, the zero
// padding guesses, then the caller's.
std::vector<KnownBits> ladderGuesses(const std::vector<KnownBits>& knownBits)
{
    std::vector<KnownBits> guesses;
    guesses.push_back(noKnownBits());
    for (int knownZeroBits : PADDING_HYPOTHESES)
    {
        KnownBits known = noKnownBits();
        for (int i = PAYLOAD_BITS - knownZeroBits; i < PAYLOAD_BITS; i++) known[(size_t)i] = 0;
        guesses.push_back(known);
    }
    guesses.insert(guesses.end(), knownBits.begin(), knownBits.end());
    return guesses;
}

} // namespace

bool decodeWithGuesses(const FrameLlrs& llrs, const std::vector<KnownBits>& knownBits, Payload& payloadOut,
                       LadderStep* stepOut)
{
    const std::vector<KnownBits> guesses = ladderGuesses(knownBits);
    for (size_t g = 0; g < guesses.size(); g++)
    {
        if (!decodeFrame(llrs, payloadOut, guesses[g]) || allZero(payloadOut)) continue;
        int errors = hardErrors(llrs, payloadOut);
        if (errors > MAX_HARD_ERRORS) continue;
        if (stepOut != nullptr) *stepOut = {(int)g, 0, errors};
        return true;
    }
    return false;
}

bool decodeListWithGuesses(const FrameLlrs& llrs, const std::vector<KnownBits>& knownBits, const PayloadCheck& check,
                           Payload& payloadOut, LadderStep* stepOut)
{
    const std::vector<KnownBits> guesses = ladderGuesses(knownBits);
    for (size_t g = 0; g < guesses.size(); g++)
    {
        for (const ListDecode& d : decodeFrameList(llrs, guesses[g], LIST_SIZE))
        {
            if (d.rank == 0 || allZero(d.payload)) continue;
            if (disagreement(llrs, d.payload) > MAX_LIST_DISAGREEMENT) continue;
            if (check && !check(d.payload)) continue;
            payloadOut = d.payload;
            if (stepOut != nullptr) *stepOut = {(int)g, d.rank, hardErrors(llrs, d.payload)};
            return true;
        }
    }
    return false;
}

namespace
{

// The frame a payload sings, as subtractFrame() and refineFrame() need it.
struct SentFrame
{
    // Every symbol's glide template, carried on the running phase of the
    // glides before it (the waveform is phase continuous) and faded at each
    // end as modulate() fades it, before any frequency offset.
    std::vector<std::complex<float>> wave;
    // Where its pitch sits between the notes of the scale, sample by sample:
    // above note[m], with weight toNext[m] on the note above that.
    std::vector<uint8_t> note;
    std::vector<float> toNext;
};

SentFrame sentFrame(const VoiceTemplates& voice, const GearInfo& gear, const Payload& payload)
{
    const int L = voice.samplesPerSymbol;
    const int n0 = voice.glideSamples;
    const int holdLength = voice.holdSamples;
    const long long frame = (long long)SYMBOLS_PER_FRAME * L;
    const std::array<int, SYMBOLS_PER_FRAME> notes = frameNotes(encodeFrame(payload));
    SentFrame out;
    out.wave.resize((size_t)frame);
    out.note.resize((size_t)frame);
    out.toNext.resize((size_t)frame);
    {
        std::vector<double> pitch((size_t)L);
        const int ramp = SAMPLE_RATE_HZ / 100;
        double running = 0.0;
        for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
        {
            const int a = k > 0 ? notes[(size_t)k - 1] : notes[0];
            const int b = notes[(size_t)k];
            const std::complex<double> carrier = std::polar(1.0, running);
            running = std::fmod(running + voice.advance[a][b], 2.0 * PI);
            const size_t g = ((size_t)a * NOTES + b) * n0;
            const size_t h = (size_t)b * holdLength;
            pitchTrack(voice.notes[(size_t)a], voice.notes[(size_t)b], L, gear.glide, pitch.data());
            for (int n = 0; n < L; n++)
            {
                const long long m = (long long)k * L + n;
                std::complex<double> t = n < n0 ? std::complex<double>(voice.glideRe[g + n], voice.glideIm[g + n])
                                                : voice.rotation[a][b] * std::complex<double>(voice.holdRe[h + n - n0],
                                                                                              voice.holdIm[h + n - n0]);
                double w = 1.0;
                if (m < ramp)
                    w = 0.5 - 0.5 * std::cos(PI * (double)m / ramp);
                else if (m >= frame - ramp)
                    w = 0.5 - 0.5 * std::cos(PI * (double)(frame - 1 - m) / ramp);
                out.wave[(size_t)m] = std::complex<float>(w * carrier * t);

                const double f = pitch[(size_t)n];
                int j = 0;
                while (j < NOTES - 2 && f >= voice.notes[(size_t)j + 1]) j++;
                double x = (f - voice.notes[(size_t)j]) / (voice.notes[(size_t)j + 1] - voice.notes[(size_t)j]);
                out.note[(size_t)m] = (uint8_t)j;
                out.toNext[(size_t)m] = (float)std::min(std::max(x, 0.0), 1.0);
            }
        }
    }
    return out;
}

} // namespace

void refineFrame(const ComplexSignal& z, const VoiceTemplates& voice, const GearInfo& gear, const Payload& payload,
                 long long& start, double& dfHz)
{
    const int L = voice.samplesPerSymbol;
    const long long size = (long long)z.size();
    const SentFrame sent = sentFrame(voice, gear, payload);

    // The decoder's timing and offset are good enough to decode, not always
    // to cancel: a glide is a chirp, and a little early on a chirp looks much
    // like a little low, so the sync search can trade one for the other by
    // 10 ms or so. With every note now known, the whole melody times the
    // frame far more sharply than the 21 sync symbols did. Each symbol's
    // correlation is squared before summing, so fading does not matter.
    auto fit = [&](long long at, double df) {
        double total = 0.0;
        for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
        {
            const long long base = at + (long long)k * L;
            if (base < 0 || base + L > size) continue;
            std::complex<double> c = 0.0;
            const std::complex<float>* t = sent.wave.data() + (size_t)k * L;
            forEachPhasor(-df, (size_t)L, [&](size_t n, double pr, double pi) {
                std::complex<double> y(z.re[(size_t)(base + (long long)n)], z.im[(size_t)(base + (long long)n)]);
                c += y * std::complex<double>(pr, pi) * std::conj(std::complex<double>(t[n]));
            });
            total += std::norm(c);
        }
        return total;
    };
    long long at = start;
    double df = dfHz;
    {
        double best = fit(at, df);
        auto searchTime = [&](long long half, long long step) {
            long long centre = at;
            for (long long d = -half; d <= half; d += step)
            {
                if (d == 0) continue;
                double e = fit(centre + d, df);
                if (e > best)
                {
                    best = e;
                    at = centre + d;
                }
            }
        };
        auto searchFrequency = [&](double half, double step) {
            double centre = df;
            for (double d = -half; d <= half + 1e-9; d += step)
            {
                if (std::fabs(d) < 1e-12) continue;
                double e = fit(at, centre + d);
                if (e > best)
                {
                    best = e;
                    df = centre + d;
                }
            }
        };
        const long long coarse = std::max(1, L / 64);
        searchTime(12 * coarse, coarse);
        const double resolution = 1.0 / gear.symbolSeconds;
        searchFrequency(0.25 * resolution, 0.03125 * resolution);
        const long long medium = std::max(1LL, coarse / 8);
        searchTime(coarse, medium);
        searchTime(medium, 1);
        searchFrequency(0.03125 * resolution, 0.0078125 * resolution);
        searchTime(2, 1);
    }
    start = at;
    dfHz = df;
}

void subtractFrame(ComplexSignal& z, const VoiceTemplates& voice, const GearInfo& gear, long long start, double dfHz,
                   const Payload& payload)
{
    const int L = voice.samplesPerSymbol;
    const long long frame = (long long)SYMBOLS_PER_FRAME * L;
    const long long size = (long long)z.size();
    if (start >= size || start + frame <= 0) return;
    const SentFrame sent = sentFrame(voice, gear, payload);
    const long long at = start;
    const double df = dfHz;

    // The frame as heard at that timing and offset.
    auto heard = [&](long long m) {
        double cycles = std::fmod(df * (double)m / SAMPLE_RATE_HZ, 1.0);
        return std::complex<double>(sent.wave[(size_t)m]) * std::polar(1.0, 2.0 * PI * cycles);
    };

    // An HF channel's gain changes with time (fading) and with frequency
    // (multipath: two paths a millisecond apart cancel at some notes and add
    // at others). So it is modelled as a gain at every note of the scale,
    // interpolated between notes as the pitch glides, and found by least
    // squares over a few symbols at a time. Per symbol: the normal equations
    // of that fit, A g = r.
    struct Normal
    {
        double A[NOTES][NOTES] = {};
        std::complex<double> r[NOTES] = {};
    };
    std::vector<Normal> normals(SYMBOLS_PER_FRAME);
    std::array<bool, SYMBOLS_PER_FRAME> measured{};
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
    {
        const long long base = at + (long long)k * L;
        if (base < 0 || base + L > size) continue;
        Normal& eq = normals[(size_t)k];
        for (int n = 0; n < L; n++)
        {
            const long long m = (long long)k * L + n;
            const std::complex<double> v = heard(m);
            const std::complex<double> y(z.re[(size_t)(base + n)], z.im[(size_t)(base + n)]);
            const double e = std::norm(v);
            const std::complex<double> c = y * std::conj(v);
            const int j = sent.note[(size_t)m];
            const double w1 = sent.toNext[(size_t)m];
            const double w0 = 1.0 - w1;
            eq.A[j][j] += w0 * w0 * e;
            eq.A[j][j + 1] += w0 * w1 * e;
            eq.A[j + 1][j] += w0 * w1 * e;
            eq.A[j + 1][j + 1] += w1 * w1 * e;
            eq.r[j] += w0 * c;
            eq.r[j + 1] += w1 * c;
        }
        measured[(size_t)k] = true;
    }

    // Each symbol's gains come from it and its neighbours over about a
    // quarter second, which follows CCIR poor fading (1 Hz) and averages
    // down the noise. Notes the stretch hardly sounds lean towards the
    // stretch's single, flat gain rather than being left undetermined.
    const int span = std::max(1, (int)std::lround(0.12 / gear.symbolSeconds));
    std::vector<std::array<std::complex<double>, NOTES>> gain(SYMBOLS_PER_FRAME);
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
    {
        double A[NOTES][NOTES] = {};
        std::complex<double> r[NOTES] = {};
        for (int j = -span; j <= span; j++)
        {
            int i = k + j;
            if (i < 0 || i >= SYMBOLS_PER_FRAME || !measured[(size_t)i]) continue;
            double w = span + 1 - std::abs(j);
            for (int p = 0; p < NOTES; p++)
            {
                r[p] += w * normals[(size_t)i].r[p];
                for (int q = 0; q < NOTES; q++) A[p][q] += w * normals[(size_t)i].A[p][q];
            }
        }
        double largest = 0.0;
        double all = 0.0;
        std::complex<double> rAll = 0.0;
        for (int p = 0; p < NOTES; p++)
        {
            largest = std::max(largest, A[p][p]);
            rAll += r[p];
            for (int q = 0; q < NOTES; q++) all += A[p][q];
        }
        if (!(largest > 0.0) || !(all > 0.0)) continue;
        const std::complex<double> flat = rAll / all;
        const double ridge = 1e-2 * largest;
        for (int p = 0; p < NOTES; p++)
        {
            A[p][p] += ridge;
            r[p] += ridge * flat;
        }
        // Gaussian elimination; A is symmetric positive definite.
        for (int p = 0; p < NOTES; p++)
        {
            for (int q = p + 1; q < NOTES; q++)
            {
                double f = A[q][p] / A[p][p];
                if (f == 0.0) continue;
                for (int c = p; c < NOTES; c++) A[q][c] -= f * A[p][c];
                r[q] -= f * r[p];
            }
        }
        for (int p = NOTES - 1; p >= 0; p--)
        {
            std::complex<double> v = r[p];
            for (int c = p + 1; c < NOTES; c++) v -= A[p][c] * gain[(size_t)k][(size_t)c];
            gain[(size_t)k][(size_t)p] = v / A[p][p];
        }
    }

    // Take the frame out, its gains interpolated between notes as the pitch
    // glides and between symbol centres in time.
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
    {
        const long long base = at + (long long)k * L;
        if (base + L <= 0 || base >= size) continue;
        for (int n = 0; n < L; n++)
        {
            const long long m = (long long)k * L + n;
            const long long i = base + n;
            if (i < 0 || i >= size) continue;
            const int j = sent.note[(size_t)m];
            const double w1 = sent.toNext[(size_t)m];
            const double x = (n + 0.5) / L - 0.5; // -0.5 .. 0.5 across the symbol
            const int other = std::min(std::max(x < 0.0 ? k - 1 : k + 1, 0), SYMBOLS_PER_FRAME - 1);
            const double far = std::fabs(x);
            std::complex<double> g = (1.0 - w1) * ((1.0 - far) * gain[(size_t)k][(size_t)j] + far * gain[(size_t)other][(size_t)j]) +
                                     w1 * ((1.0 - far) * gain[(size_t)k][(size_t)j + 1] + far * gain[(size_t)other][(size_t)j + 1]);
            const std::complex<double> v = g * heard(m);
            z.re[(size_t)i] -= (float)v.real();
            z.im[(size_t)i] -= (float)v.imag();
        }
    }
}

double softAgreement(const FrameLlrs& a, const FrameLlrs& b)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (int i = 0; i < FRAME_BITS; i++)
    {
        ab += (double)a[(size_t)i] * b[(size_t)i];
        aa += (double)a[(size_t)i] * a[(size_t)i];
        bb += (double)b[(size_t)i] * b[(size_t)i];
    }
    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt(aa * bb) : 0.0;
}

VoiceDecode receiveVoice(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice,
                         long long searchFrom, long long searchTo, double maxOffsetHz, int candidates,
                         const std::vector<KnownBits>& knownBits)
{
    return receiveVoice(z, gear, std::vector<const VoiceTemplates*>{&voice}, searchFrom, searchTo, maxOffsetHz,
                        candidates, knownBits);
}

namespace
{

// Refines one sync candidate, demodulates and decodes it (the body of
// receiveVoice()'s loop). Returns true when it decoded; otherwise adds it
// to result.failed when it measured as anything at all.
bool tryCandidate(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice, const SyncCandidate& found,
                  double maxOffsetHz, const std::vector<KnownBits>& knownBits, Correlations& corr, VoiceDecode& result)
{
    SyncCandidate sync = refineSync(z, voice, found);
    // Refinement may walk a peak at the edge of the search range out of
    // it; the range is a promise to the caller, so such a peak is not
    // a candidate.
    if (std::fabs(sync.df) > maxOffsetHz) return false;

    correlate(z, voice, sync, corr);
    FrameLlrs llrs;
    ChannelReport report;
    double esOverN0 = demodulate(corr, gear, voice, llrs, report);
    if (std::isnan(esOverN0)) return false;

    // Too weak to decode by a wide margin: no Viterbi, so no chance of
    // a CRC passing by luck (see MIN_ES_OVER_N0).
    Payload payload{};
    LadderStep step;
    bool ok = esOverN0 >= MIN_ES_OVER_N0 && decodeWithGuesses(llrs, knownBits, payload, &step);

    if (!result.haveCandidate)
    {
        // Until something decodes, report the best sync candidate.
        result.haveCandidate = true;
        result.decode.startSample = sync.start;
        result.decode.frequencyOffsetHz = sync.df;
        result.decode.report = report;
        result.syncScore = sync.score;
        result.esOverN0 = esOverN0;
        result.hypothesis = sync.hypothesis;
    }
    if (ok)
    {
        double doppler = soundChannel(corr, gear, voice, payload);
        if (doppler >= 0) report.dopplerHz = doppler;
        result.decode.ok = true;
        result.decode.payload = payload;
        result.decode.startSample = sync.start;
        result.decode.frequencyOffsetHz = sync.df;
        result.decode.report = report;
        result.syncScore = sync.score;
        result.esOverN0 = esOverN0;
        result.hypothesis = sync.hypothesis;
        result.step = step;
        result.failed.clear();
        return true;
    }

    SoftFrame soft;
    soft.llrs = llrs;
    soft.startSample = sync.start;
    soft.frequencyOffsetHz = sync.df;
    soft.syncScore = sync.score;
    soft.esOverN0 = esOverN0;
    soft.hypothesis = sync.hypothesis;
    soft.report = report;
    result.failed.push_back(soft);
    return false;
}

} // namespace

VoiceDecode receiveVoice(const ComplexSignal& z, const GearInfo& gear, const std::vector<const VoiceTemplates*>& voices,
                         long long searchFrom, long long searchTo, double maxOffsetHz, int candidates,
                         const std::vector<KnownBits>& knownBits)
{
    VoiceDecode result;
    std::unique_ptr<Correlations> corr(new Correlations);
    for (const SyncCandidate& found : syncSearch(z, gear, voices, searchFrom, searchTo, maxOffsetHz, candidates))
    {
        if (tryCandidate(z, gear, *voices[(size_t)found.hypothesis], found, maxOffsetHz, knownBits, *corr, result))
            break;
    }
    return result;
}

VoiceDecode receiveAt(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice, long long start,
                      double dfHz, double maxOffsetHz, const std::vector<KnownBits>& knownBits)
{
    VoiceDecode result;
    const long long frame = (long long)SYMBOLS_PER_FRAME * voice.samplesPerSymbol;
    if (start < 0 || start + frame > (long long)z.size()) return result;
    std::unique_ptr<Correlations> corr(new Correlations);
    tryCandidate(z, gear, voice, {start, dfHz, 0.0, 0}, maxOffsetHz, knownBits, *corr, result);
    return result;
}

double soundDecoded(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice, long long start,
                    double dfHz, const Payload& payload)
{
    const long long frame = (long long)SYMBOLS_PER_FRAME * voice.samplesPerSymbol;
    if (start < 0 || start + frame > (long long)z.size()) return -1.0;
    std::unique_ptr<Correlations> corr(new Correlations);
    correlate(z, voice, {start, dfHz, 0.0, 0}, *corr);
    return soundChannel(*corr, gear, voice, payload);
}

bool decodeByList(const ComplexSignal& z, const GearInfo& gear, const std::vector<const VoiceTemplates*>& voices,
                  const std::vector<KnownBits>& knownBits, const PayloadCheck& check, VoiceDecode& result)
{
    if (result.decode.ok) return true;
    for (const SoftFrame& soft : result.failed)
    {
        if (soft.esOverN0 < MIN_ES_OVER_N0) continue;
        Payload payload{};
        LadderStep step;
        if (!decodeListWithGuesses(soft.llrs, knownBits, check, payload, &step)) continue;

        ChannelReport report = soft.report;
        double doppler = soundDecoded(z, gear, *voices[(size_t)soft.hypothesis], soft.startSample,
                                      soft.frequencyOffsetHz, payload);
        if (doppler >= 0) report.dopplerHz = doppler;
        result.decode.ok = true;
        result.decode.payload = payload;
        result.decode.startSample = soft.startSample;
        result.decode.frequencyOffsetHz = soft.frequencyOffsetHz;
        result.decode.report = report;
        result.syncScore = soft.syncScore;
        result.esOverN0 = soft.esOverN0;
        result.hypothesis = soft.hypothesis;
        result.step = step;
        result.failed.clear();
        return true;
    }
    return false;
}

} // namespace detail
} // namespace Glissando
