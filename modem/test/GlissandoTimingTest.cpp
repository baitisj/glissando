//=========================================================================
// Name:            GlissandoTimingTest.cpp
// Purpose:         Times one streaming search per gear (what the
//                  StreamingReceiver's worker does every quarter frame) on
//                  noise, where all three sync candidates are demodulated
//                  and fail, the worst case, and on a frame.
//=========================================================================

#include <algorithm>

#include "GlissandoTestUtil.h"

using namespace Glissando;
using namespace GlissandoTest;

namespace
{

// One search: the analytic signal of the window, then every voice's sync
// search over a hop of starts, with refinement, demodulation and decoding of
// up to three candidates. Returns seconds taken and whether anything decoded.
double timeSearch(const std::vector<float>& window, int gear, long long firstStart, long long hop, bool& decoded)
{
    const GearInfo& info = gearInfo(gear);
    auto start = std::chrono::steady_clock::now();
    detail::ComplexSignal z;
    detail::analyticSignal(window.data(), window.size(), z);
    decoded = false;
    for (int voice = 0; voice < info.voices; voice++)
    {
        auto templates = detail::voiceTemplates(Scale::Pentatonic, voice, gear, 0.0);
        detail::VoiceDecode d = detail::receiveVoice(z, info, *templates, firstStart, firstStart + hop, 25.0, 3);
        decoded = decoded || d.decode.ok;
    }
    return secondsSince(start);
}

} // namespace

int main()
{
    Random rng(0x7131);
    double total = 0.0;
    for (int gear = MIN_GEAR; gear <= MAX_GEAR; gear++)
    {
        const GearInfo& info = gearInfo(gear);
        const long long L = info.samplesPerSymbol();
        const long long frame = info.frameSamples();
        const long long hop = frame / 4;
        const long long before = 5 * (L / 8) + 256;
        const long long length = before + hop + frame + L / 8 + 256;

        // Build the templates first; that happens once per gear, not per search.
        auto built = std::chrono::steady_clock::now();
        for (int voice = 0; voice < info.voices; voice++) detail::voiceTemplates(Scale::Pentatonic, voice, gear, 0.0);
        double buildSeconds = secondsSince(built);

        std::vector<float> noise((size_t)length);
        for (float& v : noise) v = (float)(0.3 * rng.gaussian());
        bool decoded = false;
        double noiseSeconds = timeSearch(noise, gear, before, hop, decoded);
        CHECK(!decoded);

        std::vector<Payload> payloads;
        for (int v = 0; v < info.voices; v++) payloads.push_back(rng.payload());
        ModemSettings tx;
        tx.gear = gear;
        std::vector<float> x = modulate(payloads, tx);
        std::vector<float> window = pad(x, (size_t)(before + hop / 2), (size_t)(length - before - hop / 2 - frame));
        addNoise(window, -10.0, meanSquare(x), rng);
        double signalSeconds = timeSearch(window, gear, before, hop, decoded);
        CHECK(decoded);

        double hopSeconds = (double)hop / SAMPLE_RATE_HZ;
        printf("G%d: a search every %.2f s over a %.1f s window: %.0f ms on noise (%.1f %% of real time), "
               "%.0f ms with a frame; templates built in %.0f ms\n",
               gear, hopSeconds, (double)length / SAMPLE_RATE_HZ, noiseSeconds * 1000, 100 * noiseSeconds / hopSeconds,
               signalSeconds * 1000, buildSeconds * 1000);
        total += noiseSeconds / hopSeconds;
        // Far inside real time on any desktop, even unoptimised.
        CHECK(noiseSeconds < hopSeconds);
    }
    printf("all five gears on noise: %.1f %% of one core\n", 100 * total);
    if (failures == 0) printf("glissando timing tests passed\n");
    return failures == 0 ? 0 : 1;
}
