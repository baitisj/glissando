//=========================================================================
// Name:            Data2GTransport.h
// Purpose:         Carries chat through a separately running data2g-host:
//                  frames over its KISS port, and when the transmitter is
//                  keyed and the channel busy from its command port.
//
// data2g-host owns the sound card and the PTT; this only talks TCP to it.
// The operator starts it. Nothing of Data2G is built into this program.
//=========================================================================

#ifndef TEXT_MESSAGING__DATA2G_TRANSPORT_H
#define TEXT_MESSAGING__DATA2G_TRANSPORT_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Data2GLink.h"
#include "FrameCodec.h"
#include "TextMessagingProtocol.h"

namespace TextMessaging
{

class Data2GTransport : public ITextMessagingTransport
{
public:
    struct Settings
    {
        std::string host = "127.0.0.1";
        int kissPort = Data2G::DEFAULT_KISS_PORT;

        // The command port reports PTT and BUSY. data2g-host serves one
        // command client at a time, so a station that also runs VarAC or
        // Pat against the same host turns this off.
        bool useCommandPort = true;
        int commandPort = Data2G::DEFAULT_COMMAND_PORT;
    };

    struct Status
    {
        bool running = false;           // start() called, stop() not
        bool kissConnected = false;
        bool commandConnected = false;
        bool transmitting = false;      // data2g-host reports PTT on
        bool channelBusy = false;
        std::string mode;               // last MODE reported, empty if none
        std::string error;              // why the last connection failed
    };

    using FrameCallback = std::function<void(const Frame& frame, float snr)>;
    using LogFunction = std::function<void(const std::string& line)>;

    Data2GTransport();
    ~Data2GTransport() override;

    Data2GTransport(const Data2GTransport&) = delete;
    Data2GTransport& operator=(const Data2GTransport&) = delete;

    // Frames arrive on the transport's own thread. Set before start().
    void setFrameCallback(FrameCallback callback);
    void setLogFunction(LogFunction log);

    // Connects, and keeps reconnecting while data2g-host is not there.
    // Calling it again with other settings reconnects with those.
    void start(const Settings& settings);
    void stop();

    Status status() const;

    bool transmit(const std::vector<OutgoingBurst>& bursts) override;
    bool isTransmitting() const override;
    bool isChannelBusy() const override;

    // Test hooks: the clock the keying timers read.
    void setClock(std::function<uint64_t()> monotonicMs);

    // Without the command port, or when data2g-host never keys for what we
    // queued (it holds KISS traffic during an ARQ session or while the
    // channel is busy), a keying is taken as over after this long.
    static constexpr uint64_t NO_PTT_TIMEOUT_MS = 60000;

private:
    enum class Keying
    {
        Idle,
        Queued,     // written to the KISS port, not yet keyed
        OnAir,      // data2g-host reported PTT ON since we queued
    };

    void run(Settings settings);
    void log(const std::string& line);
    void handleCommandLine(const std::string& line);
    void handleKissFrame(const std::vector<uint8_t>& payload);
    uint64_t now() const;

    mutable std::mutex mutex_;
    Status status_;
    Keying keying_;
    uint64_t queuedAtMs_;
    bool pttOn_;

    std::mutex sendMutex_;
    int kissFd_;            // written by transmit(), owned by the thread

    FrameCallback frameCallback_;
    LogFunction log_;
    std::function<uint64_t()> clock_;

    std::atomic<bool> stopping_;
    std::thread thread_;
};

} // namespace TextMessaging

#endif // TEXT_MESSAGING__DATA2G_TRANSPORT_H
