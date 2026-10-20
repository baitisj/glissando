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

// The soft bits of a sync candidate that did not decode, kept so that a
// later copy of the same frame can be added to them (averaging; see
// combineSoftFrames()).
struct SoftFrame
{
    FrameLlrs llrs{};
    long long startSample = 0;  // in z's sample numbering
    double frequencyOffsetHz = 0.0;
    double syncScore = 0.0;
    double esOverN0 = 0.0;
    int hypothesis = 0;
    ChannelReport report;
};

// Where on the decoder's ladder a payload was found: the guess (0 as
// heard, then the zero padding guesses, then knownBits in order), the rank
// of its path in that guess's list (0 the most likely), and how many of the
// soft bits disagree with it re-encoded.
struct LadderStep
{
    int guess = 0;
    int rank = 0;
    int hardErrors = 0;
};

// Everything the receiver works out about one voice of one frame.
struct VoiceDecode
{
    bool haveCandidate = false; // false when no sync peak cleared the floors (or none fit the range)
    Decode decode;              // startSample in z's sample numbering
    double syncScore = 0.0;     // sync peak over the median sync score
    double esOverN0 = 0.0;      // signal to noise ratio per symbol, measured on the sync symbols
    int hypothesis = 0;         // index into the voices searched: which scale the frame was heard in
    LadderStep step;            // how the decoder found the payload, when it decoded

    // Candidates that were searched but did not decode, strongest sync
    // first, when no candidate decoded. Empty when one did.
    std::vector<SoftFrame> failed;
};

// The decoder's ladder on one set of soft bits: the most likely path as
// heard, then on the zero padding guesses, then on each of knownBits.
// Never reports the all-zero payload.
bool decodeWithGuesses(const FrameLlrs& llrs, const std::vector<KnownBits>& knownBits, Payload& payloadOut,
                       LadderStep* stepOut = nullptr);

// List decoding, for soft bits decodeWithGuesses() failed on: the
// runners-up of each of the same guesses in the same order, held to a
// ceiling on their disagreement() and, when it is set, to check. Every
// runner-up is another chance for noise to pass the CRC, so a receiver
// lists each frame it hears once, on its best copy.
bool decodeListWithGuesses(const FrameLlrs& llrs, const std::vector<KnownBits>& knownBits, const PayloadCheck& check,
                           Payload& payloadOut, LadderStep* stepOut = nullptr);

// How many of the soft bits disagree in sign with this payload re-encoded
// (bits with no information either way are not counted).
int hardErrors(const FrameLlrs& llrs, const Payload& payload);

// The share of the soft bits' weight (the sum of |LLR|) that is in the bits
// disagreeing in sign with this payload re-encoded: 0 when every bit
// agrees, 1 when there is no weight at all. Unlike hardErrors() it counts
// a confident bit against a payload for more than a doubtful one.
double disagreement(const FrameLlrs& llrs, const Payload& payload);

// Signal subtraction, as JTTY and FT8 decoders do it: a decoded frame is
// taken back out of the audio so that a weaker frame under it can be heard.
//
// refineFrame() times a decoded frame against the whole melody its payload
// sings, which is much sharper than the sync search (start in z's
// numbering, the offset in Hz; both are updated). subtractFrame()
// re-synthesises that melody at that start and offset, measures the
// channel's gain on it, note by note and symbol by symbol, and subtracts it.
// Parts of the frame outside z are left out.
void refineFrame(const ComplexSignal& z, const VoiceTemplates& voice, const GearInfo& gear, const Payload& payload,
                 long long& start, double& dfHz);
void subtractFrame(ComplexSignal& z, const VoiceTemplates& voice, const GearInfo& gear, long long start, double dfHz,
                   const Payload& payload);

// How alike two candidates' soft bits are: the normalised correlation of
// their LLRs, 1 for identical, near 0 for unrelated frames or noise (about
// 0.07 standard deviation over FRAME_BITS bits).
double softAgreement(const FrameLlrs& a, const FrameLlrs& b);

// Searches z for a frame of one voice starting in [searchFrom, searchTo) and
// decodes it (prototype glissando.receive() for one voice): coarse sync,
// then for each of the best `candidates` sync peaks: refine, soft
// demodulation, Viterbi and CRC, and channel sounding once a CRC passes.
VoiceDecode receiveVoice(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice,
                         long long searchFrom, long long searchTo, double maxOffsetHz, int candidates,
                         const std::vector<KnownBits>& knownBits = std::vector<KnownBits>());

// The same, listening for several scales at once: `voices` holds the
// templates of one voice in each scale (same gear and voice). The best
// `candidates` sync peaks are taken from all of them together, so this
// decodes no more candidates than a one-scale search. knownBits are the
// receiver's guesses (ModemSettings::knownBits), tried after the zero
// padding guesses.
VoiceDecode receiveVoice(const ComplexSignal& z, const GearInfo& gear, const std::vector<const VoiceTemplates*>& voices,
                         long long searchFrom, long long searchTo, double maxOffsetHz, int candidates,
                         const std::vector<KnownBits>& knownBits = std::vector<KnownBits>());

// Decodes the frame of one voice that starts near `start`, offset near dfHz
// (a candidate an earlier search found and could not decode), without a
// sync search: refinement, soft demodulation and the decoder's ladder.
VoiceDecode receiveAt(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice, long long start,
                      double dfHz, double maxOffsetHz, const std::vector<KnownBits>& knownBits);

// The Doppler spread of a decoded frame, sounded on its whole melody at
// this start and offset (soundChannel()), or -1 when that cannot be done.
double soundDecoded(const ComplexSignal& z, const GearInfo& gear, const VoiceTemplates& voice, long long start,
                    double dfHz, const Payload& payload);

// After a receiveVoice() that decoded nothing: list decoding on each of the
// candidates it tried, strongest sync first, filling in result as a decode
// does. voices are the ones receiveVoice() searched.
bool decodeByList(const ComplexSignal& z, const GearInfo& gear, const std::vector<const VoiceTemplates*>& voices,
                  const std::vector<KnownBits>& knownBits, const PayloadCheck& check, VoiceDecode& result);

} // namespace detail
} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_INTERNAL_H
