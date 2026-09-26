//=========================================================================
// Name:            GlissandoReceiver.h
// Purpose:         Real-time Glissando decoding on a continuous audio stream.
//
// The audio thread pushes 8 kHz samples; a worker thread keeps the last
// frame and a bit of audio for each gear it listens to and, every quarter of
// that gear's frame, searches the new stretch for frames that have now
// arrived in full. A frame is reported once, however many searches cover it.
//=========================================================================

#ifndef GLISSANDO__GLISSANDO_RECEIVER_H
#define GLISSANDO__GLISSANDO_RECEIVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "GlissandoModem.h"

namespace Glissando
{

struct StreamDecode
{
    int gear = 0;
    Decode decode;          // startSample counts samples since start()
};

class StreamingReceiver
{
public:
    using DecodeCallback = std::function<void(const StreamDecode& decode)>;

    StreamingReceiver();
    ~StreamingReceiver();

    StreamingReceiver(const StreamingReceiver&) = delete;
    StreamingReceiver& operator=(const StreamingReceiver&) = delete;

    // Which gears to listen for (any of 1..5), in which scale and at which
    // tuning offset. Takes effect at the next search; buffered audio is kept.
    void configure(const std::vector<int>& gears, Scale scale, double tuningOffsetHz);

    // Called on the worker thread for every CRC-valid frame.
    void setDecodeCallback(DecodeCallback callback);

    void start();
    void stop();

    // From the audio thread. Copies and returns quickly; never blocks on a
    // search in progress. Samples are 16 bit at SAMPLE_RATE_HZ.
    void push(const short* samples, int numSamples);

    // Drop buffered audio, e.g. after our own transmission.
    void reset();

    // Blocks until every search due on the audio pushed so far has run and
    // reported. For tests and for decoding recordings; never call it from
    // the audio thread.
    void flush();

    // Samples pushed since start().
    long long samplesReceived() const;

    // Most recent channel report from any search that found a candidate,
    // decoded or not, and when (in samplesReceived() units). -1 if none.
    ChannelReport lastReport(long long* atSample = nullptr) const;

    // True when a frame ended within holdSamples of the newest sample
    // pushed: somebody is (or was just) on the channel. Carrier sense for
    // the chat protocol. Safe from any thread.
    bool isBusy(long long holdSamples) const;

private:
    // Implementation defined in the .cpp; kept opaque so the header stays
    // light.
    struct Impl;
    Impl* impl_;
};

} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_RECEIVER_H
