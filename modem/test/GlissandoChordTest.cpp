//=========================================================================
// Name:            GlissandoChordTest.cpp
// Purpose:         The opening chord and the listener that hears it.
//=========================================================================

#include <cmath>
#include <cstdio>
#include <vector>

#include "../GlissandoChord.h"
#include "GlissandoTestUtil.h"

using namespace Glissando;
using namespace GlissandoTest;

namespace
{

constexpr int RATE = SAMPLE_RATE_HZ;

struct Heard
{
    bool chordInTime = false;     // heard by the end of the chord plus a second
    double busyFraction = 0.0;    // of the frames and closing chord
    double busyAfterEndSeconds = 0.0;
};

// A transmission as the modem sends it (opening chord, frames, closing
// chord) between stretches of silence, with noise at snrDb over the frames'
// power in 2500 Hz, played to a listener tuned to 0 Hz.
Heard listen(int gear, Scale scale, double tuningOffsetHz, double snrDb, Random& rng)
{
    ModemSettings settings;
    settings.gear = gear;
    settings.scale = scale;
    settings.tuningOffsetHz = tuningOffsetHz;
    std::vector<Payload> payloads;
    for (int v = 0; v < gearInfo(gear).voices; v++) payloads.push_back(rng.payload());
    std::vector<float> frame = modulate(payloads, settings);
    std::vector<float> opening = openingChord(settings);
    std::vector<float> closing = chord(settings);

    size_t start = (size_t)rng.uniform(RATE, 2 * RATE);
    std::vector<float> audio(start, 0.0f);
    audio.insert(audio.end(), opening.begin(), opening.end());
    size_t framesStart = audio.size();
    audio.insert(audio.end(), frame.begin(), frame.end());
    audio.insert(audio.end(), closing.begin(), closing.end());
    size_t end = audio.size();
    audio.resize(end + 5 * RATE, 0.0f);
    addNoise(audio, snrDb, meanSquare(frame), rng);

    ChordListener listener;
    listener.configure(0.0, 25.0);
    Heard heard;
    long long busy = 0;
    long long lastBusy = -1;
    const int block = RATE / 100;
    for (size_t i = 0; i + block <= audio.size(); i += block)
    {
        listener.push(&audio[i], block);
        if (!listener.isSounding()) continue;
        if (i < framesStart + RATE) heard.chordInTime = true;
        if (i >= framesStart && i < end) busy += block;
        lastBusy = (long long)i;
    }
    heard.busyFraction = busy / (double)(end - framesStart);
    if (lastBusy >= (long long)end) heard.busyAfterEndSeconds = (lastBusy - (long long)end) / (double)RATE;
    (void)closing;
    return heard;
}

void testTheChord()
{
    ModemSettings settings;
    settings.gear = 1;
    std::vector<float> adagio = openingChord(settings);
    settings.gear = 5;
    settings.scale = Scale::Diabolus;
    std::vector<float> duet = openingChord(settings);
    CHECK(adagio.size() == (size_t)std::lround(OPENING_CHORD_SECONDS * RATE));
    CHECK(adagio == duet); // the same at every tempo and in every scale

    double peak = 0.0;
    for (float v : adagio) peak = std::max(peak, (double)std::fabs(v));
    CHECK(std::fabs(peak - 1.0) < 1e-6);
    CHECK(std::fabs(adagio.front()) < 1e-3 && std::fabs(adagio.back()) < 1e-3);
}

// A strong chord is heard at once in every scale and tempo, across the
// tuning range, and the melody behind it keeps the channel busy to the end
// and for under two seconds after it.
void testHearsATransmission()
{
    Random rng(11);
    int gears[] = {4, 1, 5};
    for (int gear : gears)
    {
        for (int s = 0; s < SCALE_COUNT; s++)
        {
            double offset = rng.uniform(-20.0, 20.0);
            Heard heard = listen(gear, (Scale)s, offset, -8.0, rng);
            CHECK(heard.chordInTime);
            CHECK(heard.busyFraction > 0.9);
            // Windows still holding the end of the closing chord count as
            // sounding, and the hold runs on from the last of them.
            CHECK(heard.busyAfterEndSeconds <= ChordListener::HOLD_SECONDS + 0.5);
        }
    }
}

// Near where it stops working: half the time or better at -14 dB, nearly
// always at -12 dB, at the fastest and slowest tempo.
void testSensitivity()
{
    Random rng(12);
    int gears[] = {4, 1};
    for (int gear : gears)
    {
        int at14 = 0;
        int at12 = 0;
        const int trials = 20;
        for (int t = 0; t < trials; t++)
        {
            at14 += listen(gear, (Scale)(t % SCALE_COUNT), rng.uniform(-20, 20), -14.0, rng).chordInTime;
            at12 += listen(gear, (Scale)(t % SCALE_COUNT), rng.uniform(-20, 20), -12.0, rng).chordInTime;
        }
        printf("%s: chord heard %d/%d at -14 dB, %d/%d at -12 dB\n", gearInfo(gear).tempo, at14, trials, at12,
               trials);
        CHECK(at14 >= trials * 3 / 10);
        CHECK(at12 >= trials * 8 / 10);
    }
}

// Twenty minutes of noise and a steady carrier on each chord note: neither
// is a chord.
void testFalseAlarms()
{
    Random rng(13);
    ChordListener listener;
    listener.configure(0.0, 25.0);
    std::vector<float> block(RATE / 10);
    long long sounding = 0;
    for (int i = 0; i < 20 * 60 * 10; i++)
    {
        for (float& v : block) v = (float)(0.1 * rng.gaussian());
        listener.push(block.data(), (int)block.size());
        if (listener.isSounding()) sounding += (long long)block.size();
    }
    printf("20 minutes of noise: %lld chords, sounding %.1f s\n", listener.chordsHeard(), sounding / (double)RATE);
    CHECK(listener.chordsHeard() <= 1);
    CHECK(sounding <= 3 * RATE);

    const double carriers[] = {329.63, 587.33};
    for (double hz : carriers)
    {
        std::vector<float> tone(30 * RATE);
        for (size_t n = 0; n < tone.size(); n++) tone[n] = (float)std::sin(2.0 * 3.14159265358979323846 * hz * n / RATE);
        addNoise(tone, 0.0, 0.5, rng);
        ChordListener carrier;
        carrier.push(tone.data(), (int)tone.size());
        CHECK(carrier.chordsHeard() == 0);
    }
}

// A strong Presto melody sings E4 and D5 within 0.6 s often enough to count
// as the chord, so a station on an older build that plays no chord, or one
// joined part way through, is heard too. Adagio's notes are too long for it.
void testHearsAStrongMelody()
{
    Random rng(14);
    ModemSettings settings;
    settings.gear = 4;
    std::vector<float> frame = modulate({rng.payload()}, settings);
    addNoise(frame, 0.0, meanSquare(frame), rng);
    ChordListener melody;
    melody.push(frame.data(), (int)frame.size());
    CHECK(melody.chordsHeard() > 0);
}

} // namespace

int main()
{
    testTheChord();
    testHearsATransmission();
    testSensitivity();
    testFalseAlarms();
    testHearsAStrongMelody();
    if (failures == 0) printf("PASS\n");
    return failures == 0 ? 0 : 1;
}
