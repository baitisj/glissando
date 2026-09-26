//=========================================================================
// Name:            GlissandoDecodeTest.cpp
// Purpose:         Blind decoding with the batch receiver: every gear in
//                  AWGN a few dB above the prototype's measured thresholds,
//                  every scale, a tuning offset, a clip made by the
//                  prototype, noise alone, and a noiseless frame between
//                  stretches of digital silence.
//=========================================================================

#include <cmath>
#include <fstream>

#include "GlissandoTestUtil.h"
#include "GlissandoVectors.h"

using namespace Glissando;
using namespace GlissandoTest;

namespace
{

// The prototype's measured 50 % decode thresholds in AWGN (DESIGN 7), dB in
// 2500 Hz. The tests run 3 dB above them, where the prototype decodes
// nearly every frame.
const double THRESHOLD_50_DB[MAX_GEAR] = {-26.5, -23.6, -20.4, -17.4, -14.1};
constexpr double MARGIN_DB = 3.0;

// As prototype/sim.py: the frame starts 0.3 to 1.2 s into a buffer that
// runs 1.5 s past the frame's latest start, with a +/-15 Hz offset; the
// receiver searches the first 1.5 s and +/-25 Hz.
constexpr double SEARCH_SECONDS = 1.5;

struct Trial
{
    bool decoded = false;   // every voice came back exact
    bool falseDecode = false;
    double seconds = 0.0;
    std::vector<Decode> decodes;
};

Trial runTrial(int gear, Scale scale, double snrDb, uint64_t seed, double txTuning = 0.0, double rxTuning = 0.0)
{
    Random rng(seed);
    const GearInfo& info = gearInfo(gear);
    std::vector<Payload> payloads;
    for (int v = 0; v < info.voices; v++) payloads.push_back(rng.payload());

    ModemSettings tx;
    tx.gear = gear;
    tx.scale = scale;
    tx.tuningOffsetHz = txTuning;
    std::vector<float> x = modulate(payloads, tx);

    // Lead-in, then a frequency shift (a mistuned receiver), then noise, as
    // prototype/sim.py.
    size_t lead = (size_t)(rng.uniform(0.3, 1.2) * SAMPLE_RATE_HZ);
    double offset = rng.uniform(-15.0, 15.0);
    std::vector<float> buffer = pad(x, lead, (size_t)(SEARCH_SECONDS * SAMPLE_RATE_HZ) - lead);
    frequencyShift(buffer, offset);
    if (!std::isnan(snrDb)) addNoise(buffer, snrDb, meanSquare(x), rng);

    ModemSettings rx = tx;
    rx.tuningOffsetHz = rxTuning;
    auto start = std::chrono::steady_clock::now();
    Trial trial;
    trial.decodes = receive(buffer.data(), buffer.size(), rx, 0, (long long)(SEARCH_SECONDS * SAMPLE_RATE_HZ));
    trial.seconds = secondsSince(start);

    trial.decoded = trial.decodes.size() == payloads.size();
    for (size_t v = 0; v < trial.decodes.size() && v < payloads.size(); v++)
    {
        const Decode& d = trial.decodes[v];
        bool right = d.ok && d.payload == payloads[v];
        if (!right) trial.decoded = false;
        if (d.ok && d.payload != payloads[v]) trial.falseDecode = true;
        if (right)
        {
            // The frame is found to within a millisecond (a glide is a chirp,
            // and chirps time sharply) and the offset to within a few steps
            // of the fine frequency search (1/16 of the symbol rate); near
            // threshold the prototype is off by up to 1.6 Hz at G4 too.
            long long timing = d.startSample - (long long)lead;
            double offsetError = d.frequencyOffsetHz - (offset + txTuning - rxTuning);
            if (std::llabs(timing) > 8 || std::fabs(offsetError) > 0.25 / info.symbolSeconds + 0.2)
                printf("  G%d voice %zu: timing error %lld samples, offset error %.3f Hz\n", gear, v, timing,
                       offsetError);
            CHECK(std::llabs(timing) <= 8);
            CHECK(std::fabs(offsetError) < 0.25 / info.symbolSeconds + 0.2);
        }
    }
    return trial;
}

void testClean()
{
    for (int gear = MIN_GEAR; gear <= MAX_GEAR; gear++)
    {
        Trial t = runTrial(gear, Scale::Pentatonic, NAN, 100 + gear);
        printf("G%d clean: %s in %.3f s, SNR estimate %.1f dB, Doppler %.2f Hz\n", gear,
               t.decoded ? "decoded" : "MISSED", t.seconds, t.decodes[0].report.snrDb,
               t.decodes[0].report.dopplerHz);
        CHECK(t.decoded);
        CHECK(!t.falseDecode);
        // No fading: the sounder should see (almost) no Doppler spread.
        if (t.decoded) CHECK(t.decodes[0].report.dopplerHz < 0.5);
    }
}

void testAwgn()
{
    // More trials at the fast gears, where they are cheap.
    const int trials[MAX_GEAR] = {1, 2, 3, 4, 3};
    for (int gear = MIN_GEAR; gear <= MAX_GEAR; gear++)
    {
        double snr = THRESHOLD_50_DB[gear - 1] + MARGIN_DB;
        int decoded = 0;
        double seconds = 0.0;
        double snrEstimate = 0.0;
        for (int i = 0; i < trials[gear - 1]; i++)
        {
            Trial t = runTrial(gear, Scale::Pentatonic, snr, 1000 * gear + i);
            decoded += t.decoded;
            seconds += t.seconds;
            snrEstimate += t.decodes[0].report.snrDb;
            CHECK(!t.falseDecode);
        }
        int n = trials[gear - 1];
        printf("G%d at %.1f dB: %d/%d decoded, %.3f s per receive, mean SNR estimate %.1f dB\n", gear, snr, decoded, n,
               seconds / n, snrEstimate / n);
        // Near threshold a miss is possible, if rare; allow one in three or more.
        CHECK(decoded >= (n >= 3 ? n - 1 : n));
    }
}

void testScales()
{
    for (int s = 0; s < SCALE_COUNT; s++)
    {
        int decoded = 0;
        for (int i = 0; i < 2; i++)
        {
            Trial t = runTrial(4, (Scale)s, THRESHOLD_50_DB[3] + MARGIN_DB, 5000 + 10 * s + i);
            decoded += t.decoded;
            CHECK(!t.falseDecode);
        }
        printf("G4 %s: %d/2 decoded\n", scaleName((Scale)s), decoded);
        CHECK(decoded == 2);
    }
    // The duet in a tritone scale.
    Trial t = runTrial(5, Scale::Diminished, -8.0, 5100);
    CHECK(t.decoded);
}

void testTuningOffset()
{
    // Both ends tuned 250 Hz up; the transmitter a few Hz off on top.
    Trial t = runTrial(4, Scale::Pentatonic, -12.0, 6000, 250.0, 250.0);
    printf("G4 tuned +250 Hz: %s\n", t.decoded ? "decoded" : "MISSED");
    CHECK(t.decoded);

    // A receiver tuned elsewhere hears nothing.
    t = runTrial(4, Scale::Pentatonic, -12.0, 6001, 250.0, 0.0);
    CHECK(!t.decodes[0].ok);
}

void testPythonClip(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
    if (!file.good())
    {
        fprintf(stderr, "cannot open %s\n", path);
        return;
    }
    std::vector<float> audio;
    unsigned char bytes[2];
    while (file.read((char*)bytes, 2)) audio.push_back((float)(int16_t)(bytes[0] | (bytes[1] << 8)) / 32768.0f);
    CHECK((int)audio.size() == GlissandoVectors::CLIP_SAMPLES);

    Payload expected{};
    for (const auto& v : GlissandoVectors::FEC)
    {
        if (std::string(v.name) == "random3")
            for (int i = 0; i < PAYLOAD_BITS; i++) expected[i] = (uint8_t)(v.payload[i] == '1');
    }

    ModemSettings rx;
    rx.gear = 4;
    std::vector<Decode> d = receive(audio.data(), audio.size(), rx);
    CHECK(d.size() == 1 && d[0].ok && d[0].payload == expected);
    if (d.empty()) return;
    printf("prototype clip: %s at %lld (prototype %d), %.3f Hz (sent %.1f), SNR estimate %.2f dB (prototype %.2f)\n",
           d[0].ok ? "decoded" : "MISSED", d[0].startSample, GlissandoVectors::CLIP_PROTOTYPE_START,
           d[0].frequencyOffsetHz, GlissandoVectors::CLIP_OFFSET_HZ, d[0].report.snrDb,
           GlissandoVectors::CLIP_PROTOTYPE_SNR_DB);
    CHECK(std::llabs(d[0].startSample - GlissandoVectors::CLIP_PROTOTYPE_START) <= 2);
    CHECK(std::fabs(d[0].report.snrDb - GlissandoVectors::CLIP_PROTOTYPE_SNR_DB) < 0.3);
}

void testNoiseOnly()
{
    Random rng(0xBADC0DE);
    int falseDecodes = 0;
    for (int gear = MIN_GEAR; gear <= MAX_GEAR; gear++)
    {
        const GearInfo& info = gearInfo(gear);
        std::vector<float> noise((size_t)info.frameSamples() + (size_t)(SEARCH_SECONDS * SAMPLE_RATE_HZ));
        for (float& v : noise) v = (float)(0.3 * rng.gaussian());
        ModemSettings rx;
        rx.gear = gear;
        for (const Decode& d : receive(noise.data(), noise.size(), rx)) falseDecodes += d.ok;
    }
    printf("noise only: %d false decodes\n", falseDecodes);
    CHECK(falseDecodes == 0);
}


// A noiseless loopback: a Presto frame, quantised as a sound card would,
// with digital silence before and after, searched at every gear. Windows
// that catch part of the frame and part of the silence used to decode as
// the all-zero word at a wildly high SNR.
void testSilenceAround()
{
    Random rng(0x51137);
    Payload sent = rng.payload();
    ModemSettings tx;
    tx.gear = 4;
    std::vector<float> frame = modulate({sent}, tx);

    int falseDecodes = 0;
    int found = 0;
    for (int gear = MIN_GEAR; gear <= MAX_GEAR; gear++)
    {
        const long long frameSamples = gearInfo(gear).frameSamples();
        std::vector<float> audio((size_t)SAMPLE_RATE_HZ, 0.0f);
        for (float v : frame) audio.push_back(std::round(v * 16384.0f) / 32768.0f);
        audio.resize(audio.size() + (size_t)frameSamples, 0.0f);

        ModemSettings rx;
        rx.gear = gear;
        const long long searchTo = (long long)audio.size() - frameSamples;
        for (long long from = 0; from < searchTo; from += frameSamples / 4)
        {
            for (const Decode& d : receive(audio.data(), audio.size(), rx, from,
                                           std::min(from + frameSamples / 4, searchTo)))
            {
                if (!d.ok) continue;
                if (d.payload == sent)
                    found++;
                else
                    falseDecodes++;
            }
        }
    }
    printf("silence around a clean frame: %d decodes of it, %d false decodes\n", found, falseDecodes);
    CHECK(found > 0);
    CHECK(falseDecodes == 0);
}

} // namespace

int main(int argc, char** argv)
{
    auto start = std::chrono::steady_clock::now();
    testClean();
    testAwgn();
    testScales();
    testTuningOffset();
    if (argc > 1) testPythonClip(argv[1]);
    testNoiseOnly();
    testSilenceAround();
    printf("decode tests took %.1f s\n", secondsSince(start));
    if (failures == 0) printf("glissando decode tests passed\n");
    return failures == 0 ? 0 : 1;
}
