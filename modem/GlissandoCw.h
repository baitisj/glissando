//=========================================================================
// Name:            GlissandoCw.h
// Purpose:         The CW Glorifier: Morse code sung on the notes of a
//                  scale, for the tail that ends a transmission.
//
// A casual listener who hears Glissando on the air has nothing to search
// for. The tail spells it out in Morse ("Gliss de AG7EW" by default),
// which also identifies the station in plain CW. The rhythm is standard
// Morse, so it copies by ear as usual; only the pitch of each dit and dah
// moves, following the rules long notes follow in a melody:
//
//   - a dah leaps to the next tone of the scale's home chord;
//   - a dit steps to the neighbouring note, a passing tone;
//   - letters take turns rising and falling, turning back at the ends of
//     the range, so a word sounds like a phrase;
//   - each word starts on the home note and the last note lands on it.
//
// Each note glides in from the one before, the Glissando swoop, and fades
// in and out over 5 ms so the keying makes no clicks. See docs/CW_TAIL.md.
//=========================================================================

#ifndef GLISSANDO__GLISSANDO_CW_H
#define GLISSANDO__GLISSANDO_CW_H

#include <string>
#include <vector>

#include "GlissandoModem.h"

namespace Glissando
{

constexpr int CW_MIN_WPM = 10;
constexpr int CW_MAX_WPM = 40;
constexpr int CW_DEFAULT_WPM = 20;

// Longer tails are not sent (the chord closes the keying instead): the
// transmit time-out split leaves 20 s spare at the end of a keying.
constexpr double CW_TAIL_MAX_SECONDS = 15.0;

// Morse for one character, as '.' and '-', or nullptr for none. Letters
// in either case.
const char* morseFor(char c);

// The text as the tail sends it: upper case, characters Morse has no code
// for dropped, runs of spaces folded to one, no leading or trailing space.
std::string cwSendable(const std::string& text);

// One dit, dah or silence of the tail, in Morse units (a dit is one unit;
// PARIS is 50). note is the scale degree (0 lowest .. 7 highest) for a dit
// or dah, and -1 for a silence.
struct CwElement
{
    int units = 0;
    int note = -1;
};

// The glorified tune of cwSendable(text) in this scale, starting with the
// word space that separates it from the frames before it. Empty when there
// is nothing to send. The same text and scale always give the same tune.
std::vector<CwElement> cwTune(const std::string& text, Scale scale);

// Length of the tail at this speed, in seconds (the leading word space
// included), and whether it fits in CW_TAIL_MAX_SECONDS.
double cwTailSeconds(const std::string& text, int wpm);
bool cwTailFits(const std::string& text, int wpm);

// How the tail is sung: the Glorifier's tune, or straight Morse keyed on
// E4 and D5 together, the opening chord's two notes, which every scale
// shares and which a CW decoder tuned to either note can copy.
enum class CwStyle
{
    Glorified,
    Straight,
};

// The tail's audio at SAMPLE_RATE_HZ, tuning offset included, peak
// amplitude 1 like a frame. Glorified, it is sung on the low voice of the
// scale. Speed is clamped to CW_MIN_WPM .. CW_MAX_WPM.
std::vector<float> cwTail(const std::string& text, int wpm, const ModemSettings& settings,
                          CwStyle style = CwStyle::Glorified);

// The two notes of the straight tail, before any tuning offset.
constexpr double CW_STRAIGHT_LOW_HZ = 329.63;   // E4
constexpr double CW_STRAIGHT_HIGH_HZ = 587.33;  // D5

} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_CW_H
