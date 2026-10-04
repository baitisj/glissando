//=========================================================================
// Name:            GlissandoReceiver.cpp
// Purpose:         Real-time Glissando decoding on a continuous audio stream.
//
// Every gear searches its own sliding window. A gear's window covers every
// frame that could start in the next hop (a quarter frame) of the stream, so
// the search for it runs once the last of those frames can have arrived in
// full. Hops are aligned to multiples of the hop from the first sample, so
// G4 and G5, which have the same tempo, always search the same stretch, G4
// first.
//=========================================================================

#include "GlissandoReceiver.h"

#include <algorithm>
#include <cstdlib>

#include "GlissandoInternal.h"

namespace Glissando
{

namespace
{

// Frequency search range and sync candidates per search, as the prototype.
constexpr double MAX_OFFSET_HZ = 25.0;
constexpr int CANDIDATES = 3;

// Samples kept either side of a window so the FFT based analytic signal's
// edge effects (a few ms for a narrowband signal) stay off the frame.
constexpr long long HILBERT_GUARD = 256;

// Audio the worker may fall behind by before the oldest is dropped (60 s),
// and history kept beyond what the searches need (1 s), so the history is
// not moved on every push.
constexpr size_t PENDING_LIMIT = 60 * SAMPLE_RATE_HZ;
constexpr long long HISTORY_SLACK = SAMPLE_RATE_HZ;

// Averaging. A frame that fails to decode is kept as soft bits for
// AVERAGING_SECONDS; when a later candidate in the same voice, scale and
// tempo also fails, it is added to each kept copy that sat within
// AVERAGING_OFFSET_HZ of it and whose soft bits agree with it by
// MIN_AGREEMENT (detail::softAgreement), and the sum is decoded. A repeat
// (a retry, a second ping) then decodes on less signal than either copy
// needs alone, as JT65 decoders average. Unrelated frames and noise agree
// by about 0 +/- 0.07, so the agreement floor keeps the extra decodes, and
// with them the extra chances for noise to pass the CRC, few.
constexpr long long AVERAGING_SECONDS = 10 * 60;
constexpr size_t AVERAGING_KEPT = 2048;
// Only candidates that look like a signal are kept. A frame 1.5 dB below
// the decoding threshold measures an Es/N0 (see detail::VoiceDecode) of 1.35
// or more 95 % of the time; a noise peak measures 1.0 on median and above
// 1.3 about one time in six.
constexpr double MIN_KEPT_ES_OVER_N0 = 1.3;
constexpr double AVERAGING_OFFSET_HZ = 3.0;
constexpr double MIN_AGREEMENT = 0.2;
constexpr int MAX_AVERAGING_TRIES = 4;

// Geometry of one gear's search.
struct Geometry
{
    long long L;        // samples per symbol
    long long frame;    // samples per frame
    long long hop;      // a quarter frame: 21.5 symbols
    long long step;     // coarse sync step, L/8
    long long before;   // audio needed before the first start of a hop
    long long after;    // audio needed after the last frame of a hop ends

    explicit Geometry(int gear)
    {
        const GearInfo& info = gearInfo(gear);
        L = info.samplesPerSymbol();
        frame = info.frameSamples();
        hop = frame / 4;
        step = std::max(1LL, L / 8);
        // Coarse starts reach 2 steps below the hop (so a frame on the
        // boundary is seen by both searches), the fine search one more step,
        // and refinement a few samples more.
        before = 5 * step + HILBERT_GUARD;
        after = step + HILBERT_GUARD;
    }

    long long windowLength() const { return before + hop + frame + after; }
};

long long floorToMultiple(long long x, long long m)
{
    long long q = x / m;
    if (x < 0 && q * m != x) q--;
    return q * m;
}

} // namespace

struct StreamingReceiver::Impl
{
    struct GearState
    {
        int gear;
        long long nextStart;    // first frame start of the next hop to search
    };

    struct Recent
    {
        long long samplesPerSymbol;
        int voice;
        long long start;
    };

    // A candidate that did not decode, kept for averaging.
    struct Kept
    {
        long long samplesPerSymbol;
        int voice;
        Scale scale;
        long long start;
        double frequencyOffsetHz;
        double syncScore;
        FrameLlrs llrs;
    };

    // Shared with push() and the control calls, under mutex.
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::vector<float> pending;     // capacity PENDING_LIMIT, so push() never allocates
    long long pendingStart = 0;     // stream index of pending[0]
    long long processedUpTo = 0;    // every search due on audio before this has run
    bool stopRequested = false;
    bool running = false;
    unsigned resetGeneration = 0;
    long long resetAt = 0;
    bool configChanged = true;
    std::vector<int> gears{1, 2, 3, 4, 5};
    Scale scale = Scale::Pentatonic;
    double tuningOffsetHz = 0.0;
    bool anyScale = false;

    std::mutex callbackMutex;
    DecodeCallback callback;

    std::mutex knownBitsMutex;
    std::vector<KnownBits> knownBits;

    std::atomic<long long> samplesPushed{0};
    std::atomic<long long> lastFrameEnd{-1};

    mutable std::mutex reportMutex;
    ChannelReport lastReport;
    long long lastReportAt = -1;

    std::thread worker;

    // Worker thread only.
    std::vector<float> history;
    long long historyStart = 0;
    std::vector<GearState> states;
    std::vector<Recent> recent;
    std::vector<Kept> kept;
    unsigned seenGeneration = 0;
    double activeTuning = 0.0;
    std::vector<Scale> activeScales{Scale::Pentatonic};

    void run();
    void applyConfig(const std::vector<int>& newGears);
    GearState* nextDue();
    void search(GearState& state, unsigned generation);
    bool alreadyReported(long long samplesPerSymbol, int voice, long long start);
    bool average(const Geometry& geo, int voice, const detail::SoftFrame& soft, long long start, Scale scale,
                 const std::vector<KnownBits>& known, Decode& decodeOut);
    void keep(const Geometry& geo, int voice, const detail::SoftFrame& soft, long long start, Scale scale);
    void forgetKept(long long samplesPerSymbol, int voice, long long start);
};

StreamingReceiver::StreamingReceiver()
    : impl_(new Impl)
{
    impl_->pending.reserve(PENDING_LIMIT);
}

StreamingReceiver::~StreamingReceiver()
{
    stop();
    delete impl_;
}

void StreamingReceiver::configure(const std::vector<int>& gears, Scale scale, double tuningOffsetHz, bool anyScale)
{
    std::vector<int> valid;
    for (int g : gears)
    {
        if (g >= MIN_GEAR && g <= MAX_GEAR && std::find(valid.begin(), valid.end(), g) == valid.end())
            valid.push_back(g);
    }
    std::sort(valid.begin(), valid.end());

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->gears = valid;
    impl_->scale = scale;
    impl_->tuningOffsetHz = tuningOffsetHz;
    impl_->anyScale = anyScale;
    impl_->configChanged = true;
    impl_->wake.notify_one();
}

void StreamingReceiver::setKnownBits(const std::vector<KnownBits>& knownBits)
{
    std::lock_guard<std::mutex> lock(impl_->knownBitsMutex);
    impl_->knownBits = knownBits;
}

void StreamingReceiver::setDecodeCallback(DecodeCallback callback)
{
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->callback = std::move(callback);
}

void StreamingReceiver::start()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->running) return;

    impl_->pending.clear();
    impl_->pendingStart = 0;
    impl_->processedUpTo = 0;
    impl_->stopRequested = false;
    impl_->configChanged = true;
    impl_->samplesPushed = 0;
    impl_->lastFrameEnd = -1;
    {
        std::lock_guard<std::mutex> reportLock(impl_->reportMutex);
        impl_->lastReport = ChannelReport();
        impl_->lastReportAt = -1;
    }
    impl_->history.clear();
    impl_->historyStart = 0;
    impl_->states.clear();
    impl_->recent.clear();
    impl_->kept.clear();
    impl_->seenGeneration = impl_->resetGeneration;

    impl_->running = true;
    impl_->worker = std::thread([this]() { impl_->run(); });
}

void StreamingReceiver::stop()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running) return;
        impl_->stopRequested = true;
        impl_->wake.notify_one();
    }
    impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->running = false;
    impl_->idle.notify_all();
}

void StreamingReceiver::push(const short* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running) return;

    std::vector<float>& pending = impl_->pending;
    if (pending.size() + (size_t)numSamples > PENDING_LIMIT)
    {
        // The worker has fallen a minute behind; lose the oldest audio
        // rather than grow without bound.
        size_t drop = std::min(pending.size(), pending.size() + (size_t)numSamples - PENDING_LIMIT);
        pending.erase(pending.begin(), pending.begin() + (long)drop);
        impl_->pendingStart += (long long)drop;
    }
    for (int i = 0; i < numSamples; i++) pending.push_back(samples[i] * (1.0f / 32768.0f));
    impl_->samplesPushed += numSamples;
    impl_->wake.notify_one();
}

void StreamingReceiver::reset()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pendingStart += (long long)impl_->pending.size();
    impl_->pending.clear();
    impl_->resetGeneration++;
    impl_->resetAt = impl_->pendingStart;
    impl_->wake.notify_one();
}

void StreamingReceiver::flush()
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    long long target = impl_->samplesPushed;
    impl_->idle.wait(lock, [&]() { return !impl_->running || impl_->processedUpTo >= target; });
}

long long StreamingReceiver::samplesReceived() const
{
    return impl_->samplesPushed;
}

ChannelReport StreamingReceiver::lastReport(long long* atSample) const
{
    std::lock_guard<std::mutex> lock(impl_->reportMutex);
    if (atSample != nullptr) *atSample = impl_->lastReportAt;
    return impl_->lastReport;
}

long long StreamingReceiver::lastFrameEnd() const
{
    return impl_->lastFrameEnd;
}

bool StreamingReceiver::isBusy(long long holdSamples) const
{
    long long end = impl_->lastFrameEnd;
    return end >= 0 && impl_->samplesPushed - end <= holdSamples;
}

void StreamingReceiver::Impl::run()
{
    std::vector<float> incoming;
    incoming.reserve(PENDING_LIMIT);
    for (;;)
    {
        long long incomingStart;
        std::vector<int> newGears;
        bool haveNewConfig = false;
        unsigned generation;
        long long resetPoint;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&]() {
                return stopRequested || !pending.empty() || configChanged || resetGeneration != seenGeneration;
            });
            if (stopRequested) break;

            // Take the pending audio; the audio thread carries on into the
            // worker's emptied buffer, which already has the capacity.
            incoming.clear();
            std::swap(incoming, pending);
            incomingStart = pendingStart;
            pendingStart += (long long)incoming.size();

            if (configChanged)
            {
                newGears = gears;
                activeTuning = tuningOffsetHz;
                activeScales = listenedScales(scale, anyScale);
                configChanged = false;
                haveNewConfig = true;
            }
            generation = resetGeneration;
            resetPoint = resetAt;
        }

        if (generation != seenGeneration)
        {
            // Forget the audio before the reset and start every gear's
            // search at the first hop the new audio can fill.
            seenGeneration = generation;
            history.clear();
            historyStart = resetPoint;
            for (GearState& state : states)
            {
                Geometry geo(state.gear);
                state.nextStart = std::max(state.nextStart, floorToMultiple(resetPoint, geo.hop));
            }
        }

        if (!incoming.empty())
        {
            if (incomingStart != historyStart + (long long)history.size())
            {
                // Audio was dropped (a reset, or the worker fell too far
                // behind): the history is no longer contiguous.
                history.clear();
                historyStart = incomingStart;
            }
            history.insert(history.end(), incoming.begin(), incoming.end());
        }

        if (haveNewConfig) applyConfig(newGears);

        while (GearState* state = nextDue())
        {
            search(*state, generation);
        }

        // Drop the history no search needs any more: everything before the
        // earliest sample of any gear's next window. That is at most one
        // window of the slowest gear, unless the worker has fallen behind.
        long long end = historyStart + (long long)history.size();
        long long keepFrom = end;
        for (const GearState& state : states)
        {
            keepFrom = std::min(keepFrom, state.nextStart - Geometry(state.gear).before);
        }
        long long excess = keepFrom - historyStart;
        if (excess > HISTORY_SLACK)
        {
            history.erase(history.begin(), history.begin() + (long)excess);
            historyStart += excess;
        }

        std::lock_guard<std::mutex> lock(mutex);
        processedUpTo = std::max(processedUpTo, historyStart + (long long)history.size());
        idle.notify_all();
    }
}

void StreamingReceiver::Impl::applyConfig(const std::vector<int>& newGears)
{
    std::vector<GearState> next;
    for (int g : newGears)
    {
        auto it = std::find_if(states.begin(), states.end(), [g](const GearState& s) { return s.gear == g; });
        if (it != states.end())
        {
            next.push_back(*it);
        }
        else
        {
            // A new gear searches from the oldest audio still held.
            Geometry geo(g);
            next.push_back({g, floorToMultiple(historyStart, geo.hop)});
        }
    }
    states = next;
}

// The gear whose next window has arrived in full, earliest window first and
// the lower gear first when two windows end together (so G4 runs before G5).
StreamingReceiver::Impl::GearState* StreamingReceiver::Impl::nextDue()
{
    long long end = historyStart + (long long)history.size();
    GearState* best = nullptr;
    long long bestEnd = 0;
    for (GearState& state : states)
    {
        Geometry geo(state.gear);
        if (state.nextStart + geo.hop <= historyStart)
        {
            // Every frame of this hop started before the audio we still
            // hold (a reset, or the worker fell behind); move on to what we
            // have.
            state.nextStart = floorToMultiple(historyStart, geo.hop);
        }
        long long windowEnd = state.nextStart + geo.hop + geo.frame + geo.after;
        if (windowEnd > end) continue;
        if (best == nullptr || windowEnd < bestEnd || (windowEnd == bestEnd && state.gear < best->gear))
        {
            best = &state;
            bestEnd = windowEnd;
        }
    }
    return best;
}

bool StreamingReceiver::Impl::average(const Geometry& geo, int voice, const detail::SoftFrame& soft, long long start,
                                      Scale scale, const std::vector<KnownBits>& known, Decode& decodeOut)
{
    // Earlier copies that could be this frame sent again, best agreement
    // first. A copy less than a frame earlier is this same frame, seen by
    // an overlapping search.
    std::vector<std::pair<double, size_t>> matches;
    for (size_t i = 0; i < kept.size(); i++)
    {
        const Kept& k = kept[i];
        if (k.samplesPerSymbol != geo.L || k.voice != voice || k.scale != scale) continue;
        if (start - k.start < geo.frame) continue;
        if (std::fabs(k.frequencyOffsetHz - soft.frequencyOffsetHz) > AVERAGING_OFFSET_HZ) continue;
        double agreement = detail::softAgreement(k.llrs, soft.llrs);
        if (agreement >= MIN_AGREEMENT) matches.push_back({agreement, i});
    }
    if (matches.empty()) return false;
    std::sort(matches.begin(), matches.end(), std::greater<std::pair<double, size_t>>());
    if (matches.size() > (size_t)MAX_AVERAGING_TRIES) matches.resize((size_t)MAX_AVERAGING_TRIES);

    auto add = [](FrameLlrs& sum, const FrameLlrs& llrs) {
        for (int i = 0; i < FRAME_BITS; i++) sum[(size_t)i] += llrs[(size_t)i];
    };
    auto decoded = [&](const FrameLlrs& sum, int copies, const std::vector<size_t>& used) {
        Payload payload;
        if (!detail::decodeWithGuesses(sum, known, payload)) return false;
        decodeOut.ok = true;
        decodeOut.payload = payload;
        decodeOut.startSample = start;
        decodeOut.frequencyOffsetHz = soft.frequencyOffsetHz;
        decodeOut.report = soft.report;
        decodeOut.scale = scale;
        decodeOut.copies = copies;
        // Those copies are spent: a later repeat starts afresh.
        std::vector<size_t> drop = used;
        std::sort(drop.begin(), drop.end(), std::greater<size_t>());
        for (size_t i : drop) kept.erase(kept.begin() + (long)i);
        return true;
    };

    // This copy with each earlier one, then with all of them together.
    for (const auto& match : matches)
    {
        FrameLlrs sum = soft.llrs;
        add(sum, kept[match.second].llrs);
        if (decoded(sum, 2, {match.second})) return true;
    }
    if (matches.size() > 1)
    {
        FrameLlrs sum = soft.llrs;
        std::vector<size_t> used;
        for (const auto& match : matches)
        {
            add(sum, kept[match.second].llrs);
            used.push_back(match.second);
        }
        if (decoded(sum, 1 + (int)used.size(), used)) return true;
    }
    return false;
}

void StreamingReceiver::Impl::keep(const Geometry& geo, int voice, const detail::SoftFrame& soft, long long start,
                                   Scale scale)
{
    if (soft.esOverN0 < MIN_KEPT_ES_OVER_N0) return;

    // Overlapping searches find the same frame more than once; keep the
    // copy with the better sync.
    for (Kept& k : kept)
    {
        if (k.samplesPerSymbol == geo.L && k.voice == voice && std::llabs(k.start - start) <= geo.L &&
            std::fabs(k.frequencyOffsetHz - soft.frequencyOffsetHz) <= AVERAGING_OFFSET_HZ)
        {
            if (soft.syncScore > k.syncScore)
                k = {geo.L, voice, scale, start, soft.frequencyOffsetHz, soft.syncScore, soft.llrs};
            return;
        }
    }
    if (kept.size() >= AVERAGING_KEPT) kept.erase(kept.begin());
    kept.push_back({geo.L, voice, scale, start, soft.frequencyOffsetHz, soft.syncScore, soft.llrs});
}

void StreamingReceiver::Impl::forgetKept(long long samplesPerSymbol, int voice, long long start)
{
    kept.erase(std::remove_if(kept.begin(), kept.end(),
                              [&](const Kept& k) {
                                  return k.samplesPerSymbol == samplesPerSymbol && k.voice == voice &&
                                         std::llabs(k.start - start) <= samplesPerSymbol;
                              }),
               kept.end());
}

bool StreamingReceiver::Impl::alreadyReported(long long samplesPerSymbol, int voice, long long start)
{
    // One frame is found by every search whose window covers it, and a
    // voice 0 frame at G4's tempo by both G4 and G5 (it is the same
    // waveform): report it once. Frames of one voice at one tempo cannot
    // start within a symbol of each other.
    for (const Recent& r : recent)
    {
        if (r.samplesPerSymbol == samplesPerSymbol && r.voice == voice && std::llabs(r.start - start) <= samplesPerSymbol)
            return true;
    }
    return false;
}

void StreamingReceiver::Impl::search(GearState& state, unsigned generation)
{
    const GearInfo& info = gearInfo(state.gear);
    Geometry geo(state.gear);
    const long long first = state.nextStart;
    state.nextStart += geo.hop;

    long long windowStart = std::max(historyStart, first - geo.before);
    long long windowEnd = first + geo.hop + geo.frame + geo.after;
    if (windowEnd - windowStart < geo.frame) return;

    detail::ComplexSignal z;
    detail::analyticSignal(history.data() + (windowStart - historyStart), (size_t)(windowEnd - windowStart), z);

    std::vector<KnownBits> known;
    {
        std::lock_guard<std::mutex> lock(knownBitsMutex);
        known = knownBits;
    }

    std::vector<StreamDecode> found;
    bool haveReport = false;
    bool reportDecoded = false;
    ChannelReport report;
    long long reportAt = -1;
    for (int voice = 0; voice < info.voices; voice++)
    {
        std::vector<std::shared_ptr<const detail::VoiceTemplates>> held;
        std::vector<const detail::VoiceTemplates*> hypotheses;
        for (Scale scale : activeScales)
        {
            held.push_back(detail::voiceTemplates(scale, voice, state.gear, activeTuning));
            hypotheses.push_back(held.back().get());
        }
        detail::VoiceDecode result = detail::receiveVoice(z, info, hypotheses, first - 2 * geo.step - windowStart,
                                                          first + geo.hop - windowStart, MAX_OFFSET_HZ, CANDIDATES,
                                                          known);
        if (!result.haveCandidate) continue;

        long long start = windowStart + result.decode.startSample;
        // The report of a decoded frame beats the best guess of a search
        // that decoded nothing.
        if (!haveReport || (result.decode.ok && !reportDecoded))
        {
            haveReport = true;
            reportDecoded = result.decode.ok;
            report = result.decode.report;
            reportAt = start + geo.frame;
        }
        Decode decoded = result.decode;
        decoded.scale = activeScales[(size_t)result.hypothesis];
        if (!decoded.ok)
        {
            // Strongest candidate first: add each to the copies kept from
            // earlier, and keep it in turn if that does not decode either.
            for (const detail::SoftFrame& soft : result.failed)
            {
                long long softStart = windowStart + soft.startSample;
                Scale softScale = activeScales[(size_t)soft.hypothesis];
                if (average(geo, voice, soft, softStart, softScale, known, decoded))
                {
                    start = softStart;
                    break;
                }
            }
            for (const detail::SoftFrame& soft : result.failed)
            {
                long long softStart = windowStart + soft.startSample;
                if (!decoded.ok || std::llabs(softStart - start) > geo.L)
                    keep(geo, voice, soft, softStart, activeScales[(size_t)soft.hypothesis]);
            }
        }
        if (!decoded.ok) continue;
        // A kept copy of a frame that has now decoded is no use any more.
        forgetKept(geo.L, voice, start);
        if (alreadyReported(geo.L, voice, start)) continue;

        recent.push_back({geo.L, voice, start});
        StreamDecode decode;
        decode.gear = state.gear;
        decode.decode = decoded;
        decode.decode.voice = voice;
        decode.decode.startSample = start;
        found.push_back(decode);
    }

    // Forget kept copies too old to be averaged with.
    kept.erase(std::remove_if(kept.begin(), kept.end(),
                              [&](const Kept& k) {
                                  return k.start < first - AVERAGING_SECONDS * SAMPLE_RATE_HZ;
                              }),
               kept.end());

    // Forget frames at this tempo that no window can reach any more (the
    // windows reach only a few steps before their first start).
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [&](const Recent& r) {
                                    return r.samplesPerSymbol == geo.L && r.start < first - 2 * geo.frame;
                                }),
                 recent.end());
    if (recent.size() > 64) recent.erase(recent.begin(), recent.end() - 64);

    {
        // A reset while the search ran means this audio is no longer
        // wanted (typically it was our own transmission).
        std::lock_guard<std::mutex> lock(mutex);
        if (resetGeneration != generation) return;
    }

    if (haveReport)
    {
        std::lock_guard<std::mutex> lock(reportMutex);
        lastReport = report;
        lastReportAt = reportAt;
    }

    // Voice order within a duet frame (the link layer reassembles in it).
    std::stable_sort(found.begin(), found.end(),
                     [](const StreamDecode& a, const StreamDecode& b) { return a.decode.voice < b.decode.voice; });
    DecodeCallback cb;
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        cb = callback;
    }
    for (const StreamDecode& decode : found)
    {
        long long end = decode.decode.startSample + geo.frame;
        if (end > lastFrameEnd) lastFrameEnd = end;
        if (cb) cb(decode);
    }
}

} // namespace Glissando
