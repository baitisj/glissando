//=========================================================================
// Name:            GlissandoInternal.h
// Purpose:         Pieces of the Glissando modem shared by the batch
//                  receiver, the streaming receiver and the tests. Not part
//                  of the public API.
//=========================================================================

#ifndef GLISSANDO__GLISSANDO_INTERNAL_H
#define GLISSANDO__GLISSANDO_INTERNAL_H

#include <array>
#include <complex>
#include <memory>
#include <vector>

#include "GlissandoFec.h"
#include "GlissandoModem.h"

namespace Glissando
{
namespace detail
{

constexpr int BITS_PER_SYMBOL = 3;
constexpr int DATA_SYMBOLS = FRAME_BITS / BITS_PER_SYMBOL;     // 65
constexpr int MOTIF_LENGTH = 7;
constexpr int SYNC_SYMBOLS = 3 * MOTIF_LENGTH;                  // 21

// FT8's 7x7 Costas array, played as a tune: the signature motif.
constexpr int COSTAS7[MOTIF_LENGTH] = {3, 1, 4, 0, 6, 5, 2};

// Note index -> 3 bit label, so neighbouring notes differ in one bit.
constexpr int GRAY[NOTES] = {0, 1, 3, 2, 6, 7, 5, 4};

// Layout: motif(7) data(32) motif(7) data(33) motif(7) = 86 symbols.
constexpr int DATA_BLOCK_LENGTH[2] = {32, 33};
constexpr int MOTIF_START[3] = {0, 39, 79};

// Symbol index of the k-th sync symbol (k = 0..20).
inline int syncPosition(int k)
{
    return MOTIF_START[k / MOTIF_LENGTH] + k % MOTIF_LENGTH;
}

// Instantaneous frequency, in Hz, of each of the L samples of a symbol
// gliding from fa to fb (prototype _pitch_track()).
void pitchTrack(double fa, double fb, int L, double glide, double* f);

// Note of every symbol of a frame carrying these coded bits.
std::array<int, SYMBOLS_PER_FRAME> frameNotes(const CodedFrame& coded);

// Note frequencies of one voice, tuning offset included.
std::array<double, NOTES> voiceFrequencies(Scale scale, int voice, double tuningOffsetHz);

// Complex samples as separate real and imaginary arrays, which keeps the
// inner loops simple enough for the compiler to vectorise.
struct ComplexSignal
{
    std::vector<float> re;
    std::vector<float> im;

    size_t size() const { return re.size(); }
    void resize(size_t n)
    {
        re.resize(n);
        im.resize(n);
    }
};

// The analytic signal of x (prototype glissando.analytic()), computed with
// one FFT of the next power of two at or above n; the zero padding only
// changes the few samples nearest each end.
void analyticSignal(const float* x, size_t n, ComplexSignal& out);

// Pre-computed matched filter templates for one voice at one tempo (the
// prototype's Voice class), cached by voiceTemplates().
struct VoiceTemplates
{
    int samplesPerSymbol = 0;   // L
    int glideSamples = 0;       // n0 = ceil(glide * L): the glide is over before this sample
    int holdSamples = 0;        // L - n0
    std::array<double, NOTES> notes{};

    // Every (from a, to b) glide template t_ab is split in two. Samples
    // [0, n0) are the glide itself, stored per pair; from n0 on the pitch is
    // constant at note b, so t_ab[n] = rotation[a][b] * hold_b[n] with hold_b
    // the sustain template of note b. Correlating against all 64 templates
    // then costs 64 short glide correlations and 8 sustain correlations.
    std::vector<float> glideRe, glideIm;    // [(a * 8 + b) * n0 + n]
    std::vector<float> holdRe, holdIm;      // [b * holdSamples + (n - n0)]
    std::complex<double> rotation[NOTES][NOTES];
    double advance[NOTES][NOTES];           // phase advance over a whole glide symbol, radians

    // Full length templates of the 21 sync symbols (prototype
    // _sync_templates()): a sync symbol that follows data has an unknown
    // glide, so only its sustain is used.
    std::vector<float> syncRe, syncIm;      // [k * L + n]

    // The sustain templates shifted by each trial frequency step of the fine
    // frequency search in refineSync(), [(i * 8 + b) * holdSamples + n].
    static constexpr int FREQ_STEPS = 13;   // -6..6 steps
    std::vector<float> holdShiftRe, holdShiftIm;
    double freqStepHz = 0.0;
};

std::shared_ptr<const VoiceTemplates> voiceTemplates(Scale scale, int voice, int gear, double tuningOffsetHz);

// Everything the receiver works out about one voice of one frame.
struct VoiceDecode
{
    bool haveCandidate = false; // false when no sync peak cleared the floors (or none fit the range)
    Decode decode;              // startSample in z's sample numbering
    double syncScore = 0.0;     // sync peak over the median sync score
    double esOverN0 = 0.0;      // signal to noise ratio per symbol, measured on the sync symbols
};

// Searches z for a frame of one voice starting in [searchFrom, searchTo) and
// decodes it (prototype glissando.receive() for one voice): coarse sync,
// then for each of the best `candidates` sync peaks: refine, soft
// demodulation, Viterbi and CRC, and channel sounding once a CRC passes.
VoiceDecode receiveVoice(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice,
                         long long searchFrom, long long searchTo, double maxOffsetHz, int candidates);

} // namespace detail
} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_INTERNAL_H
