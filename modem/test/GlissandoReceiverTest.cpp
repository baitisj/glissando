//=========================================================================
// Name:            GlissandoReceiverTest.cpp
// Purpose:         The streaming receiver on continuous audio pushed in
//                  20 ms chunks: frames reported once each, in voice order,
//                  carrier sense, reset, and silence.
//=========================================================================

#include <algorithm>
#include <climits>
#include <mutex>

#include "../GlissandoReceiver.h"
#include "GlissandoTestUtil.h"

using namespace Glissando;
using namespace GlissandoTest;

namespace
{

constexpr int CHUNK = SAMPLE_RATE_HZ / 50; // 20 ms, as an audio callback delivers

class Collector
{
public:
    void attach(StreamingReceiver& receiver)
    {
        receiver.setDecodeCallback([this](const StreamDecode& d) {
            std::lock_guard<std::mutex> lock(mutex_);
            decodes_.push_back(d);
        });
    }

    std::vector<StreamDecode> decodes()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return decodes_;
    }

private:
    std::mutex mutex_;
    std::vector<StreamDecode> decodes_;
};

struct Placed
{
    int gear;
    long long start;
    std::vector<Payload> payloads;
};

// Adds a transmission to the stream at `start`, at the given SNR relative to
// the stream's noise of standard deviation sigma.
void place(std::vector<float>& stream, const Placed& p, double snrDb, double sigma)
{
    ModemSettings tx;
    tx.gear = p.gear;
    std::vector<float> x = modulate(p.payloads, tx);
    // Invert channel.add_noise(): the signal power that puts sigma at snrDb.
    double n0 = sigma * sigma / (SAMPLE_RATE_HZ / 2.0);
    double power = n0 * 2500.0 * std::pow(10.0, snrDb / 10.0);
    double gain = std::sqrt(power / meanSquare(x));
    for (size_t n = 0; n < x.size(); n++) stream[(size_t)p.start + n] += (float)(gain * x[n]);
}

std::vector<short> toShort(const std::vector<float>& x)
{
    std::vector<short> out(x.size());
    for (size_t n = 0; n < x.size(); n++)
    {
        double v = std::round(x[n] * 32768.0);
        out[n] = (short)std::min(std::max(v, (double)SHRT_MIN), (double)SHRT_MAX);
    }
    return out;
}

// Pushes like an audio thread: 20 ms at a time, never waiting for the
// worker. Returns the longest push() in seconds.
double pushAll(StreamingReceiver& receiver, const std::vector<short>& audio)
{
    double longest = 0.0;
    for (size_t n = 0; n < audio.size(); n += CHUNK)
    {
        int count = (int)std::min<size_t>(CHUNK, audio.size() - n);
        auto start = std::chrono::steady_clock::now();
        receiver.push(audio.data() + n, count);
        longest = std::max(longest, secondsSince(start));
    }
    return longest;
}

bool matches(const StreamDecode& d, const Placed& p, int voice)
{
    return d.decode.ok && d.decode.voice == voice && d.decode.payload == p.payloads[(size_t)voice] &&
           std::llabs(d.decode.startSample - p.start) <= 8;
}

// Two back-to-back G4 frames and a G3 frame, listening on G3, G4 and G5 (G5
// voice 0 is G4's waveform, so every G4 frame is found twice and must be
// reported once).
void testFramesReportedOnce()
{
    Random rng(0x57EA);
    const double sigma = 0.02;
    const long long f4 = gearInfo(4).frameSamples();
    const long long f3 = gearInfo(3).frameSamples();

    std::vector<Placed> frames;
    long long a = (long long)(rng.uniform(1.0, 3.0) * SAMPLE_RATE_HZ);
    frames.push_back({4, a, {rng.payload()}});
    frames.push_back({4, a + f4, {rng.payload()}}); // back to back
    long long c = a + 2 * f4 + (long long)(rng.uniform(0.5, 2.0) * SAMPLE_RATE_HZ);
    frames.push_back({3, c, {rng.payload()}});
    // Room after the last frame for the search that covers it (a quarter
    // G3 frame plus a little), then some quiet for the busy test.
    long long total = c + f3 + (long long)(4.5 * SAMPLE_RATE_HZ);

    std::vector<float> stream((size_t)total);
    for (float& v : stream) v = (float)(sigma * rng.gaussian());
    for (const Placed& p : frames) place(stream, p, -10.0, sigma);
    std::vector<short> audio = toShort(stream);

    StreamingReceiver receiver;
    Collector collector;
    collector.attach(receiver);
    receiver.configure({3, 4, 5}, Scale::Pentatonic, 0.0);
    receiver.start();
    CHECK(!receiver.isBusy(10 * SAMPLE_RATE_HZ));

    auto start = std::chrono::steady_clock::now();
    double longestPush = pushAll(receiver, audio);
    double pushSeconds = secondsSince(start);
    receiver.flush();
    double seconds = secondsSince(start);
    CHECK(receiver.samplesReceived() == total);

    std::vector<StreamDecode> decodes = collector.decodes();
    printf("streaming: %.1f s of audio pushed in %.2f s (longest push %.3f ms), decoded in %.2f s: %zu decodes\n",
           (double)total / SAMPLE_RATE_HZ, pushSeconds, longestPush * 1000, seconds, decodes.size());
    for (const StreamDecode& d : decodes)
    {
        printf("  G%d voice %d at %lld, %.2f Hz, SNR %.1f dB\n", d.gear, d.decode.voice, d.decode.startSample,
               d.decode.frequencyOffsetHz, d.decode.report.snrDb);
    }
    CHECK(decodes.size() == frames.size());
    for (const Placed& p : frames)
    {
        int count = 0;
        for (const StreamDecode& d : decodes) count += matches(d, p, 0);
        CHECK(count == 1);
    }
    // The G3 frame comes from the G3 search, the G4 ones from G4 or G5.
    for (const StreamDecode& d : decodes)
    {
        bool g3 = matches(d, frames[2], 0);
        CHECK(g3 ? d.gear == 3 : (d.gear == 4 || d.gear == 5));
    }
    // push() copies and returns: it must never wait out a search.
    CHECK(longestPush < 0.005);

    // Carrier sense: the G3 frame ended 4.5 s before the last sample.
    CHECK(receiver.isBusy(5 * SAMPLE_RATE_HZ));
    CHECK(!receiver.isBusy(4 * SAMPLE_RATE_HZ));

    long long at = -1;
    receiver.lastReport(&at);
    CHECK(at > 0);

    receiver.stop();
    receiver.stop(); // idempotent
}

// A duet frame is reported voice 0 first, each voice once, whether G4 or
// G5 finds voice 0 first.
void testDuet()
{
    Random rng(0xD0E7);
    const double sigma = 0.02;
    const long long f5 = gearInfo(5).frameSamples();
    Placed duet{5, (long long)(rng.uniform(1.0, 2.0) * SAMPLE_RATE_HZ), {rng.payload(), rng.payload()}};
    long long total = duet.start + f5 + (long long)(2.5 * SAMPLE_RATE_HZ);
    std::vector<float> stream((size_t)total);
    for (float& v : stream) v = (float)(sigma * rng.gaussian());
    place(stream, duet, -4.0, sigma);
    std::vector<short> audio = toShort(stream);

    for (const std::vector<int>& gears : {std::vector<int>{5}, std::vector<int>{4, 5}})
    {
        StreamingReceiver receiver;
        Collector collector;
        collector.attach(receiver);
        receiver.configure(gears, Scale::Pentatonic, 0.0);
        receiver.start();
        receiver.start(); // idempotent
        pushAll(receiver, audio);
        receiver.flush();
        std::vector<StreamDecode> decodes = collector.decodes();
        CHECK(decodes.size() == 2);
        if (decodes.size() == 2)
        {
            CHECK(matches(decodes[0], duet, 0));
            CHECK(matches(decodes[1], duet, 1));
            CHECK(decodes[1].gear == 5);
        }
    }
}

// Audio dropped by reset() is not decoded, and the stream carries on.
void testReset()
{
    Random rng(0x2E5E);
    const double sigma = 0.02;
    const long long f4 = gearInfo(4).frameSamples();
    Placed first{4, 8000, {rng.payload()}};
    Placed second{4, 8000 + f4 + 16000, {rng.payload()}};
    long long total = second.start + f4 + 2 * SAMPLE_RATE_HZ;
    std::vector<float> stream((size_t)total);
    for (float& v : stream) v = (float)(sigma * rng.gaussian());
    place(stream, first, -6.0, sigma);
    place(stream, second, -6.0, sigma);
    std::vector<short> audio = toShort(stream);

    StreamingReceiver receiver;
    Collector collector;
    collector.attach(receiver);
    receiver.configure({4}, Scale::Pentatonic, 0.0);
    receiver.start();

    // Half of the first frame, then a reset (as after our own transmission).
    std::vector<short> part(audio.begin(), audio.begin() + (long)(first.start + f4 / 2));
    pushAll(receiver, part);
    receiver.flush();
    receiver.reset();
    std::vector<short> rest(audio.begin() + (long)part.size(), audio.end());
    pushAll(receiver, rest);
    receiver.flush();

    std::vector<StreamDecode> decodes = collector.decodes();
    CHECK(decodes.size() == 1);
    if (!decodes.empty()) CHECK(matches(decodes[0], second, 0));
    CHECK(receiver.samplesReceived() == total);
}

// Digital silence and a constant input (a DC offset) decode nothing: no
// signal means no LLRs, and zero LLRs would decode to the all-zero word,
// whose CRC is zero too.
void testSilence()
{
    for (short level : {(short)0, (short)3, (short)-200})
    {
        std::vector<short> audio((size_t)(20 * SAMPLE_RATE_HZ), level);
        StreamingReceiver receiver;
        Collector collector;
        collector.attach(receiver);
        receiver.push(audio.data(), CHUNK); // before start(): ignored
        receiver.configure({2, 3, 4, 5}, Scale::Pentatonic, 0.0);
        receiver.start();
        pushAll(receiver, audio);
        receiver.flush();
        size_t n = collector.decodes().size();
        if (n != 0) printf("constant input %d: %zu decodes\n", level, n);
        CHECK(n == 0);
        CHECK(!receiver.isBusy(LLONG_MAX / 2));
        CHECK(receiver.samplesReceived() == (long long)audio.size());

        for (int gear = MIN_GEAR; gear <= MAX_GEAR; gear++)
        {
            ModemSettings rx;
            rx.gear = gear;
            std::vector<float> buffer((size_t)gearInfo(gear).frameSamples() + 12000, level / 32768.0f);
            for (const Decode& d : receive(buffer.data(), buffer.size(), rx)) CHECK(!d.ok);
        }
    }
}

} // namespace

int main()
{
    auto start = std::chrono::steady_clock::now();
    testFramesReportedOnce();
    testDuet();
    testReset();
    testSilence();
    printf("receiver tests took %.1f s\n", secondsSince(start));
    if (failures == 0) printf("glissando receiver tests passed\n");
    return failures == 0 ? 0 : 1;
}
