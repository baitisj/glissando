//=========================================================================
// Name:            GlissandoChord.h
// Purpose:         The opening chord, and a listener that hears it: fast
//                  carrier sense for the chat protocol.
//
// A Glissando transmission can only be sensed once its first frame has
// decoded: about 10 s in at Presto and 70 s at Adagio. Until then a second
// station can key over it. The opening chord is a fixed 0.6 s minor seventh,
// E4 and D5, the two notes every scale shares, and the listener looks for it
// across the tuning range, so it hears a transmission start within a second
// at any tempo and in any scale. After the chord, the notes of whichever
// scale is singing at that tuning keep the channel marked busy for as long
// as they go on sounding.
//
// Two notes rather than all eight: the chord's power is capped by the
// transmitter's peak, and the notes share it. At 0.6 s, with one false
// chord in four hours of noise, the listener hears this chord half the time
// at about -14 dB SNR (a frame's power in 2500 Hz); every note of the scale
// needed -8 dB, and a three note chord -11.5 dB. See docs/CHORDS.md.
//=========================================================================

#ifndef GLISSANDO__GLISSANDO_CHORD_H
#define GLISSANDO__GLISSANDO_CHORD_H

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "GlissandoFft.h"
#include "GlissandoModem.h"

namespace Glissando
{

constexpr double OPENING_CHORD_SECONDS = 0.6;

// The opening chord of a transmission in these settings: E4 and D5, tuning
// offset included, for OPENING_CHORD_SECONDS at any gear and in any scale,
// peak amplitude 1.
std::vector<float> openingChord(const ModemSettings& settings);

// Listens for opening chords on the receive audio. Not thread safe, except
// isSounding(), which any thread may call.
class ChordListener
{
public:
    ChordListener();

    // Where to listen: every opening chord within maxOffsetHz of the tuning
    // offset. Takes effect at once; anything sounding is forgotten.
    void configure(double tuningOffsetHz, double maxOffsetHz = 25.0);

    // Audio at SAMPLE_RATE_HZ, as it arrives.
    void push(const short* samples, int numSamples);
    void push(const float* samples, int numSamples);

    // Forget the audio heard so far and anything sounding, e.g. after our
    // own transmission.
    void reset();

    // A frame of a melody has just decoded: whatever chord is being followed
    // opened a real keying, so MAX_TRACK_SECONDS counts again from here.
    // Without this a keying longer than that, as one at Allegro and slower
    // often is, stopped being followed before its closing chord or CW tail,
    // which no frame decode covers, and a reply keyed over the tail.
    void heardFrame();

    // An opening chord has been heard and the notes of its scale, at its
    // tuning, have sounded within the last HOLD. Safe from any thread.
    bool isSounding() const { return sounding_.load(std::memory_order_acquire); }

    struct Heard
    {
        double offsetHz = 0.0;      // tuning offset the chord was heard at
        double strength = 0.0;      // the weaker note's power over the noise per bin
        long long endSample = 0;    // of the window it was heard in, since construction or reset
    };

    // Opening chords heard since construction or reset, and the last one.
    long long chordsHeard() const { return chordsHeard_; }
    Heard lastChord() const { return last_; }

    long long samplesReceived() const { return samples_; }

    // Detection thresholds, from noise alone; see the .cpp.
    static constexpr double CHORD_THRESHOLD = 8.0;

    static constexpr double SOUNDING_THRESHOLD = 26.0;
    // Longer than the 2 s a long keying lets the radio up for, before the
    // time-out timer, so the channel stays taken across it.
    static constexpr double HOLD_SECONDS = 2.5;

    // A chord's notes must have been this far below it just before it, as
    // a fraction of its weaker note: a chord starts, it is not something
    // that was already there.
    static constexpr double ONSET_RATIO = 0.5;

    // And each note must stand this far over the bins a few hertz either
    // side: a steady tone, not one wandering through.
    static constexpr double PURITY = 4.0;
    static constexpr double PURITY_FROM_HZ = 4.0;
    static constexpr double PURITY_TO_HZ = 10.0;

    // And sound in both halves of the window, each with at least this share
    // of what a tone held for the whole of it would have there.
    static constexpr double STEADY_SHARE = 0.2;

    // While the melody is followed, its scale's notes must stand this far
    // over what they usually were before the chord (the 90th percentile of
    // the last BEFORE_SECONDS), so signals that were already on the channel
    // cannot keep it busy.
    static constexpr double OVER_BEFORE = 1.5;
    static constexpr double BEFORE_SECONDS = 15.0;
    static constexpr double BEFORE_PERCENTILE = 0.9;

    // The longest a chord holds the channel on its own: until the first
    // frame of an Adagio melody after it has decoded, which renews it (see
    // heardFrame()). A chord the band made up costs no more than this.
    static constexpr double MAX_TRACK_SECONDS = 75.0;

private:
    void analyse();
    double noiseLevel();
    void remember(bool silence);
    void rememberCombs(bool silence);
    double usualComb(size_t scale, int step);
    double purity(size_t bin) const;
    bool steady(size_t bin) const;
    static double combPower(const std::vector<double>& power, const std::array<double, NOTES>& notes,
                            double offsetHz);

    Fft fft_;
    std::vector<float> window_;     // the last WINDOW samples, oldest first
    std::vector<double> work_;
    std::vector<double> power_;     // per bin, of the last window
    std::vector<double> band_;
    int fill_ = 0;                  // samples in window_ so far
    int sinceHop_ = 0;
    long long samples_ = 0;

    double tuningOffsetHz_ = 0.0;
    double maxOffsetHz_ = 25.0;

    Heard last_;
    long long chordsHeard_ = 0;
    bool tracking_ = false;
    std::array<double, SCALE_COUNT> scaleEnergy_{}; // each scale's notes since the chord
    std::array<double, SCALE_COUNT> before_{};      // and their usual comb before it
    long long chordStartSample_ = 0;                // of the chord that started tracking, or the last frame heard since

    // The last few windows' power per bin over their noise, oldest at
    // historyNext_: the one a full window before the current one is what
    // was on the channel just before a chord heard in it.
    std::vector<std::vector<double>> history_;
    size_t historyNext_ = 0;
    size_t historyCount_ = 0;
    std::vector<double> normalized_;

    // Each scale's comb at each tuning step, per window, for BEFORE_SECONDS.
    int steps_ = 0;                 // either side of the tuning offset
    std::vector<float> combs_;      // [window][scale][step]
    size_t combsNext_ = 0;
    size_t combsCount_ = 0;
    std::vector<float> sorted_;
    long long lastSoundedSample_ = 0;
    std::atomic<bool> sounding_{false};
};

} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_CHORD_H
