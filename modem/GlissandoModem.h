//=========================================================================
// Name:            GlissandoModem.h
// Purpose:         The Glissando melodic chirp modem, ported from the NumPy
//                  prototype in https://github.com/baitisj/glissando
//                  (prototype/glissando.py and prototype/fec.py).
//
// Every symbol is a glide from the previous note of an 8 note scale to the
// next, then a sustain on the new note. Three copies of a Costas array
// "signature motif" give time and frequency sync. A frame carries a 77 bit
// payload (plus CRC-14, K=7 r=1/2 convolutional code) in 86 symbols; the
// tempo "gear" sets how long a symbol lasts. See docs/DESIGN.md in the
// glissando repository for the reasoning behind every number.
//
// Nothing here depends on wxWidgets or codec2.
//=========================================================================

#ifndef GLISSANDO__GLISSANDO_MODEM_H
#define GLISSANDO__GLISSANDO_MODEM_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Glissando
{

constexpr int SAMPLE_RATE_HZ = 8000;
constexpr int PAYLOAD_BITS = 77;
constexpr int NOTES = 8;
constexpr int SYMBOLS_PER_FRAME = 86;

// The prototype's gears: 1 Adagio, 2 Andante, 3 Allegro, 4 Presto,
// 5 Presto duet (two voices, two payloads per frame).
constexpr int MIN_GEAR = 1;
constexpr int MAX_GEAR = 5;

struct GearInfo
{
    int number;
    const char* name;       // "G1".."G5"
    const char* tempo;      // "Adagio".."Presto duet"
    double symbolSeconds;   // 0.64, 0.32, 0.16, 0.08, 0.08
    int voices;             // 1, or 2 for the duet
    double glide;           // fraction of a symbol spent gliding (0.4)

    int samplesPerSymbol() const;
    double frameSeconds() const;
    int frameSamples() const;
};

// Throws nothing: an out of range gear is clamped.
const GearInfo& gearInfo(int gear);

enum class Scale
{
    Pentatonic,     // A minor pentatonic E4..A5; the default
    WholeTone,
    Diminished,
    Diabolus,
};

constexpr int SCALE_COUNT = 4;

const char* scaleName(Scale scale);     // "pentatonic", "wholetone", ...
bool scaleFromName(const std::string& name, Scale& scaleOut);

// The 8 note frequencies of one voice (0 low, 1 high) of a scale, in Hz,
// before any tuning offset.
std::array<double, NOTES> scaleNotes(Scale scale, int voice);

// 77 bits, one per element (0 or 1), first bit first as in the prototype.
using Payload = std::array<uint8_t, PAYLOAD_BITS>;

struct ModemSettings
{
    int gear = 3;
    Scale scale = Scale::Pentatonic;

    // Added to every note, in Hz, on transmit and on receive: the audio
    // equivalent of moving the tuning dial.
    double tuningOffsetHz = 0.0;
};

// One transmission: gearInfo(gear).voices payloads (one per voice), peak
// amplitude 1, constant envelope, with a 10 ms raised cosine fade at each end.
// Matches prototype glissando.transmit() to within float rounding when
// tuningOffsetHz is zero.
std::vector<float> modulate(const std::vector<Payload>& payloads, const ModemSettings& settings);

struct ChannelReport
{
    double snrDb = 0.0;         // in 2500 Hz
    double dopplerHz = 0.0;
    double coherence = 0.0;
};

struct Decode
{
    bool ok = false;            // CRC passed
    int voice = 0;
    Payload payload{};
    long long startSample = 0;  // of symbol 0, in the buffer's (or stream's) sample count
    double frequencyOffsetHz = 0.0;
    ChannelReport report;
};

// Batch receiver: decodes every voice of a frame of the given settings that
// starts within [searchFrom, searchTo) samples of audio. Mirrors prototype
// glissando.receive(); frequency search is +/- maxOffsetHz around the tuned
// notes. Returns one Decode per voice (ok false when nothing decoded; the
// report is then still the best candidate's).
std::vector<Decode> receive(const float* audio, size_t numSamples, const ModemSettings& settings,
                            long long searchFrom = 0, long long searchTo = -1,
                            double maxOffsetHz = 25.0, int candidates = 3);

// Pick the fastest gear the measured path supports (prototype
// recommend_gear()).
int recommendGear(double snrDb, double dopplerHz);

} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_MODEM_H
