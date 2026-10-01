//=========================================================================
// Name:            GlissandoCwTest.cpp
// Purpose:         The CW Glorifier: Morse timing, the tune it sings, and
//                  that a tail never decodes as a frame.
//=========================================================================

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../GlissandoCw.h"
#include "GlissandoTestUtil.h"

using namespace Glissando;
using namespace GlissandoTest;

namespace
{

constexpr int RATE = SAMPLE_RATE_HZ;
constexpr Scale SCALES[] = {Scale::Pentatonic, Scale::WholeTone, Scale::Diminished, Scale::Diabolus};

int units(const std::vector<CwElement>& tune)
{
    int total = 0;
    for (const CwElement& element : tune) total += element.units;
    return total;
}

std::string degrees(const std::vector<CwElement>& tune)
{
    std::string out;
    for (const CwElement& element : tune)
    {
        if (element.note >= 0) out += (char)('0' + element.note);
    }
    return out;
}

// Power at hz over [from, to) of the audio, by Goertzel.
double power(const std::vector<float>& audio, size_t from, size_t to, double hz)
{
    double coeff = 2.0 * std::cos(2.0 * 3.14159265358979323846 * hz / RATE);
    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t n = from; n < to; n++)
    {
        double s = audio[n] + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

void testText()
{
    CHECK(std::string(morseFor('a')) == ".-");
    CHECK(std::string(morseFor('7')) == "--...");
    CHECK(morseFor('!') == nullptr);
    CHECK(cwSendable("  Glissando   de ag7ew! ") == "GLISSANDO DE AG7EW");
    CHECK(cwSendable("!!! ###").empty());
    CHECK(cwTune("", Scale::Pentatonic).empty());
    CHECK(cwTune("#", Scale::Pentatonic).empty());
}

void testTiming()
{
    // PARIS is 43 units plus the word space before it: the standard 50.
    std::vector<CwElement> paris = cwTune("PARIS", Scale::Pentatonic);
    CHECK(units(paris) == 50);
    CHECK(std::fabs(cwTailSeconds("paris", 20) - 3.0) < 1e-9);
    CHECK(std::fabs(cwTailSeconds("paris", 25) - 2.4) < 1e-9);
    CHECK(std::fabs(cwTailSeconds("paris", 100) - cwTailSeconds("paris", CW_MAX_WPM)) < 1e-9);

    // The rhythm is Morse's own: a dit is a unit and a dah three, a
    // sounding element never follows another without a gap.
    for (Scale scale : SCALES)
    {
        std::vector<CwElement> tune = cwTune("Glissando de AG7EW 0123456789 /?.,=+-@", scale);
        CHECK(tune.front().note < 0 && tune.front().units == 7);
        for (size_t i = 0; i < tune.size(); i++)
        {
            if (tune[i].note >= 0)
            {
                CHECK(tune[i].units == 1 || tune[i].units == 3);
                CHECK(tune[i].note < NOTES);
                if (i > 0) CHECK(tune[i - 1].note < 0);
            }
            else
            {
                CHECK(tune[i].units == 1 || tune[i].units == 3 || tune[i].units == 7);
            }
        }
    }

    CHECK(cwTailFits("Glissando de AG7EW", CW_DEFAULT_WPM));
    CHECK(cwTailFits("Glissando de VE3ABC/VE7", CW_DEFAULT_WPM));
    CHECK(!cwTailFits("Glissando de AG7EW, a melodic chirp mode for HF", CW_DEFAULT_WPM));
}

void testTune()
{
    // The sketch the tune rules were heard in (docs/CW_TAIL.md) sang this.
    CHECK(degrees(cwTune("de AG7EW Glissando", Scale::Pentatonic)) ==
          "3454353212345656750234567656765434532345322");

    const int home[] = {2, 0, 0, 0};
    const std::vector<std::vector<int>> chord = {{0, 2, 3, 5, 7}, {0, 2, 4, 6}, {0, 2, 4, 6}, {0, 1, 3, 5, 7}};
    for (int s = 0; s < 4; s++)
    {
        std::vector<CwElement> tune = cwTune("Glissando de AG7EW", SCALES[s]);
        CHECK(degrees(tune) == degrees(cwTune("Glissando de AG7EW", SCALES[s])));
        CHECK(degrees(tune).back() == '0' + home[s]);

        bool sawHigh = false;
        bool sawLow = false;
        for (const CwElement& element : tune)
        {
            if (element.note < 0) continue;
            sawHigh = sawHigh || element.note >= 5;
            sawLow = sawLow || element.note <= 2;
            // Dahs rest on the home chord (the last note is home, which is in it).
            if (element.units == 3)
            {
                bool inChord = false;
                for (int note : chord[(size_t)s]) inChord = inChord || note == element.note;
                CHECK(inChord);
            }
        }
        // A melody, not a drone.
        CHECK(sawHigh && sawLow);
    }
}

void testAudio()
{
    for (Scale scale : SCALES)
    {
        ModemSettings settings;
        settings.scale = scale;
        settings.tuningOffsetHz = 12.5;
        const std::string text = "Glissando de AG7EW";
        std::vector<float> audio = cwTail(text, 20, settings);
        CHECK(std::abs((long)audio.size() - std::lround(cwTailSeconds(text, 20) * RATE)) <= 1);

        float peak = 0.0f;
        for (float v : audio) peak = std::max(peak, std::fabs(v));
        CHECK(peak <= 1.0f && peak > 0.99f);

        // Silent in the gaps, and each element's note the loudest of the
        // scale once its glide is over.
        std::vector<CwElement> tune = cwTune(text, scale);
        std::array<double, NOTES> hz = scaleNotes(scale, 0);
        const double unit = 1.2 / 20 * RATE;
        int at = 0;
        for (const CwElement& element : tune)
        {
            size_t from = (size_t)std::lround(at * unit);
            at += element.units;
            size_t to = (size_t)std::lround(at * unit);
            if (element.note < 0)
            {
                float loudest = 0.0f;
                for (size_t n = from; n < to; n++) loudest = std::max(loudest, std::fabs(audio[n]));
                CHECK(loudest == 0.0f);
                continue;
            }
            size_t settled = from + (size_t)(0.025 * RATE);
            int best = 0;
            double bestPower = -1.0;
            for (int note = 0; note < NOTES; note++)
            {
                double p = power(audio, settled, to, hz[(size_t)note] + settings.tuningOffsetHz);
                if (p > bestPower)
                {
                    bestPower = p;
                    best = note;
                }
            }
            CHECK(best == element.note);
        }
    }

    ModemSettings settings;
    CHECK(cwTail("###", 20, settings).empty());
}

void testStraight()
{
    // The same rhythm as the glorified tail, on E4 and D5 together and
    // nothing else, in every scale and at the tuning offset.
    for (Scale scale : SCALES)
    {
        ModemSettings settings;
        settings.scale = scale;
        settings.tuningOffsetHz = -7.0;
        const std::string text = "Glissando de AG7EW";
        std::vector<float> straight = cwTail(text, 20, settings, CwStyle::Straight);
        std::vector<float> glorified = cwTail(text, 20, settings);
        CHECK(straight.size() == glorified.size());

        float peak = 0.0f;
        for (float v : straight) peak = std::max(peak, std::fabs(v));
        CHECK(peak <= 1.0f && peak > 0.99f);

        std::array<double, NOTES> hz = scaleNotes(scale, 0);
        const double low = CW_STRAIGHT_LOW_HZ + settings.tuningOffsetHz;
        const double high = CW_STRAIGHT_HIGH_HZ + settings.tuningOffsetHz;
        const double unit = 1.2 / 20 * RATE;
        int at = 0;
        for (const CwElement& element : cwTune(text, scale))
        {
            size_t from = (size_t)std::lround(at * unit);
            at += element.units;
            size_t to = (size_t)std::lround(at * unit);
            if (element.note < 0)
            {
                float loudest = 0.0f;
                for (size_t n = from; n < to; n++) loudest = std::max(loudest, std::fabs(straight[n]));
                CHECK(loudest == 0.0f);
                continue;
            }
            double lowPower = power(straight, from, to, low);
            double highPower = power(straight, from, to, high);
            CHECK(lowPower > 0.5 * highPower && highPower > 0.5 * lowPower);
            for (double note : hz)
            {
                note += settings.tuningOffsetHz;
                if (std::fabs(note - low) < 1.0 || std::fabs(note - high) < 1.0) continue;
                CHECK(power(straight, from, to, note) < 0.05 * lowPower);
            }
        }
    }
}

void testNeverAFrame()
{
    // Clean and noisy, the tail on its own never decodes, at any tempo and
    // in any scale.
    Random rng(20260930);
    int decodes = 0;
    for (Scale scale : SCALES)
    {
        ModemSettings settings;
        settings.scale = scale;
        for (CwStyle style : {CwStyle::Glorified, CwStyle::Straight})
        {
            std::vector<float> tail = cwTail("Glissando de AG7EW", 25, settings, style);
            for (int gear : {3, 4, 5})
            {
                ModemSettings rx;
                rx.gear = gear;
                rx.anyScale = true;
                size_t frame = (size_t)gearInfo(gear).frameSamples();
                for (double snrDb : {100.0, 0.0})
                {
                    std::vector<float> audio = pad(tail, RATE, frame);
                    if (snrDb < 100.0) addNoise(audio, snrDb, meanSquare(tail), rng);
                    for (const Decode& d :
                         receive(audio.data(), audio.size(), rx, 0, (long long)(audio.size() - frame)))
                    {
                        if (d.ok) decodes++;
                    }
                }
            }
        }
    }
    CHECK(decodes == 0);
}

} // namespace

int main()
{
    testText();
    testTiming();
    testTune();
    testAudio();
    testStraight();
    testNeverAFrame();
    if (failures == 0) printf("PASS\n");
    return failures == 0 ? 0 : 1;
}
