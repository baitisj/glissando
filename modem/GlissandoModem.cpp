//=========================================================================
// Name:            GlissandoModem.cpp
// Purpose:         Gears, scales, the transmitter and the batch receiver of
//                  the Glissando modem (prototype/glissando.py).
//=========================================================================

#include "GlissandoModem.h"

#include <algorithm>
#include <cmath>

#include "GlissandoFec.h"
#include "GlissandoInternal.h"

namespace Glissando
{

namespace
{

constexpr double PI = 3.14159265358979323846;

// The prototype's GEARS table. Doubling the symbol length buys 3 dB and
// halves the rate; the glide takes 40 % of every symbol (DESIGN 3.2).
const GearInfo GEARS[MAX_GEAR - MIN_GEAR + 1] = {
    {1, "G1", "Adagio", 0.64, 1, 0.4},
    {2, "G2", "Andante", 0.32, 1, 0.4},
    {3, "G3", "Allegro", 0.16, 1, 0.4},
    {4, "G4", "Presto", 0.08, 1, 0.4},
    {5, "G5", "Presto duet", 0.08, 2, 0.4},
};

// A minor pentatonic, equal temperament, A4 = 440 Hz: E4..A5 and C6..E7.
constexpr double PENTATONIC_LOW[NOTES] = {329.63, 392.00, 440.00, 523.25, 587.33, 659.26, 783.99, 880.00};
constexpr double PENTATONIC_HIGH[NOTES] = {1046.50, 1174.66, 1318.51, 1567.98, 1760.00, 2093.00, 2349.32, 2637.02};

// The tritone scales as MIDI note numbers (prototype SCALES). The high voice
// of each is the low voice a tritone plus an octave (18 semitones) up, which
// maps these symmetric scales onto themselves.
//   wholetone   E4..F#5 in whole tones
//   diminished  half-whole octatonic on E
//   diabolus    E major and Bb major triads a tritone apart, plus octaves
constexpr int WHOLETONE_MIDI[2][NOTES] = {{64, 66, 68, 70, 72, 74, 76, 78}, {82, 84, 86, 88, 90, 92, 94, 96}};
constexpr int DIMINISHED_MIDI[2][NOTES] = {{64, 65, 67, 68, 70, 71, 73, 74}, {82, 83, 85, 86, 88, 89, 91, 92}};
constexpr int DIABOLUS_MIDI[2][NOTES] = {{64, 68, 70, 71, 74, 76, 77, 80}, {82, 86, 88, 89, 92, 94, 95, 98}};

const char* const SCALE_NAMES[SCALE_COUNT] = {"pentatonic", "wholetone", "diminished", "diabolus"};

double midiToHz(int midi)
{
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

// One voice of a frame as real audio, phase continuous from symbol to
// symbol (prototype modulate_voice()).
void modulateVoice(const std::array<int, SYMBOLS_PER_FRAME>& notes, const std::array<double, NOTES>& freqs,
                   const GearInfo& gear, float* out)
{
    int L = gear.samplesPerSymbol();
    std::vector<double> f((size_t)L);
    double phase = 0.0;
    int prev = notes[0];
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
    {
        detail::pitchTrack(freqs[prev], freqs[notes[k]], L, gear.glide, f.data());
        double sum = 0.0;
        double current = phase;
        for (int n = 0; n < L; n++)
        {
            out[(size_t)k * L + n] += (float)std::sin(current);
            sum += f[n];
            current = phase + 2.0 * PI * sum / SAMPLE_RATE_HZ;
        }
        phase = std::fmod(current, 2.0 * PI);
        prev = notes[k];
    }
}

} // namespace

int GearInfo::samplesPerSymbol() const
{
    return (int)std::lround(symbolSeconds * SAMPLE_RATE_HZ);
}

double GearInfo::frameSeconds() const
{
    return SYMBOLS_PER_FRAME * symbolSeconds;
}

int GearInfo::frameSamples() const
{
    return SYMBOLS_PER_FRAME * samplesPerSymbol();
}

const GearInfo& gearInfo(int gear)
{
    gear = std::min(std::max(gear, MIN_GEAR), MAX_GEAR);
    return GEARS[gear - MIN_GEAR];
}

const char* scaleName(Scale scale)
{
    int index = (int)scale;
    if (index < 0 || index >= SCALE_COUNT) index = 0;
    return SCALE_NAMES[index];
}

bool scaleFromName(const std::string& name, Scale& scaleOut)
{
    for (int i = 0; i < SCALE_COUNT; i++)
    {
        if (name == SCALE_NAMES[i])
        {
            scaleOut = (Scale)i;
            return true;
        }
    }
    return false;
}

std::array<double, NOTES> scaleNotes(Scale scale, int voice)
{
    voice = voice != 0 ? 1 : 0;
    std::array<double, NOTES> notes{};
    const int(*midi)[NOTES] = nullptr;
    switch (scale)
    {
    case Scale::WholeTone:
        midi = WHOLETONE_MIDI;
        break;
    case Scale::Diminished:
        midi = DIMINISHED_MIDI;
        break;
    case Scale::Diabolus:
        midi = DIABOLUS_MIDI;
        break;
    case Scale::Pentatonic:
    default:
        break;
    }
    for (int i = 0; i < NOTES; i++)
    {
        if (midi != nullptr)
            notes[i] = midiToHz(midi[voice][i]);
        else
            notes[i] = voice == 0 ? PENTATONIC_LOW[i] : PENTATONIC_HIGH[i];
    }
    return notes;
}

namespace detail
{

// Instantaneous frequency for a glide fa -> fb then a sustain on fb
// (prototype _pitch_track()). The glide is linear in log frequency, the way
// a voice or a slide whistle moves, with a raised cosine ease so the
// frequency has no corners: no clicks, a compact spectrum.
void pitchTrack(double fa, double fb, int L, double glide, double* f)
{
    for (int n = 0; n < L; n++)
    {
        double t = (double)n / L;
        double x = glide > 0 ? std::min(std::max(t / glide, 0.0), 1.0) : 1.0;
        double ease = 0.5 - 0.5 * std::cos(PI * x);
        f[n] = fa * std::pow(fb / fa, ease);
    }
}

std::array<double, NOTES> voiceFrequencies(Scale scale, int voice, double tuningOffsetHz)
{
    std::array<double, NOTES> notes = scaleNotes(scale, voice);
    for (double& note : notes) note += tuningOffsetHz;
    return notes;
}

std::array<int, SYMBOLS_PER_FRAME> frameNotes(const CodedFrame& coded)
{
    // Inverse Gray map: 3 bit label -> note index.
    int grayInverse[NOTES];
    for (int note = 0; note < NOTES; note++) grayInverse[GRAY[note]] = note;

    std::array<int, SYMBOLS_PER_FRAME> notes{};
    int data = 0;
    int sync = 0;
    for (int k = 0; k < SYMBOLS_PER_FRAME; k++)
    {
        if (sync < SYNC_SYMBOLS && k == syncPosition(sync))
        {
            notes[k] = COSTAS7[sync % MOTIF_LENGTH];
            sync++;
        }
        else
        {
            const uint8_t* bits = coded.data() + BITS_PER_SYMBOL * data;
            notes[k] = grayInverse[bits[0] * 4 + bits[1] * 2 + bits[2]];
            data++;
        }
    }
    return notes;
}

} // namespace detail

std::vector<float> modulate(const std::vector<Payload>& payloads, const ModemSettings& settings)
{
    const GearInfo& gear = gearInfo(settings.gear);
    size_t length = (size_t)gear.frameSamples();
    std::vector<float> audio(length, 0.0f);

    for (int voice = 0; voice < gear.voices; voice++)
    {
        // A missing payload for a voice is sent as all zeros.
        Payload payload{};
        if ((size_t)voice < payloads.size()) payload = payloads[(size_t)voice];
        std::array<int, SYMBOLS_PER_FRAME> notes = detail::frameNotes(encodeFrame(payload));
        modulateVoice(notes, detail::voiceFrequencies(settings.scale, voice, settings.tuningOffsetHz), gear,
                      audio.data());
    }

    // Short raised cosine fade in and out (10 ms), which keeps the key-down
    // click off the air.
    const int ramp = SAMPLE_RATE_HZ / 100;
    for (size_t n = 0; n < length; n++)
    {
        double w = 1.0;
        if (n < (size_t)ramp)
            w = 0.5 - 0.5 * std::cos(PI * (double)n / ramp);
        else if (n >= length - ramp)
            w = 0.5 - 0.5 * std::cos(PI * (double)(length - 1 - n) / ramp);
        audio[n] = (float)(audio[n] / gear.voices * w);
    }
    return audio;
}

std::vector<Decode> receive(const float* audio, size_t numSamples, const ModemSettings& settings,
                            long long searchFrom, long long searchTo, double maxOffsetHz, int candidates)
{
    const GearInfo& gear = gearInfo(settings.gear);
    std::vector<Decode> decodes((size_t)gear.voices);
    for (int voice = 0; voice < gear.voices; voice++) decodes[(size_t)voice].voice = voice;
    if (audio == nullptr || numSamples == 0) return decodes;

    detail::ComplexSignal z;
    detail::analyticSignal(audio, numSamples, z);
    if (searchTo < 0) searchTo = (long long)numSamples;

    for (int voice = 0; voice < gear.voices; voice++)
    {
        auto templates = detail::voiceTemplates(settings.scale, voice, gear.number, settings.tuningOffsetHz);
        detail::VoiceDecode result =
            detail::receiveVoice(z, gear, *templates, searchFrom, searchTo, maxOffsetHz, candidates);
        decodes[(size_t)voice] = result.decode;
        decodes[(size_t)voice].voice = voice;
    }
    return decodes;
}

int recommendGear(double snrDb, double dopplerHz)
{
    // SNR (2500 Hz) for 90 % decode on the CCIR "moderate" path, from the
    // prototype's sweeps (DESIGN 7), plus a 2 dB margin; and the largest
    // symbol length x Doppler spread product a gear is used at. The sweeps
    // show G1 still beats G2 on the CCIR "poor" path (T x fd = 0.64), so the
    // Doppler limit only bites on flutter and auroral paths.
    const double threshold[MAX_GEAR - MIN_GEAR + 1] = {-22.2, -19.1, -15.5, -13.0, -7.8};
    const double maxSymbolDoppler = 1.0;
    const double marginDb = 2.0;

    for (int g = MAX_GEAR; g >= MIN_GEAR; g--)
    {
        if (snrDb >= threshold[g - MIN_GEAR] + marginDb && gearInfo(g).symbolSeconds * dopplerHz <= maxSymbolDoppler)
            return g;
    }

    // Nothing fits with margin: the slowest gear the Doppler spread allows,
    // and G3 when even the fastest is too slow (as the prototype does).
    for (int g = MIN_GEAR; g <= MAX_GEAR; g++)
    {
        if (gearInfo(g).symbolSeconds * dopplerHz <= maxSymbolDoppler) return g;
    }
    return 3;
}

} // namespace Glissando
