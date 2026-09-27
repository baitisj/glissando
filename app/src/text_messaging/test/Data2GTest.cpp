//=========================================================================
// Name:            Data2GTest.cpp
// Purpose:         Chat over an external data2g-host: the KISS, AX.25 and
//                  command formats, and two chat stations exchanging a
//                  message through a fake pair of hosts on localhost.
//=========================================================================

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../Data2GLink.h"
#include "../Data2GTransport.h"
#include "../FrameCodec.h"
#include "../HeardStationList.h"
#include "../MessageStore.h"
#include "../TextMessagingProtocol.h"

using namespace TextMessaging;

namespace
{

int failures = 0;

void check(bool condition, const char* what, int line)
{
    if (!condition)
    {
        failures++;
        fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

template <typename Predicate>
bool waitFor(Predicate predicate, int milliseconds = 3000)
{
    for (int waited = 0; waited < milliseconds; waited += 5)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

std::vector<uint8_t> textFrame(const std::string& origin, const std::string& text)
{
    Frame frame;
    frame.type = FrameType::Broadcast;
    frame.originCallsign = origin;
    frame.airId = 0x1234;
    frame.payload.assign(text.begin(), text.end());
    return FrameCodec::encode(frame, TEXT_FRAME_BYTES);
}

//-------------------------------------------------------------------------
// Formats
//-------------------------------------------------------------------------

void testKissRoundTripsAwkwardBytes()
{
    std::vector<uint8_t> data = {0x01, 0xC0, 0xDB, 0xDC, 0xDD, 0xC0, 0xC0, 0x7F};
    std::vector<uint8_t> encoded = Data2G::kissEncode(data);
    CHECK(encoded.front() == 0xC0 && encoded.back() == 0xC0);
    CHECK(encoded[1] == 0x00);
    CHECK(std::count(encoded.begin() + 1, encoded.end() - 1, 0xC0) == 0);

    // Fed a byte at a time, between a stray FEND and a frame on another
    // KISS command (TXDELAY), which must be dropped.
    std::vector<uint8_t> stream = {0xC0, 0xC0, 0x01, 0x32, 0xC0};
    stream.insert(stream.end(), encoded.begin(), encoded.end());
    Data2G::KissDecoder decoder;
    std::vector<std::vector<uint8_t>> frames;
    for (uint8_t b : stream) decoder.feed(&b, 1, frames);
    CHECK(frames.size() == 1);
    CHECK(frames.size() == 1 && frames[0] == data);
}

void testAx25Addresses()
{
    CHECK(Data2G::ax25Address("vk2abc-7").call == "VK2ABC");
    CHECK(Data2G::ax25Address("vk2abc-7").ssid == 7);
    CHECK(Data2G::ax25Address("W1AW/P").call == "W1AW");
    CHECK(Data2G::ax25Address("W1AW-99").ssid == 0);
    CHECK(Data2G::ax25Address("ABCDEFGH").call == "ABCDEF");
    CHECK(Data2G::ax25Address("").call == "NOCALL");
}

void testUiFrameRoundTrip()
{
    std::vector<uint8_t> info = {1, 2, 3};
    std::vector<uint8_t> frame = Data2G::ax25UiFrame("K2XYZ-3", "GLISS", info);
    CHECK(frame.size() == 14 + 2 + 3);
    CHECK(frame[14] == 0x03 && frame[15] == 0xF0);
    CHECK((frame[6] & 0x80) != 0);  // command: C bit on the destination
    CHECK((frame[13] & 0x01) != 0); // source is the last address

    Data2G::Ax25Ui ui;
    CHECK(Data2G::parseAx25Ui(frame, ui));
    CHECK(ui.source.call == "K2XYZ" && ui.source.ssid == 3);
    CHECK(ui.destination.call == "GLISS");
    CHECK(ui.info == info);

    // A connected-mode I frame is not ours to read.
    std::vector<uint8_t> iframe = frame;
    iframe[14] = 0x00;
    CHECK(!Data2G::parseAx25Ui(iframe, ui));
}

void testChatPayloadTagging()
{
    std::vector<uint8_t> chat = textFrame("W1AW", "hi");
    std::vector<uint8_t> payload = Data2G::payloadForChatFrame("W1AW", chat);
    std::vector<uint8_t> back;
    CHECK(Data2G::chatFrameFromPayload(payload, back));
    CHECK(back == chat);

    // An APRS-looking UI frame to another destination, and one to ours
    // without the tag, are both somebody else's.
    std::vector<uint8_t> aprs = Data2G::ax25UiFrame("W1AW", "APRS", chat);
    CHECK(!Data2G::chatFrameFromPayload(aprs, back));
    std::vector<uint8_t> untagged = Data2G::ax25UiFrame("W1AW", "GLISS", chat);
    CHECK(!Data2G::chatFrameFromPayload(untagged, back));
}

void testCommandLines()
{
    Data2G::LineSplitter splitter;
    std::vector<std::string> lines;
    const char* part1 = "PTT ON\rBUSY O";
    const char* part2 = "FF\rMODE n10-qpsk-r1/5\rBUFFER 0\r";
    splitter.feed(part1, (int)strlen(part1), lines);
    splitter.feed(part2, (int)strlen(part2), lines);
    CHECK(lines.size() == 4);
    if (lines.size() != 4) return;

    Data2G::CommandEvent ptt = Data2G::parseCommandLine(lines[0]);
    CHECK(ptt.type == Data2G::CommandEvent::Type::Ptt && ptt.on);
    Data2G::CommandEvent busy = Data2G::parseCommandLine(lines[1]);
    CHECK(busy.type == Data2G::CommandEvent::Type::Busy && !busy.on);
    Data2G::CommandEvent mode = Data2G::parseCommandLine(lines[2]);
    CHECK(mode.type == Data2G::CommandEvent::Type::Mode && mode.mode == "n10-qpsk-r1/5");
    CHECK(Data2G::parseCommandLine(lines[3]).type == Data2G::CommandEvent::Type::Other);
}

void testTimingCoversAWholeBurst()
{
    AirTiming timing = Data2G::airTiming();
    AirTiming codec2;
    CHECK(timing.ackTimeoutMs > codec2.ackTimeoutMs + 12000);
    CHECK(timing.textFragmentAirMs >= 12000);
    CHECK(timing.reassemblyTimeoutMs >= codec2.reassemblyTimeoutMs);
}

//-------------------------------------------------------------------------
// A fake data2g-host pair. Each side has a KISS and a command listener;
// whatever a side's KISS client sends is "transmitted": that side reports
// PTT ON, the frames reach the other side's KISS client, and PTT OFF
// follows, with the other side reporting BUSY around it.
//-------------------------------------------------------------------------

int listenOnLoopback(int& portOut)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(fd, (sockaddr*)&addr, sizeof(addr));
    listen(fd, 4);
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr*)&addr, &len);
    portOut = ntohs(addr.sin_port);
    return fd;
}

class FakeHostPair
{
public:
    struct Side
    {
        int kissListen = -1, commandListen = -1;
        int kissPort = 0, commandPort = 0;
        int kissClient = -1, commandClient = -1;
        int bursts = 0;
        std::vector<std::string> commandsHeard;
    };

    FakeHostPair()
    {
        for (Side& side : sides)
        {
            side.kissListen = listenOnLoopback(side.kissPort);
            side.commandListen = listenOnLoopback(side.commandPort);
        }
        thread = std::thread([this]() { run(); });
    }

    ~FakeHostPair()
    {
        stopping = true;
        thread.join();
        for (Side& side : sides)
        {
            for (int fd : {side.kissListen, side.commandListen, side.kissClient, side.commandClient})
            {
                if (fd >= 0) close(fd);
            }
        }
    }

    int bursts(int side)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return sides[side].bursts;
    }

    bool connected(int side)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return sides[side].kissClient >= 0 && sides[side].commandClient >= 0;
    }

    void sayOnCommand(int side, const char* line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        say(sides[side].commandClient, line);
    }

    // Drops side's command client, as data2g-host going away would.
    void dropCommandClient(int side)
    {
        std::lock_guard<std::mutex> lock(mutex);
        close(sides[side].commandClient);
        sides[side].commandClient = -1;
    }

    // While set, a side holds what it is sent instead of keying.
    std::atomic<bool> holdTransmissions{false};

    Side sides[2];

private:
    void say(int fd, const char* line)
    {
        if (fd >= 0) send(fd, line, strlen(line), MSG_NOSIGNAL);
    }

    void run()
    {
        std::vector<uint8_t> buffer(4096);
        while (!stopping)
        {
            std::vector<pollfd> fds;
            std::vector<std::pair<int, int>> what; // side, 0 kiss listen 1 cmd listen 2 kiss client 3 cmd client
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (int s = 0; s < 2; s++)
                {
                    Side& side = sides[s];
                    fds.push_back({side.kissListen, POLLIN, 0});
                    what.push_back({s, 0});
                    fds.push_back({side.commandListen, POLLIN, 0});
                    what.push_back({s, 1});
                    if (side.kissClient >= 0)
                    {
                        fds.push_back({side.kissClient, POLLIN, 0});
                        what.push_back({s, 2});
                    }
                    if (side.commandClient >= 0)
                    {
                        fds.push_back({side.commandClient, POLLIN, 0});
                        what.push_back({s, 3});
                    }
                }
            }
            if (poll(fds.data(), (nfds_t)fds.size(), 20) <= 0) continue;

            std::lock_guard<std::mutex> lock(mutex);
            for (size_t i = 0; i < fds.size(); i++)
            {
                if (fds[i].revents == 0) continue;
                Side& side = sides[what[i].first];
                Side& other = sides[1 - what[i].first];
                switch (what[i].second)
                {
                    case 0:
                        if (side.kissClient >= 0) close(side.kissClient);
                        side.kissClient = accept(side.kissListen, nullptr, nullptr);
                        break;
                    case 1:
                        if (side.commandClient >= 0) close(side.commandClient);
                        side.commandClient = accept(side.commandListen, nullptr, nullptr);
                        say(side.commandClient, "IAMALIVE\r");
                        break;
                    case 2:
                    {
                        ssize_t got = recv(side.kissClient, buffer.data(), buffer.size(), 0);
                        if (got <= 0)
                        {
                            close(side.kissClient);
                            side.kissClient = -1;
                            break;
                        }
                        if (holdTransmissions) break;
                        side.bursts++;
                        say(side.commandClient, "PTT ON\rMODE qpsk-r1/5\r");
                        say(other.commandClient, "BUSY ON\r");
                        // An APRS frame from some other program on the far
                        // host, which chat must ignore.
                        std::vector<uint8_t> aprs = Data2G::kissEncode(
                            Data2G::ax25UiFrame("N0CALL", "APRS", {'!', '1'}));
                        if (other.kissClient >= 0)
                        {
                            send(other.kissClient, aprs.data(), aprs.size(), MSG_NOSIGNAL);
                            send(other.kissClient, buffer.data(), (size_t)got, MSG_NOSIGNAL);
                        }
                        say(other.commandClient, "BUSY OFF\r");
                        say(side.commandClient, "PTT OFF\r");
                        break;
                    }
                    case 3:
                    {
                        ssize_t got = recv(side.commandClient, buffer.data(), buffer.size(), 0);
                        if (got <= 0)
                        {
                            close(side.commandClient);
                            side.commandClient = -1;
                            break;
                        }
                        side.commandsHeard.emplace_back((const char*)buffer.data(), (size_t)got);
                        break;
                    }
                }
            }
        }
    }

    std::mutex mutex;
    std::atomic<bool> stopping{false};
    std::thread thread;
};

class RecordingObserver : public ITextMessagingObserver
{
public:
    void onMessageAdded(const TextMessage& message) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        added.push_back(message);
    }
    void onMessageUpdated(const TextMessage& message) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        updated.push_back(message);
    }
    void onStationsChanged() override {}

    bool sawStatus(MessageStatus status)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const TextMessage& m : updated)
        {
            if (m.status == status) return true;
        }
        return false;
    }

    std::vector<TextMessage> receivedTexts()
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<TextMessage> out;
        for (const TextMessage& m : added)
        {
            if (m.direction == MessageDirection::Received) out.push_back(m);
        }
        return out;
    }

    std::mutex mutex;
    std::vector<TextMessage> added;
    std::vector<TextMessage> updated;
};

// A chat station on a Data2GTransport, with a clock the test runs fast so
// the protocol's turnarounds pass in a blink.
struct Station
{
    Station(const std::string& callsign, const FakeHostPair::Side& host, bool useCommandPort = true)
        : protocol(store, stations)
    {
        CHECK(store.open(":memory:"));
        protocol.setTransport(&transport);
        protocol.setObserver(&observer);
        protocol.setMyCallsign(callsign);
        protocol.setAirTiming(Data2G::airTiming());
        protocol.setClocks([this]() { return nowMs.load(); }, []() { return (std::time_t)1750000000; });
        transport.setClock([this]() { return nowMs.load(); });
        transport.setFrameCallback(
            [this](const Frame& frame, float snr) { protocol.onFrameReceived(frame, snr); });

        Data2GTransport::Settings settings;
        settings.kissPort = host.kissPort;
        settings.commandPort = host.commandPort;
        settings.useCommandPort = useCommandPort;
        transport.start(settings);
    }

    ~Station() { transport.stop(); }

    void step(uint64_t ms = 100)
    {
        nowMs += ms;
        protocol.tick();
    }

    MessageStore store;
    HeardStationList stations;
    RecordingObserver observer;
    Data2GTransport transport;
    TextMessagingProtocol protocol;
    std::atomic<uint64_t> nowMs{1000};
};

void runBoth(Station& a, Station& b, const std::function<bool()>& done, int steps = 3000)
{
    for (int i = 0; i < steps && !done(); i++)
    {
        a.step();
        b.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void testMessageIsDeliveredAndAcknowledged()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0]);
    Station b("VK3ABC", hosts.sides[1]);
    CHECK(waitFor([&]() { return hosts.connected(0) && hosts.connected(1); }));
    CHECK(waitFor([&]() { return a.transport.status().commandConnected; }));

    std::string error;
    CHECK(a.protocol.sendMessage("Hello through Data2G", "VK3ABC", error));
    runBoth(a, b, [&]() { return a.observer.sawStatus(MessageStatus::Acknowledged); });

    CHECK(a.observer.sawStatus(MessageStatus::Acknowledged));
    std::vector<TextMessage> got = b.observer.receivedTexts();
    CHECK(got.size() == 1);
    CHECK(got.size() == 1 && got[0].text == "Hello through Data2G");
    CHECK(got.size() == 1 && got[0].originCallsign == "W1AW");
    CHECK(got.size() == 1 && std::isnan(got[0].snr)); // Data2G reports none
    CHECK(b.stations.contains("W1AW"));
    CHECK(hosts.bursts(0) == 1); // the message, once
    CHECK(hosts.bursts(1) == 1); // the acknowledgement
    CHECK(a.transport.status().mode == "qpsk-r1/5");
    CHECK(!a.transport.isTransmitting());
    // Nothing is said on the command port that could change the host's state.
    CHECK(hosts.sides[0].commandsHeard.empty());
}

void testNotConnectedRefusesToTransmit()
{
    Data2GTransport transport;
    OutgoingBurst burst;
    burst.frame = textFrame("W1AW", "x");
    CHECK(!transport.transmit({burst}));

    // Nobody listening on the port: it keeps trying, and says why.
    int port = 0;
    int fd = listenOnLoopback(port);
    close(fd);
    Data2GTransport::Settings settings;
    settings.kissPort = port;
    settings.useCommandPort = false;
    transport.start(settings);
    CHECK(waitFor([&]() { return !transport.status().error.empty(); }));
    CHECK(!transport.status().kissConnected);
    CHECK(!transport.transmit({burst}));
    transport.stop();
}

void testWithoutCommandPortTheBurstIsEstimated()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], false);
    CHECK(waitFor([&]() { return a.transport.status().kissConnected; }));

    OutgoingBurst burst;
    burst.frame = textFrame("W1AW", "estimate");
    CHECK(a.transport.transmit({burst}));
    CHECK(a.transport.isTransmitting());
    CHECK(!a.transport.isChannelBusy());
    a.nowMs += Data2G::ESTIMATED_BURST_MILLISECONDS - 1;
    CHECK(a.transport.isTransmitting());
    a.nowMs += 2;
    CHECK(!a.transport.isTransmitting());
}

void testAKeyingThatNeverAirsIsGivenUp()
{
    FakeHostPair hosts;
    hosts.holdTransmissions = true;
    Station a("W1AW", hosts.sides[0]);
    CHECK(waitFor([&]() { return a.transport.status().commandConnected && a.transport.status().kissConnected; }));

    OutgoingBurst burst;
    burst.frame = textFrame("W1AW", "held");
    CHECK(a.transport.transmit({burst}));
    a.nowMs += Data2G::ESTIMATED_BURST_MILLISECONDS + 1;
    CHECK(a.transport.isTransmitting()); // PTT reports are expected, so it waits longer
    a.nowMs += Data2GTransport::NO_PTT_TIMEOUT_MS;
    CHECK(!a.transport.isTransmitting());
}

void testLosingTheCommandPortClearsBusy()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0]);
    Station b("VK3ABC", hosts.sides[1]);
    CHECK(waitFor([&]() { return hosts.connected(0) && hosts.connected(1); }));
    CHECK(waitFor([&]() { return b.transport.status().commandConnected; }));

    // B's host says BUSY ON, then goes away without saying BUSY OFF.
    hosts.sayOnCommand(1, "BUSY ON\r");
    CHECK(waitFor([&]() { return b.transport.isChannelBusy(); }));
    hosts.dropCommandClient(1);
    CHECK(waitFor([&]() { return !b.transport.isChannelBusy(); }));
    // And it comes back on its own.
    CHECK(waitFor([&]() { return b.transport.status().commandConnected; }, 5000));
}

} // namespace

int main()
{
    testKissRoundTripsAwkwardBytes();
    testAx25Addresses();
    testUiFrameRoundTrip();
    testChatPayloadTagging();
    testCommandLines();
    testTimingCoversAWholeBurst();
    testNotConnectedRefusesToTransmit();
    testMessageIsDeliveredAndAcknowledged();
    testWithoutCommandPortTheBurstIsEstimated();
    testAKeyingThatNeverAirsIsGivenUp();
    testLosingTheCommandPortClearsBusy();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    printf("all Data2G checks passed\n");
    return 0;
}
