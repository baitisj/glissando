//=========================================================================
// Name:            GlissandoCw.cpp
// Purpose:         The CW Glorifier: Morse code sung on the notes of a
//                  scale.
//=========================================================================

#include "GlissandoCw.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace Glissando
{

namespace
{

constexpr double PI = 3.14159265358979323846;

// Timing in Morse units.
constexpr int DIT = 1;
constexpr int DAH = 3;
constexpr int ELEMENT_GAP = 1;
constexpr int LETTER_GAP = 3;
constexpr int WORD_GAP = 7;

// A dit lasts 1.2 s / WPM (the PARIS standard: 50 units a word).
constexpr double PARIS_SECONDS = 1.2;

constexpr double GLIDE_SECONDS = 0.020;
constexpr double RAMP_SECONDS = 0.005;

struct Code
{
    char c;
    const char* morse;
};

const Code MORSE[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},    {'E', "."},      {'F', "..-."},
    {'G', "--."},    {'H', "...."},   {'I', ".."},     {'J', ".---"},   {'K', "-.-"},    {'L', ".-.."},
    {'M', "--"},     {'N', "-."},     {'O', "---"},    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},
    {'S', "..."},    {'T', "-"},      {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},
    {'Y', "-.--"},   {'Z', "--.."},   {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},  {'6', "-...."},  {'7', "--..."},  {'8', "---.."},  {'9', "----."},
    {'/', "-..-."},  {'?', "..--.."}, {'.', ".-.-.-"}, {',', "--..--"}, {'=', "-...-"},  {'+', ".-.-."},
    {'-', "-....-"}, {'@', ".--.-."},
};

// Where a scale's tune starts and ends, and the notes of its home chord,
// where the dahs land. Degrees of the low voice, 0 lowest.
struct Harmony
{
    int home;
    std::vector<int> chord;
};

Harmony harmony(Scale scale)
{
    switch (scale)
    {
    case Scale::Pentatonic:
        // E4 G4 A4 C5 D5 E5 G5 A5: A minor, home on A4.
        return {2, {0, 2, 3, 5, 7}};
    case Scale::Diabolus:
        // E4 G#4 A#4 B4 D5 E5 F5 G#5: E major against B flat, home on E4.
        return {0, {0, 1, 3, 5, 7}};
    case Scale::WholeTone:
        // E4 F#4 G#4 A#4 C5 D5 E5 F#5: the augmented chord E G# C.
    case Scale::Diminished:
        // E4 F4 G4 G#4 A#4 B4 C#5 D5: the diminished seventh E G A# C#.
    default:
        return {0, {0, 2, 4, 6}};
    }
}

// The nearest chord note beyond pos in direction dir, turning around when
// there is none that way.
int nextChordNote(int pos, int& dir, const std::vector<int>& chord)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        int best = -1;
        for (int note : chord)
        {
            if ((note - pos) * dir > 0 && (best < 0 || std::abs(note - pos) < std::abs(best - pos))) best = note;
        }
        if (best >= 0) return best;
        dir = -dir;
    }
    return pos;
}

} // namespace

const char* morseFor(char c)
{
    char upper = (char)std::toupper((unsigned char)c);
    for (const Code& code : MORSE)
    {
        if (code.c == upper) return code.morse;
    }
    return nullptr;
}

std::string cwSendable(const std::string& text)
{
    std::string out;
    bool space = false;
    for (char c : text)
    {
        if (std::isspace((unsigned char)c))
        {
            space = !out.empty();
            continue;
        }
        if (morseFor(c) == nullptr) continue;
        if (space) out += ' ';
        space = false;
        out += (char)std::toupper((unsigned char)c);
    }
    return out;
}

std::vector<CwElement> cwTune(const std::string& text, Scale scale)
{
    const std::string sendable = cwSendable(text);
    std::vector<CwElement> tune;
    if (sendable.empty()) return tune;

    const Harmony h = harmony(scale);
    tune.push_back({WORD_GAP, -1});

    int pos = h.home;
    int letter = 0;
    bool wordStart = true;
    for (size_t i = 0; i < sendable.size(); i++)
    {
        char c = sendable[i];
        if (c == ' ')
        {
            tune.push_back({WORD_GAP, -1});
            pos = h.home;
            wordStart = true;
            continue;
        }
        if (!wordStart) tune.push_back({LETTER_GAP, -1});
        wordStart = false;

        const char* morse = morseFor(c);
        const int length = (int)std::char_traits<char>::length(morse);

        // Letters take turns rising and falling, and turn back rather than
        // run off either end of the eight notes.
        int dir = letter % 2 == 0 ? 1 : -1;
        letter++;
        if (pos + dir * length > NOTES || pos + dir * length < -1) dir = -dir;

        for (int e = 0; e < length; e++)
        {
            if (e > 0) tune.push_back({ELEMENT_GAP, -1});
            if (morse[e] == '.')
            {
                int next = pos + dir;
                if (next < 0 || next >= NOTES)
                {
                    dir = -dir;
                    next = pos + dir;
                }
                pos = next;
                tune.push_back({DIT, pos});
            }
            else
            {
                pos = nextChordNote(pos, dir, h.chord);
                tune.push_back({DAH, pos});
            }
        }
    }

    // Home at the end: a cadence, not a question.
    for (auto it = tune.rbegin(); it != tune.rend(); ++it)
    {
        if (it->note >= 0)
        {
            it->note = h.home;
            break;
        }
    }
    return tune;
}

double cwTailSeconds(const std::string& text, int wpm)
{
    wpm = std::min(std::max(wpm, CW_MIN_WPM), CW_MAX_WPM);
    int units = 0;
    for (const CwElement& element : cwTune(text, Scale::Pentatonic)) units += element.units;
    return units * PARIS_SECONDS / wpm;
}

bool cwTailFits(const std::string& text, int wpm)
{
    return cwTailSeconds(text, wpm) <= CW_TAIL_MAX_SECONDS;
}

std::vector<float> cwTail(const std::string& text, int wpm, const ModemSettings& settings)
{
    wpm = std::min(std::max(wpm, CW_MIN_WPM), CW_MAX_WPM);
    const std::vector<CwElement> tune = cwTune(text, settings.scale);
    std::vector<float> audio;
    if (tune.empty()) return audio;

    std::array<double, NOTES> hz = scaleNotes(settings.scale, 0);
    for (double& note : hz) note += settings.tuningOffsetHz;

    const double unitSamples = PARIS_SECONDS / wpm * SAMPLE_RATE_HZ;
    const int glide = (int)std::lround(GLIDE_SECONDS * SAMPLE_RATE_HZ);
    const int ramp = (int)std::lround(RAMP_SECONDS * SAMPLE_RATE_HZ);

    // Element edges fall on the nearest sample to the exact unit grid, so
    // rounding never builds up over a long tail.
    int units = 0;
    int previous = -1;
    double phase = 0.0;
    for (const CwElement& element : tune)
    {
        const size_t start = (size_t)std::lround(units * unitSamples);
        units += element.units;
        const size_t end = (size_t)std::lround(units * unitSamples);
        const int length = (int)(end - start);
        if (element.note < 0)
        {
            audio.resize(end, 0.0f);
            continue;
        }

        const double to = hz[(size_t)element.note];
        const double from = previous >= 0 ? hz[(size_t)previous] : to;
        for (int n = 0; n < length; n++)
        {
            // A glide in from the last note, linear in log frequency with a
            // raised cosine ease like a frame's, then the note held.
            double f = to;
            if (n < glide)
            {
                double ease = 0.5 - 0.5 * std::cos(PI * n / glide);
                f = from * std::pow(to / from, ease);
            }
            phase += 2.0 * PI * f / SAMPLE_RATE_HZ;
            double w = 1.0;
            if (n < ramp)
                w = 0.5 - 0.5 * std::cos(PI * n / ramp);
            else if (n >= length - ramp)
                w = 0.5 - 0.5 * std::cos(PI * (length - 1 - n) / ramp);
            audio.push_back((float)(w * std::sin(phase)));
        }
        previous = element.note;
    }
    return audio;
}

} // namespace Glissando
