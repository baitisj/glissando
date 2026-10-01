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

private:
    void analyse();
    double noiseLevel();
    double combPower(const std::array<double, NOTES>& notes, double offsetHz) const;

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
    long long lastSoundedSample_ = 0;
    std::atomic<bool> sounding_{false};
};

} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_CHORD_H
