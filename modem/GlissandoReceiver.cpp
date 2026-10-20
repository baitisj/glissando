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

// Subtraction, as JTTY's and FT8's decoders do it. Every frame that decodes
// is kept as heard while any audio it overlaps may still be searched, and is
// taken out of that audio (detail::subtractFrame) before every later search,
// at every tempo. A search that decodes a frame takes it out and searches
// the same stretch again, for up to MAX_DECODES_PER_SEARCH frames per voice:
// two stations that key up together at one tempo are both heard. And the
// candidates kept for averaging that a new frame overlaps are decoded again
// without it, strongest first, up to MAX_RETRIES_PER_DECODE of them: of two
// frames that collide, the one that started first was searched while the
// other was still on the air, and is often the weaker.
constexpr int MAX_DECODES_PER_SEARCH = 3;
constexpr int MAX_RETRIES_PER_DECODE = 16;

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

    // A candidate that did not decode, kept for averaging and to decode
    // again once a frame over it is taken out.
    struct Kept
    {
        long long samplesPerSymbol;
        int voice;
        Scale scale;
        long long start;
        double frequencyOffsetHz;
        double syncScore;
        FrameLlrs llrs;
        int gear;
        ChannelReport report;
        // Whether these soft bits have been list decoded (listMature()).
        bool listed;
    };

    // A frame that decoded, kept to be taken out of later searches' audio.
    struct Heard
    {
        int gear;
        int voice;
        Scale scale;
        long long start;
        double frequencyOffsetHz;
        Payload payload;
        long long frameSamples;
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
    PayloadCheck payloadCheck;  // under knownBitsMutex too

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
    std::vector<Heard> heard;
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
    void keep(const Geometry& geo, int voice, const detail::SoftFrame& soft, long long start, Scale scale, int gear);
    void forgetKept(long long samplesPerSymbol, int voice, long long start);
    bool analyticOf(long long from, long long to, detail::ComplexSignal& z) const;
    int subtractHeard(detail::ComplexSignal& z, long long zStart, long long from, long long to) const;
    void addHeard(const detail::ComplexSignal& z, long long zStart, int gear, int voice, Scale scale, Decode& decode);
    void retryKept(const Heard& over, const std::vector<KnownBits>& known, std::vector<StreamDecode>& found,
                   std::vector<Heard>& added);
    void listMature(const std::vector<KnownBits>& known, const PayloadCheck& check, std::vector<StreamDecode>& found,
                    std::vector<Heard>& added);
    long long lookBackSamples() const;
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

void StreamingReceiver::setPayloadCheck(PayloadCheck check)
{
    std::lock_guard<std::mutex> lock(impl_->knownBitsMutex);
    impl_->payloadCheck = std::move(check);
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
    impl_->heard.clear();
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
                // Frames heard at another tuning would be re-synthesised on
                // the wrong notes.
                if (tuningOffsetHz != activeTuning) heard.clear();
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
        // earliest sample of any gear's next window, and of any kept
        // candidate a new frame could still be taken out from under
        // (lookBackSamples()). That is at most a window and a frame of the
        // slowest gear, unless the worker has fallen behind.
        long long end = historyStart + (long long)history.size();
        long long keepFrom = end - lookBackSamples();
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
                                   Scale scale, int gear)
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
                k = {geo.L, voice, scale, start, soft.frequencyOffsetHz, soft.syncScore, soft.llrs, gear, soft.report,
                     false};
            return;
        }
    }
    if (kept.size() >= AVERAGING_KEPT) kept.erase(kept.begin());
    kept.push_back(
        {geo.L, voice, scale, start, soft.frequencyOffsetHz, soft.syncScore, soft.llrs, gear, soft.report, false});
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

long long StreamingReceiver::Impl::lookBackSamples() const
{
    // A kept candidate can be decoded again once a frame that overlaps it
    // decodes: as long ago as a whole window of that frame's tempo, then a
    // whole frame of the candidate's.
    long long window = 0;
    long long frame = 0;
    for (const GearState& state : states)
    {
        Geometry geo(state.gear);
        window = std::max(window, geo.windowLength());
        frame = std::max(frame, geo.frame);
    }
    return window + frame + HILBERT_GUARD;
}

bool StreamingReceiver::Impl::analyticOf(long long from, long long to, detail::ComplexSignal& z) const
{
    from = std::max(from, historyStart);
    to = std::min(to, historyStart + (long long)history.size());
    if (to <= from) return false;
    detail::analyticSignal(history.data() + (from - historyStart), (size_t)(to - from), z);
    return true;
}

int StreamingReceiver::Impl::subtractHeard(detail::ComplexSignal& z, long long zStart, long long from,
                                           long long to) const
{
    // Every heard frame that overlaps [from, to), in the order they decoded.
    int removed = 0;
    const long long zEnd = zStart + (long long)z.size();
    for (const Heard& h : heard)
    {
        if (h.start >= zEnd || h.start + h.frameSamples <= zStart) continue;
        if (h.start < to && h.start + h.frameSamples > from) removed++;
        std::shared_ptr<const detail::VoiceTemplates> voice =
            detail::voiceTemplates(h.scale, h.voice, h.gear, activeTuning);
        detail::subtractFrame(z, *voice, gearInfo(h.gear), h.start - zStart, h.frequencyOffsetHz, h.payload);
    }
    return removed;
}

void StreamingReceiver::Impl::addHeard(const detail::ComplexSignal& z, long long zStart, int gear, int voice,
                                       Scale scale, Decode& decode)
{
    // Timed against the whole melody while the whole frame is in z, which
    // later searches, holding only part of it, could not do as well.
    std::shared_ptr<const detail::VoiceTemplates> templates = detail::voiceTemplates(scale, voice, gear, activeTuning);
    long long start = decode.startSample - zStart;
    double offset = decode.frequencyOffsetHz;
    detail::refineFrame(z, *templates, gearInfo(gear), decode.payload, start, offset);
    if (heard.size() >= 256) heard.erase(heard.begin());
    heard.push_back({gear, voice, scale, zStart + start, offset, decode.payload, gearInfo(gear).frameSamples()});
}

void StreamingReceiver::Impl::retryKept(const Heard& over, const std::vector<KnownBits>& known,
                                        std::vector<StreamDecode>& found, std::vector<Heard>& added)
{
    std::vector<size_t> overlapping;
    for (size_t i = 0; i < kept.size(); i++)
    {
        const Kept& k = kept[i];
        long long frame = gearInfo(k.gear).frameSamples();
        if (k.start < over.start + over.frameSamples && k.start + frame > over.start) overlapping.push_back(i);
    }
    std::sort(overlapping.begin(), overlapping.end(),
              [&](size_t a, size_t b) { return kept[a].syncScore > kept[b].syncScore; });
    if (overlapping.size() > (size_t)MAX_RETRIES_PER_DECODE) overlapping.resize((size_t)MAX_RETRIES_PER_DECODE);

    std::vector<Kept> candidates;
    for (size_t i : overlapping) candidates.push_back(kept[i]);
    for (const Kept& k : candidates)
    {
        const GearInfo& info = gearInfo(k.gear);
        Geometry geo(k.gear);
        const long long from = k.start - geo.step - HILBERT_GUARD;
        const long long to = k.start + geo.frame + geo.step + HILBERT_GUARD;
        if (from < historyStart || to > historyStart + (long long)history.size()) continue;
        if (alreadyReported(geo.L, k.voice, k.start)) continue;

        detail::ComplexSignal z;
        if (!analyticOf(from, to, z)) continue;
        int removed = subtractHeard(z, from, k.start, k.start + geo.frame);
        std::shared_ptr<const detail::VoiceTemplates> voice =
            detail::voiceTemplates(k.scale, k.voice, k.gear, activeTuning);
        detail::VoiceDecode result =
            detail::receiveAt(z, info, *voice, k.start - from, k.frequencyOffsetHz, MAX_OFFSET_HZ, known);
        if (!result.decode.ok)
        {
            // Without the frame on top it is a cleaner copy, to average a
            // repeat with and to list decode (again).
            if (result.failed.empty()) continue;
            for (Kept& copy : kept)
            {
                if (copy.samplesPerSymbol == k.samplesPerSymbol && copy.voice == k.voice && copy.start == k.start)
                {
                    copy.llrs = result.failed.front().llrs;
                    copy.report = result.failed.front().report;
                    copy.listed = false;
                    break;
                }
            }
            continue;
        }

        long long start = from + result.decode.startSample;
        forgetKept(geo.L, k.voice, start);
        if (alreadyReported(geo.L, k.voice, start)) continue;
        recent.push_back({geo.L, k.voice, start});

        StreamDecode decode;
        decode.gear = k.gear;
        decode.decode = result.decode;
        decode.decode.voice = k.voice;
        decode.decode.scale = k.scale;
        decode.decode.startSample = start;
        decode.decode.framesRemoved = removed;
        addHeard(z, from, k.gear, k.voice, k.scale, decode.decode);
        added.push_back(heard.back());
        found.push_back(decode);
    }
}

void StreamingReceiver::Impl::listMature(const std::vector<KnownBits>& known, const PayloadCheck& check,
                                         std::vector<StreamDecode>& found, std::vector<Heard>& added)
{
    // A kept candidate is list decoded once, when no search can find a
    // better copy of it: every gear at its tempo has searched past it (G5's
    // first voice is G4's waveform). Only audio still held is listed, so a
    // decode never arrives long after its frame.
    std::vector<Kept> ready;
    const long long held = historyStart;
    for (Kept& k : kept)
    {
        if (k.listed) continue;
        bool passed = true;
        for (const GearState& state : states)
        {
            Geometry geo(state.gear);
            if (geo.L == k.samplesPerSymbol && k.start >= state.nextStart - 3 * geo.step) passed = false;
        }
        if (!passed) continue;
        k.listed = true;
        if (k.start >= held) ready.push_back(k);
    }

    for (const Kept& k : ready)
    {
        Payload payload{};
        if (!detail::decodeListWithGuesses(k.llrs, known, check, payload)) continue;
        Geometry geo(k.gear);
        forgetKept(geo.L, k.voice, k.start);
        if (alreadyReported(geo.L, k.voice, k.start)) continue;
        recent.push_back({geo.L, k.voice, k.start});

        StreamDecode decode;
        decode.gear = k.gear;
        decode.decode.ok = true;
        decode.decode.voice = k.voice;
        decode.decode.scale = k.scale;
        decode.decode.payload = payload;
        decode.decode.startSample = k.start;
        decode.decode.frequencyOffsetHz = k.frequencyOffsetHz;
        decode.decode.report = k.report;

        // Sounded, timed and taken out like any other decode, with the
        // frames heard over it out of the way first.
        const long long from = k.start - geo.step - HILBERT_GUARD;
        const long long to = k.start + geo.frame + geo.step + HILBERT_GUARD;
        detail::ComplexSignal z;
        if (from >= historyStart && to <= historyStart + (long long)history.size() && analyticOf(from, to, z))
        {
            decode.decode.framesRemoved = subtractHeard(z, from, k.start, k.start + geo.frame);
            std::shared_ptr<const detail::VoiceTemplates> voice =
                detail::voiceTemplates(k.scale, k.voice, k.gear, activeTuning);
            double doppler =
                detail::soundDecoded(z, gearInfo(k.gear), *voice, k.start - from, k.frequencyOffsetHz, payload);
            if (doppler >= 0) decode.decode.report.dopplerHz = doppler;
            addHeard(z, from, k.gear, k.voice, k.scale, decode.decode);
            added.push_back(heard.back());
        }
        found.push_back(decode);
    }
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
    // Frames already heard, at any tempo, out of the way first.
    subtractHeard(z, windowStart, windowStart, windowEnd);

    std::vector<KnownBits> known;
    PayloadCheck check;
    {
        std::lock_guard<std::mutex> lock(knownBitsMutex);
        known = knownBits;
        check = payloadCheck;
    }

    std::vector<StreamDecode> found;
    std::vector<Heard> added;
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
        for (int pass = 0; pass < MAX_DECODES_PER_SEARCH; pass++)
        {
            detail::VoiceDecode result = detail::receiveVoice(z, info, hypotheses, first - 2 * geo.step - windowStart,
                                                              first + geo.hop - windowStart, MAX_OFFSET_HZ,
                                                              CANDIDATES, known);
            if (!result.haveCandidate) break;

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
                        keep(geo, voice, soft, softStart, activeScales[(size_t)soft.hypothesis], state.gear);
                }
            }
            if (!decoded.ok) break;
            // A kept copy of a frame that has now decoded is no use any more.
            forgetKept(geo.L, voice, start);

            // Take it out, and search again for a frame it was hiding. One
            // reported already (heard by an earlier, overlapping search, or
            // left over from a take-out that was not quite clean) comes out
            // again too.
            decoded.voice = voice;
            decoded.startSample = start;
            bool fresh = !alreadyReported(geo.L, voice, start);
            int removed = 0;
            for (const Heard& h : heard)
            {
                if (h.start < start + geo.frame && h.start + h.frameSamples > start) removed++;
            }
            addHeard(z, windowStart, state.gear, voice, decoded.scale, decoded);
            const Heard& now = heard.back();
            std::shared_ptr<const detail::VoiceTemplates> templates =
                detail::voiceTemplates(now.scale, now.voice, now.gear, activeTuning);
            detail::subtractFrame(z, *templates, info, now.start - windowStart, now.frequencyOffsetHz, now.payload);
            if (!fresh) continue;

            recent.push_back({geo.L, voice, start});
            added.push_back(now);
            StreamDecode decode;
            decode.gear = state.gear;
            decode.decode = decoded;
            decode.decode.framesRemoved = removed;
            found.push_back(decode);
        }
    }

    // List decode the candidates no search will find a better copy of now.
    listMature(known, check, found, added);

    // Decode again, without the frames just heard, the candidates they were
    // on top of; and so on for whatever that brings out.
    for (size_t i = 0; i < added.size() && i < 4 * (size_t)MAX_DECODES_PER_SEARCH; i++)
    {
        Heard over = added[i];
        retryKept(over, known, found, added);
    }

    // Forget kept copies too old to be averaged with.
    kept.erase(std::remove_if(kept.begin(), kept.end(),
                              [&](const Kept& k) {
                                  return k.start < first - AVERAGING_SECONDS * SAMPLE_RATE_HZ;
                              }),
               kept.end());

    // Forget heard frames that no audio still held overlaps.
    heard.erase(std::remove_if(heard.begin(), heard.end(),
                               [&](const Heard& h) { return h.start + h.frameSamples < historyStart; }),
                heard.end());

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

    // In the order the frames went on the air, voice 0 before voice 1 within
    // a duet frame (the link layer reassembles in that order). A frame
    // decoded again from under another can be older than one decoded first.
    // The two voices of a duet frame start together, give or take the
    // timing's few samples, so a voice 1 decode within 10 ms of a voice 0
    // decode at its tempo is placed by that one's start.
    std::vector<long long> onAir(found.size());
    for (size_t i = 0; i < found.size(); i++)
    {
        const Decode& d = found[i].decode;
        onAir[i] = d.startSample;
        if (d.voice != 1) continue;
        for (const StreamDecode& other : found)
        {
            if (other.decode.voice == 0 && other.gear == found[i].gear &&
                std::llabs(other.decode.startSample - d.startSample) <= SAMPLE_RATE_HZ / 100)
            {
                onAir[i] = other.decode.startSample;
                break;
            }
        }
    }
    std::vector<size_t> order(found.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (onAir[a] != onAir[b]) return onAir[a] < onAir[b];
        return found[a].decode.voice < found[b].decode.voice;
    });
    std::vector<StreamDecode> inOrder;
    inOrder.reserve(found.size());
    for (size_t i : order) inOrder.push_back(found[i]);
    found.swap(inOrder);
    DecodeCallback cb;
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        cb = callback;
    }
    for (const StreamDecode& decode : found)
    {
        long long end = decode.decode.startSample + gearInfo(decode.gear).frameSamples();
        if (end > lastFrameEnd) lastFrameEnd = end;
        if (cb) cb(decode);
    }
}

} // namespace Glissando
