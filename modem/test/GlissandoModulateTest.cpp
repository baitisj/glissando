//=========================================================================
// Name:            GlissandoModulateTest.cpp
// Purpose:         Pins the Glissando transmitter to the prototype's
//                  glissando.transmit(), and checks gears, scales and the
//                  tuning offset.
//=========================================================================

#include <algorithm>
#include <cmath>

#include "../GlissandoInternal.h"
#include "GlissandoTestUtil.h"
#include "GlissandoVectors.h"

using namespace Glissando;
using namespace GlissandoTest;

namespace
{

Payload payloadFromString(const char* bits)
{
    Payload p{};
    for (int i = 0; i < PAYLOAD_BITS; i++) p[i] = (uint8_t)(bits[i] == '1');
    return p;
}

Payload namedPayload(const char* name)
{
    for (const auto& v : GlissandoVectors::FEC)
    {
        if (std::string(v.name) == name) return payloadFromString(v.payload);
    }
    return Payload{};
}

void compare(const std::vector<float>& x, int length, double rms, double mean, const float* spread, size_t spreadCount,
             const char* what)
{
    CHECK((int)x.size() == length);
    if ((int)x.size() != length) return;

    double worst = 0.0;
    size_t checked = 0;
    for (size_t i = 0; i < spreadCount; i++)
    {
        size_t n = i * GlissandoVectors::SPREAD_STRIDE;
        worst = std::max(worst, std::fabs((double)x[n] - spread[i]));
        checked++;
    }
    double sum = 0.0;
    for (float v : x) sum += v;
    double ourRms = std::sqrt(meanSquare(x));
    printf("%s: %zu samples compared, max |error| %.2e, RMS %.9f (prototype %.9f), mean %.2e\n", what, checked,
           worst, ourRms, rms, sum / (double)x.size());
    CHECK(worst < 1e-3);
    CHECK(std::fabs(ourRms - rms) < 1e-6);
    CHECK(std::fabs(sum / (double)x.size() - mean) < 1e-5);
}

void testAgainstPrototype()
{
    ModemSettings settings;
    settings.gear = 4;
    std::vector<float> g4 = modulate({namedPayload("random1")}, settings);
    compare(g4, GlissandoVectors::G4_LENGTH, GlissandoVectors::G4_RMS, GlissandoVectors::G4_MEAN,
            GlissandoVectors::G4_SPREAD, sizeof(GlissandoVectors::G4_SPREAD) / sizeof(float), "G4");

    settings.gear = 5;
    std::vector<float> g5 = modulate({namedPayload("random1"), namedPayload("random2")}, settings);
    compare(g5, GlissandoVectors::G5_LENGTH, GlissandoVectors::G5_RMS, GlissandoVectors::G5_MEAN,
            GlissandoVectors::G5_SPREAD, sizeof(GlissandoVectors::G5_SPREAD) / sizeof(float), "G5");
}

void testGears()
{
    const int expectedL[] = {5120, 2560, 1280, 640, 640};
    for (int g = MIN_GEAR; g <= MAX_GEAR; g++)
    {
        const GearInfo& info = gearInfo(g);
        CHECK(info.number == g);
        CHECK(info.samplesPerSymbol() == expectedL[g - 1]);
        CHECK(info.frameSamples() == SYMBOLS_PER_FRAME * expectedL[g - 1]);
        CHECK(info.voices == (g == 5 ? 2 : 1));
    }
    CHECK(gearInfo(0).number == 1);
    CHECK(gearInfo(9).number == 5);
    CHECK(std::fabs(gearInfo(1).frameSeconds() - 55.04) < 1e-9);

    // prototype recommend_gear()
    CHECK(recommendGear(-5.0, 0.1) == 5);
    CHECK(recommendGear(-10.9, 0.1) == 4);
    CHECK(recommendGear(-13.0, 0.5) == 3);
    CHECK(recommendGear(-17.0, 0.5) == 2);
    CHECK(recommendGear(-30.0, 0.5) == 1);
    CHECK(recommendGear(-30.0, 2.0) == 2);   // G1 at 0.64 s x 2 Hz is past the limit
    CHECK(recommendGear(-5.0, 20.0) == 3);   // nothing fits the Doppler spread
}

void testScales()
{
    Scale s;
    for (int i = 0; i < SCALE_COUNT; i++)
    {
        CHECK(scaleFromName(scaleName((Scale)i), s) && s == (Scale)i);
    }
    CHECK(!scaleFromName("chromatic", s));
    CHECK(std::fabs(scaleNotes(Scale::Pentatonic, 0)[2] - 440.0) < 1e-9);
    CHECK(std::fabs(scaleNotes(Scale::Pentatonic, 1)[7] - 2637.02) < 1e-9);
    CHECK(std::fabs(scaleNotes(Scale::WholeTone, 0)[0] - 329.6275569) < 1e-6);   // E4
    CHECK(std::fabs(scaleNotes(Scale::Diabolus, 1)[7] - 2349.3181433) < 1e-6);   // D7
    // Every tritone scale's high voice is its low voice 18 semitones up.
    for (Scale scale : {Scale::WholeTone, Scale::Diminished, Scale::Diabolus})
    {
        for (int i = 0; i < NOTES; i++)
        {
            CHECK(std::fabs(scaleNotes(scale, 1)[i] / scaleNotes(scale, 0)[i] - std::pow(2.0, 1.5)) < 1e-9);
        }
    }
}

// The tuning offset moves every note by the same number of Hz: the
// instantaneous frequency of a sustained note, from the phase step of the
// analytic signal, lands on note + offset.
void testTuningOffset()
{
    Payload payload{};
    ModemSettings settings;
    settings.gear = 3;
    settings.tuningOffsetHz = 123.0;
    std::vector<float> x = modulate({payload}, settings);

    detail::ComplexSignal z;
    detail::analyticSignal(x.data(), x.size(), z);
    const int L = gearInfo(3).samplesPerSymbol();
    // Symbol 0 is COSTAS7[0] held from the start: measure the middle of its sustain.
    double sumRe = 0.0, sumIm = 0.0;
    for (int n = L / 2; n < L - 10; n++)
    {
        // z[n+1] * conj(z[n])
        sumRe += (double)z.re[n + 1] * z.re[n] + (double)z.im[n + 1] * z.im[n];
        sumIm += (double)z.im[n + 1] * z.re[n] - (double)z.re[n + 1] * z.im[n];
    }
    double f = std::atan2(sumIm, sumRe) * SAMPLE_RATE_HZ / (2.0 * 3.14159265358979323846);
    double expected = scaleNotes(Scale::Pentatonic, 0)[detail::COSTAS7[0]] + 123.0;
    printf("tuning offset: sustain at %.3f Hz, expected %.3f Hz\n", f, expected);
    CHECK(std::fabs(f - expected) < 0.05);

    // Constant envelope: every sample within the fade is on the unit circle.
    double worst = 0.0;
    for (size_t n = 200; n + 200 < x.size(); n++)
    {
        worst = std::max(worst, std::fabs(std::hypot(z.re[n], z.im[n]) - 1.0));
    }
    CHECK(worst < 1e-2);
}

} // namespace

int main()
{
    testAgainstPrototype();
    testGears();
    testScales();
    testTuningOffset();
    if (failures == 0) printf("glissando modulation tests passed\n");
    return failures == 0 ? 0 : 1;
}
