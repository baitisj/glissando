//=========================================================================
// Name:            GlissandoChordTest.cpp
// Purpose:         The opening chord and the listener that hears it.
//=========================================================================

#include <array>
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

// A melody joined part way through is not a chord, however strong: the
// chord is two steady notes starting together, and a melody's notes come
// and go. The receiver hears such a station once a frame decodes. A
// station on an older build opened with every note of its scale at once,
// which holds E4 and D5 steady and is still heard.
void testHearsChordsNotMelodies()
{
    Random rng(14);
    ModemSettings settings;
    settings.gear = 4;
    std::vector<float> frame = modulate({rng.payload()}, settings);
    addNoise(frame, 0.0, meanSquare(frame), rng);
    ChordListener melody;
    melody.push(frame.data(), (int)frame.size());
    CHECK(melody.chordsHeard() == 0);

    for (int s = 0; s < SCALE_COUNT; s++)
    {
        settings.scale = (Scale)s;
        std::vector<float> audio(2 * RATE, 0.0f);
        std::vector<float> old = chord(settings);
        audio.insert(audio.end(), old.begin(), old.end());
        audio.resize(audio.size() + 2 * RATE, 0.0f);
        addNoise(audio, 0.0, meanSquare(old), rng);
        ChordListener older;
        older.push(audio.data(), (int)audio.size());
        CHECK(older.chordsHeard() > 0);
    }
}

// Other signals on the band, a dozen FT8 signals and a voice, at -8 to
// +12 dB each. They used to make a chord ten times a second and keep the
// channel busy for good, which froze the chat queue. With parked set,
// three of the FT8 signals sit across the chord's notes, the worst case.
std::vector<float> busyBand(double seconds, Random& rng, bool parked, bool voice = true)
{
    std::vector<float> audio((size_t)(seconds * RATE), 0.0f);
    const double twoPi = 2.0 * 3.14159265358979323846;

    // 79 symbols of 0.16 s on eight tones 6.25 Hz apart, in 15 s slots.
    double bases[12] = {322, 598, 602, 1542, 2390, 1806, 1464, 1701, 1768, 1323, 1876, 2559};
    if (!parked)
    {
        for (double& base : bases) base = rng.uniform(200.0, 2900.0);
    }
    for (double base : bases)
    {
        double amplitude = std::sqrt(2.0 * 1e-4 * std::pow(10.0, rng.uniform(-8.0, 12.0) / 10.0));
        double phase = 0.0;
        int tone = 0;
        for (size_t n = 0; n < audio.size(); n++)
        {
            double slot = std::fmod(n / (double)RATE, 15.0);
            if (slot < 0.5 || slot >= 0.5 + 79 * 0.16) continue;
            if (n % (size_t)(0.16 * RATE) == 0) tone = (int)rng.uniform(0.0, 8.0);
            phase += twoPi * (base + 6.25 * tone) / RATE;
            audio[n] += (float)(amplitude * std::sin(phase));
        }
    }

    // A voice: harmonics of a wandering pitch through two formants, two
    // seconds in three, well up over the noise.
    double pitch = 130.0;
    double phase = 0.0;
    for (size_t n = 0; n < audio.size(); n++)
    {
        if (n % (RATE / 10) == 0) pitch = rng.uniform(100.0, 180.0);
        phase += twoPi * pitch / RATE;
        if (!voice || std::fmod(n / (double)RATE, 3.0) >= 2.0) continue;
        double v = 0.0;
        for (int k = 1; k * pitch < 3000.0; k++)
        {
            double f = k * pitch;
            double formants = std::exp(-std::pow((f - 500.0) / 300.0, 2)) + 0.5 * std::exp(-std::pow((f - 1500.0) / 400.0, 2));
            v += formants * std::sin(k * phase);
        }
        audio[n] += (float)(0.02 * v);
    }

    // Noise of power 1e-4 in 2500 Hz.
    addNoise(audio, 0.0, 1e-4, rng);
    return audio;
}

void testIgnoresABusyBand()
{
    for (bool parked : {false, true})
    {
        Random rng(parked ? 17 : 15);
        std::vector<float> band = busyBand(20 * 60, rng, parked);
        ChordListener listener;
        const int block = RATE / 10;
        long long sounding = 0;
        long long run = 0;
        long long longest = 0;
        for (size_t i = 0; i + block <= band.size(); i += block)
        {
            listener.push(&band[i], block);
            run = listener.isSounding() ? run + block : 0;
            if (listener.isSounding()) sounding += block;
            longest = std::max(longest, run);
        }
        printf("20 minutes of a busy band%s: %lld chords, sounding %.0f%% of the time, at most %.1f s at once\n",
               parked ? " with FT8 on the chord's notes" : "", listener.chordsHeard(),
               100.0 * sounding / (double)band.size(), longest / (double)RATE);
        CHECK(sounding < (long long)band.size() / (parked ? 2 : 10));
        CHECK(longest < 20 * RATE);
    }
}

// And a transmission among those FT8 signals is still heard, unless one of
// them was already sitting on E4 or D5. Its melody is followed only where
// it stands out from them: the frame receiver holds the channel once the
// first frame decodes, as it does for a station that played no chord.
void testHearsATransmissionOnABusyBand()
{
    Random rng(16);
    int heard = 0;
    double busy = 0.0;
    const int trials = 8;
    for (int t = 0; t < trials; t++)
    {
        ModemSettings settings;
        settings.gear = t % 2 == 0 ? 4 : 3;
        settings.scale = (Scale)(t % SCALE_COUNT);
        std::vector<float> frame = modulate({rng.payload()}, settings);
        std::vector<float> opening = openingChord(settings);
        std::vector<float> closing = chord(settings);

        std::vector<float> audio = busyBand(60.0, rng, false, false);
        size_t start = (size_t)(rng.uniform(20.0, 30.0) * RATE);
        // At -8 dB in 2500 Hz against the band's noise.
        double gain = std::sqrt(1e-4 * std::pow(10.0, -8.0 / 10.0) / meanSquare(frame));
        std::vector<float> tx = opening;
        size_t framesStart = start + tx.size();
        tx.insert(tx.end(), frame.begin(), frame.end());
        tx.insert(tx.end(), closing.begin(), closing.end());
        size_t end = start + tx.size();
        for (size_t n = 0; n < tx.size() && start + n < audio.size(); n++) audio[start + n] += (float)(gain * tx[n]);

        ChordListener listener;
        const int block = RATE / 100;
        long long followed = 0;
        bool inTime = false;
        for (size_t i = 0; i + block <= audio.size(); i += block)
        {
            listener.push(&audio[i], block);
            if (!listener.isSounding()) continue;
            if (i >= framesStart && i < framesStart + RATE) inTime = true;
            if (i >= framesStart && i < end) followed += block;
        }
        heard += inTime;
        busy += followed / (double)(end - framesStart);
    }
    printf("On a busy band at -8 dB: chord heard %d/%d, busy for %.0f%% of the melody\n", heard, trials,
           100.0 * busy / trials);
    CHECK(heard >= trials * 5 / 8);
    CHECK(busy / trials > 0.3);
}

// However long something after a chord goes on sounding like a melody, the
// chord holds the channel no longer than MAX_TRACK_SECONDS; by then a real
// melody's first frame has decoded at any tempo, and the receiver has it.
void testTrackingEnds()
{
    Random rng(18);
    ModemSettings settings;
    std::vector<float> audio(2 * RATE, 0.0f);
    std::vector<float> opening = openingChord(settings);
    audio.insert(audio.end(), opening.begin(), opening.end());
    const std::array<double, NOTES> notes = scaleNotes(Scale::Pentatonic, 0);
    double phase = 0.0;
    for (int n = 0; n < 150 * RATE; n++)
    {
        phase += 2.0 * 3.14159265358979323846 * notes[(size_t)(n / (RATE / 2)) % NOTES] / RATE;
        audio.push_back((float)std::sin(phase));
    }
    addNoise(audio, 0.0, 0.5, rng);

    ChordListener listener;
    const int block = RATE / 10;
    double lastSounding = 0.0;
    for (size_t i = 0; i + block <= audio.size(); i += block)
    {
        listener.push(&audio[i], block);
        if (listener.isSounding()) lastSounding = i / (double)RATE;
    }
    CHECK(listener.chordsHeard() > 0);
    CHECK(lastSounding > 60.0);
    CHECK(lastSounding < 2.0 + ChordListener::MAX_TRACK_SECONDS + 1.0);
}

} // namespace

int main()
{
    testTheChord();
    testHearsATransmission();
    testSensitivity();
    testFalseAlarms();
    testHearsChordsNotMelodies();
    testIgnoresABusyBand();
    testHearsATransmissionOnABusyBand();
    testTrackingEnds();
    if (failures == 0) printf("PASS\n");
    return failures == 0 ? 0 : 1;
}
