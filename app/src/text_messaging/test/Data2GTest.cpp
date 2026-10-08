//=========================================================================
// Name:            Data2GTest.cpp
// Purpose:         Chat over an external data2g-host: the KISS, command,
//                  mode and session-stream formats, and two chat stations
//                  talking through a fake pair of hosts on localhost, over
//                  the GLISS broadcast group and over connected sessions.
//=========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
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
    frame.airId = 0x234;
    frame.payload.assign(text.begin(), text.end());
    return FrameCodec::encode(frame, TEXT_FRAME_BYTES);
}

std::vector<Data2G::ModeInfo> narrowModes()
{
    std::vector<Data2G::ModeInfo> out;
    for (const Data2G::ModeInfo& mode : Data2G::knownModes())
    {
        if (mode.bandwidthHz <= 500) out.push_back(mode);
    }
    return out;
}

//-------------------------------------------------------------------------
// Formats
//-------------------------------------------------------------------------

void testKissPortsAndAckMode()
{
    std::vector<uint8_t> data = {0x01, 0xC0, 0xDB, 0x7F};
    std::vector<uint8_t> stream = Data2G::kissEncode(3, data);
    std::vector<uint8_t> ack = Data2G::kissEncodeAckMode(12, 0xC0DB, data); // 12: the command byte is FEND
    stream.insert(stream.end(), ack.begin(), ack.end());
    // A TXDELAY command, which is not passed on.
    std::vector<uint8_t> txdelay = {0xC0, 0x01, 0x32, 0xC0};
    stream.insert(stream.end(), txdelay.begin(), txdelay.end());

    Data2G::KissDecoder decoder;
    std::vector<Data2G::KissFrame> frames;
    for (uint8_t b : stream) decoder.feed(&b, 1, frames);
    CHECK(frames.size() == 2);
    if (frames.size() != 2) return;
    CHECK(frames[0].port == 3 && frames[0].command == Data2G::KISS_DATA && frames[0].payload == data);
    CHECK(frames[1].port == 12 && frames[1].command == Data2G::KISS_ACKMODE);
    std::vector<uint8_t> tagged = {0xC0, 0xDB};
    tagged.insert(tagged.end(), data.begin(), data.end());
    CHECK(frames[1].payload == tagged);
}

void testCommandLines()
{
    Data2G::LineSplitter splitter;
    std::vector<std::string> lines;
    const char* part1 = "PTT ON\rBUSY O";
    const char* part2 = "FF\rMODE n10-qpsk-r1/5\rMODE qpsk-r1/3 1200 76 64 1.78 74.36\rBCAST PORT 2\r"
                        "BCAST 2 HEARD VK3ABC\rBCAST 2 DROPPED 3\rCONNECTED W1AW VK3ABC 2300\rBUFFER 120\rOK\r";
    splitter.feed(part1, (int)strlen(part1), lines);
    splitter.feed(part2, (int)strlen(part2), lines);
    CHECK(lines.size() == 10);
    if (lines.size() != 10) return;

    using Type = Data2G::CommandEvent::Type;
    Data2G::CommandEvent e = Data2G::parseCommandLine(lines[0]);
    CHECK(e.type == Type::Ptt && e.on);
    e = Data2G::parseCommandLine(lines[1]);
    CHECK(e.type == Type::Busy && !e.on);
    e = Data2G::parseCommandLine(lines[2]);
    CHECK(e.type == Type::Mode && e.text == "n10-qpsk-r1/5");
    e = Data2G::parseCommandLine(lines[3]);
    CHECK(e.type == Type::ModeLine && e.modeInfo.name == "qpsk-r1/3" && e.modeInfo.bandwidthHz == 1200 &&
          e.modeInfo.bytesPerCodeword == 76 && e.modeInfo.maxCodewords == 64 &&
          std::fabs(e.modeInfo.secondsAtMax - 74.36) < 1e-6);
    e = Data2G::parseCommandLine(lines[4]);
    CHECK(e.type == Type::BcastPort && e.number == 2);
    e = Data2G::parseCommandLine(lines[5]);
    CHECK(e.type == Type::BcastHeard && e.number == 2 && e.text == "VK3ABC");
    e = Data2G::parseCommandLine(lines[6]);
    CHECK(e.type == Type::BcastDropped && e.count == 3);
    e = Data2G::parseCommandLine(lines[7]);
    CHECK(e.type == Type::Connected && e.text == "W1AW" && e.peer == "VK3ABC");
    e = Data2G::parseCommandLine(lines[8]);
    CHECK(e.type == Type::Buffer && e.count == 120);
    CHECK(Data2G::parseCommandLine(lines[9]).type == Type::Ok);
    CHECK(Data2G::parseCommandLine("IAMALIVE").type == Type::Other);
}

void testTemposMapToModes()
{
    const auto& full = Data2G::knownModes();
    CHECK(Data2G::modeForGear(1, full).name == "fsk16r25-r1/3"); // Adagio: the most sensitive
    CHECK(Data2G::modeForGear(5, full).name == "w48-16qam-r1/2"); // Duet: a fast 2.3 kHz mode
    CHECK(Data2G::modeForGear(5, full).bandwidthHz >= 2300);

    // A host capped at 500 Hz keeps every tempo within it.
    std::vector<Data2G::ModeInfo> narrow = narrowModes();
    for (int gear = 1; gear <= 5; gear++)
    {
        Data2G::ModeInfo mode = Data2G::modeForGear(gear, narrow);
        CHECK(mode.valid() && mode.bandwidthHz <= 500);
    }
    CHECK(Data2G::modeForGear(5, narrow).name == "n10-16qam-r1/2");

    // Slower tempos take longer on the air, and wait longer for an answer.
    std::vector<int> message(1, TEXT_FRAME_BYTES);
    double previous = 1e9;
    for (int gear = 1; gear <= 5; gear++)
    {
        double seconds = Data2G::modeForGear(gear, full).burstSeconds(message);
        CHECK(seconds > 0.0 && seconds <= previous);
        previous = seconds;
    }
    CHECK(Data2G::airTiming(Data2G::modeForGear(1, full)).ackTimeoutMs >
          Data2G::airTiming(Data2G::modeForGear(5, full)).ackTimeoutMs);
}

void testSessionStream()
{
    std::vector<uint8_t> a = textFrame("W1AW", "one");
    std::vector<uint8_t> b = textFrame("W1AW", "two");
    std::vector<uint8_t> bytes = Data2G::streamEncode(a);
    std::vector<uint8_t> second = Data2G::streamEncode(b);
    bytes.insert(bytes.end(), second.begin(), second.end());

    Data2G::StreamDecoder decoder;
    std::vector<std::vector<uint8_t>> frames;
    for (uint8_t byte : bytes) CHECK(decoder.feed(&byte, 1, frames));
    CHECK(frames.size() == 2 && frames[0] == a && frames[1] == b);

    // A Winlink B2F stream is not ours.
    Data2G::StreamDecoder other;
    const char* b2f = "[WL2K-5.0-B2FWIHJM$]\r";
    frames.clear();
    CHECK(!other.feed((const uint8_t*)b2f, (int)strlen(b2f), frames));
    CHECK(frames.empty());
}

void testCommandCallsign()
{
    CHECK(Data2G::commandCallsign("vk2abc/p") == "VK2ABC/P");
    CHECK(Data2G::commandCallsign("W1AW-7 ") == "W1AW-7");
    CHECK(Data2G::commandCallsign("ABCDEFGHIJKL") == "ABCDEFGHIJ");
}

//-------------------------------------------------------------------------
// A fake pair of data2g-hosts. Each side has KISS, command and session
// data listeners. A burst one side's client sends is "transmitted": that
// side reports PTT ON, the mode and PTT OFF and acknowledges the ACKMODE
// tags, and the frames reach the other side's client on its GLISS port, or
// in its session.
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
        int listen[3] = {-1, -1, -1};   // KISS, command, data
        int client[3] = {-1, -1, -1};
        int kissPort = 0, commandPort = 0, dataPort = 0;

        std::vector<Data2G::ModeInfo> modes = Data2G::knownModes();
        std::string call;
        bool listening = false;
        int groupPort = 0;
        std::string groupMode = "qpsk-r1/5";
        std::vector<std::string> commands;      // every command line heard
        std::vector<std::string> burstModes;    // the mode of every GLISS burst sent
        int sessionBursts = 0;

        // Session bytes written to this side and not yet acknowledged by
        // the far end. With exactBuffer, BUFFER counts them as Data2G does
        // for a CHAT ON client since its PR #51; without, it says 1 for any,
        // as before. holdAcks keeps them until the test lets them through.
        std::vector<uint8_t> unacked;
        bool exactBuffer = true;
        bool holdAcks = false;
        Data2G::KissDecoder kiss;
        Data2G::LineSplitter lines;
    };

    FakeHostPair()
    {
        for (Side& side : sides)
        {
            side.listen[0] = listenOnLoopback(side.kissPort);
            side.listen[1] = listenOnLoopback(side.commandPort);
            // The data port is the command port's next; find a free pair.
            for (;;)
            {
                int dataPort = side.commandPort + 1;
                int fd = socket(AF_INET, SOCK_STREAM, 0);
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = htons((uint16_t)dataPort);
                if (bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0)
                {
                    listen(fd, 4);
                    side.listen[2] = fd;
                    side.dataPort = dataPort;
                    break;
                }
                close(fd);
                close(side.listen[1]);
                side.listen[1] = listenOnLoopback(side.commandPort);
            }
        }
        thread = std::thread([this]() { run(); });
    }

    ~FakeHostPair()
    {
        stopping = true;
        thread.join();
        for (Side& side : sides)
        {
            for (int i = 0; i < 3; i++)
            {
                if (side.listen[i] >= 0) close(side.listen[i]);
                if (side.client[i] >= 0) close(side.client[i]);
            }
        }
    }

    template <typename Function>
    auto with(Function f)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return f();
    }

    bool heardCommand(int side, const std::string& prefix)
    {
        return with([&]() {
            for (const std::string& c : sides[side].commands)
            {
                if (c.compare(0, prefix.size(), prefix) == 0) return true;
            }
            return false;
        });
    }

    bool ready(int side)
    {
        return with([&]() { return sides[side].groupPort != 0 && sides[side].client[0] >= 0; });
    }

    void dropCommandClient(int side)
    {
        with([&]() {
            close(sides[side].client[1]);
            sides[side].client[1] = -1;
            closeGroup(sides[side]);
            return 0;
        });
    }

    // The far end's modem takes in and acknowledges the first n bytes
    // waiting on this side.
    void ackSome(int side, size_t n)
    {
        with([&]() {
            deliverPending(side, n);
            return 0;
        });
    }

    size_t unackedBytes(int side)
    {
        return with([&]() { return sides[side].unacked.size(); });
    }

    // The link fails: both ends are told, what was unacknowledged is gone,
    // and the host, listening again, reports an empty buffer.
    void loseSession()
    {
        with([&]() {
            if (sessionWith < 0) return 0;
            for (Side& side : sides)
            {
                say(side, "DISCONNECTED");
                side.unacked.clear();
                say(side, "BUFFER 0");
            }
            sessionWith = -1;
            return 0;
        });
    }

    void sayOnCommand(int side, const std::string& line)
    {
        with([&]() {
            say(sides[side], line);
            return 0;
        });
    }

    Side sides[2];

private:
    void say(Side& side, const std::string& line)
    {
        if (side.client[1] < 0) return;
        std::string text = line + "\r";
        send(side.client[1], text.data(), text.size(), MSG_NOSIGNAL);
    }

    void sendBytes(int fd, const std::vector<uint8_t>& bytes)
    {
        if (fd >= 0) send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    }

    void closeGroup(Side& side)
    {
        side.groupPort = 0;
        side.listening = false;
        if (sessionWith >= 0)
        {
            say(sides[0], "DISCONNECTED");
            say(sides[1], "DISCONNECTED");
            sessionWith = -1;
        }
    }

    void command(int s, const std::string& line)
    {
        Side& side = sides[s];
        Side& other = sides[1 - s];
        side.commands.push_back(line);

        std::vector<std::string> w;
        {
            std::string word;
            for (char c : line)
            {
                if (c == ' ')
                {
                    if (!word.empty()) w.push_back(word);
                    word.clear();
                }
                else
                {
                    word.push_back(c);
                }
            }
            if (!word.empty()) w.push_back(word);
        }
        if (w.empty()) return;

        if (w[0] == "MYCALL" && w.size() == 2)
        {
            side.call = w[1];
            say(side, "OK");
        }
        else if (w[0] == "LISTEN")
        {
            side.listening = w.size() == 2 && w[1] == "ON";
            say(side, "OK");
        }
        else if (w[0] == "CHAT")
        {
            say(side, "OK");
        }
        else if (w[0] == "BCAST" && w.size() >= 3 && w[1] == "OPEN")
        {
            side.groupPort = 1;
            say(side, "BCAST PORT 1");
        }
        else if (w[0] == "BCAST" && w.size() == 4 && w[1] == "MODE")
        {
            bool offered = std::any_of(side.modes.begin(), side.modes.end(),
                                       [&](const Data2G::ModeInfo& m) { return m.name == w[3]; });
            if (offered) side.groupMode = w[3];
            say(side, offered ? "OK" : "WRONG");
        }
        else if (w[0] == "MODES")
        {
            for (const Data2G::ModeInfo& m : side.modes)
            {
                char text[200];
                snprintf(text, sizeof(text), "MODE %s %d %d %d %.2f %.2f", m.name.c_str(), m.bandwidthHz,
                         m.bytesPerCodeword, m.maxCodewords, m.secondsAtOne, m.secondsAtMax);
                say(side, text);
            }
            say(side, "OK");
        }
        else if (w[0] == "CONNECT" && w.size() == 3)
        {
            say(side, "OK");
            if (other.listening && other.call == w[2] && sessionWith < 0)
            {
                sessionWith = s;
                say(side, "CONNECTED " + w[1] + " " + w[2] + " 2300");
                say(other, "CONNECTED " + w[1] + " " + w[2] + " 2300");
            }
            else
            {
                say(side, "DISCONNECTED");
            }
        }
        else if (w[0] == "DISCONNECT" || w[0] == "ABORT")
        {
            say(side, "OK");
            if (sessionWith >= 0)
            {
                say(side, "DISCONNECTED");
                say(other, "DISCONNECTED");
                sessionWith = -1;
            }
        }
        else
        {
            say(side, "WRONG");
        }
    }

    void kissIn(int s, const uint8_t* bytes, int length)
    {
        Side& side = sides[s];
        Side& other = sides[1 - s];
        std::vector<Data2G::KissFrame> frames;
        side.kiss.feed(bytes, length, frames);
        if (frames.empty()) return;

        // Everything that came in together is one burst.
        say(side, "PTT ON");
        say(side, "MODE " + side.groupMode);
        say(other, "BUSY ON");
        std::vector<uint16_t> tags;
        for (const Data2G::KissFrame& frame : frames)
        {
            if (frame.port != side.groupPort || frame.payload.size() < 2) continue;
            std::vector<uint8_t> data = frame.payload;
            if (frame.command == Data2G::KISS_ACKMODE)
            {
                tags.push_back((uint16_t)((data[0] << 8) | data[1]));
                data.erase(data.begin(), data.begin() + 2);
            }
            if (other.groupPort != 0) sendBytes(other.client[0], Data2G::kissEncode(other.groupPort, data));
        }
        side.burstModes.push_back(side.groupMode);
        say(other, "BCAST " + std::to_string(other.groupPort) + " HEARD " + side.call);
        say(other, "BUSY OFF");
        say(side, "PTT OFF");
        for (uint16_t tag : tags) sendBytes(side.client[0], Data2G::kissEncodeAckMode(side.groupPort, tag, {}));
    }

    void sayBuffer(Side& side)
    {
        size_t n = side.unacked.size();
        say(side, "BUFFER " + std::to_string(side.exactBuffer ? n : std::min<size_t>(n, 1)));
    }

    void dataIn(int s, const uint8_t* bytes, int length)
    {
        Side& side = sides[s];
        if (sessionWith < 0) return;
        side.unacked.insert(side.unacked.end(), bytes, bytes + length);
        sayBuffer(side); // data2g-host answers every write with one
        if (!side.holdAcks) deliverPending(s, side.unacked.size());
    }

    // A burst carrying the first n waiting bytes, and the far end's
    // acknowledgement of them.
    void deliverPending(int s, size_t n)
    {
        Side& side = sides[s];
        Side& other = sides[1 - s];
        n = std::min(n, side.unacked.size());
        if (n == 0 || sessionWith < 0) return;
        say(side, "PTT ON");
        sendBytes(other.client[2], std::vector<uint8_t>(side.unacked.begin(), side.unacked.begin() + (long)n));
        say(side, "PTT OFF");
        side.unacked.erase(side.unacked.begin(), side.unacked.begin() + (long)n);
        side.sessionBursts++;
        sayBuffer(side);
    }

    void run()
    {
        std::vector<uint8_t> buffer(4096);
        while (!stopping)
        {
            std::vector<pollfd> fds;
            std::vector<std::pair<int, int>> what; // side, socket (0-2 listen, 3-5 client)
            with([&]() {
                for (int s = 0; s < 2; s++)
                {
                    for (int i = 0; i < 3; i++)
                    {
                        fds.push_back({sides[s].listen[i], POLLIN, 0});
                        what.push_back({s, i});
                        if (sides[s].client[i] >= 0)
                        {
                            fds.push_back({sides[s].client[i], POLLIN, 0});
                            what.push_back({s, 3 + i});
                        }
                    }
                }
                return 0;
            });
            if (poll(fds.data(), (nfds_t)fds.size(), 20) <= 0) continue;

            std::lock_guard<std::mutex> lock(mutex);
            for (size_t i = 0; i < fds.size(); i++)
            {
                if (fds[i].revents == 0) continue;
                int s = what[i].first;
                Side& side = sides[s];
                int which = what[i].second;
                if (which < 3)
                {
                    if (side.client[which] >= 0) close(side.client[which]);
                    side.client[which] = accept(side.listen[which], nullptr, nullptr);
                    if (which == 1) say(side, "IAMALIVE");
                    continue;
                }

                int c = which - 3;
                if (side.client[c] < 0) continue;
                ssize_t got = recv(side.client[c], buffer.data(), buffer.size(), 0);
                if (got <= 0)
                {
                    close(side.client[c]);
                    side.client[c] = -1;
                    if (c == 1) closeGroup(side);
                    continue;
                }

                if (c == 0)
                {
                    kissIn(s, buffer.data(), (int)got);
                }
                else if (c == 1)
                {
                    std::vector<std::string> lines;
                    side.lines.feed((const char*)buffer.data(), (int)got, lines);
                    for (const std::string& line : lines) command(s, line);
                }
                else
                {
                    dataIn(s, buffer.data(), (int)got);
                }
            }
        }
    }

    std::mutex mutex;
    std::atomic<bool> stopping{false};
    int sessionWith = -1;   // the side that called, while a session is up
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

    MessageStatus statusOf(int64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex);
        MessageStatus status = MessageStatus::Queued;
        for (const TextMessage& m : updated)
        {
            if (m.id == id) status = m.status;
        }
        return status;
    }

    int64_t lastAddedId()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return added.empty() ? 0 : added.back().id;
    }

    bool sawSystemLine(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const TextMessage& m : added)
        {
            if (m.kind == MessageKind::System && m.text.find(text) != std::string::npos) return true;
        }
        return false;
    }

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
    Station(const std::string& callsign, const FakeHostPair::Side& host, int gear, bool useSessions = true)
        : protocol(store, stations)
    {
        CHECK(store.open(":memory:"));
        protocol.setTransport(&transport);
        protocol.setObserver(&observer);
        protocol.setMyCallsign(callsign);
        protocol.setClocks([this]() { return nowMs.load(); }, []() { return (std::time_t)1750000000; });
        transport.setClock([this]() { return nowMs.load(); });
        transport.setFrameCallback([this](const Frame& frame, float snr, bool viaSession)
                                   { protocol.onFrameReceived(frame, snr, viaSession); });
        transport.setMyCallsign(callsign);
        transport.setGear(gear);
        protocol.setAirTiming(transport.airTiming());

        Data2GTransport::Settings settings;
        settings.kissPort = host.kissPort;
        settings.commandPort = host.commandPort;
        settings.useSessions = useSessions;
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

void runBoth(Station& a, Station& b, const std::function<bool()>& done, int steps = 6000)
{
    for (int i = 0; i < steps && !done(); i++)
    {
        a.step();
        b.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool settled(FakeHostPair& hosts, Station& a, Station& b)
{
    return waitFor([&]() {
        return hosts.ready(0) && hosts.ready(1) && a.transport.status().groupPort != 0 &&
               b.transport.status().groupPort != 0;
    });
}

void testBroadcastGoesToTheGroupAtAdagio()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 1);
    Station b("VK3ABC", hosts.sides[1], 1);
    CHECK(settled(hosts, a, b));
    CHECK(hosts.heardCommand(0, "BCAST OPEN GLISS FROM W1AW"));
    CHECK(hosts.heardCommand(0, "MODES"));

    std::string error;
    CHECK(a.protocol.sendMessage("CQ from the tortoise", "", error));
    runBoth(a, b, [&]() { return !b.observer.receivedTexts().empty(); });

    std::vector<TextMessage> got = b.observer.receivedTexts();
    CHECK(got.size() == 1 && got[0].text == "CQ from the tortoise" && got[0].originCallsign == "W1AW");
    CHECK(got.size() == 1 && std::isnan(got[0].snr)); // Data2G reports none
    CHECK(hosts.with([&]() { return hosts.sides[0].burstModes; }) == std::vector<std::string>{"fsk16r25-r1/3"});
    CHECK(hosts.heardCommand(0, "BCAST MODE 1 fsk16r25-r1/3"));
    CHECK(!hosts.heardCommand(0, "CONNECT")); // a broadcast never opens a session
    CHECK(waitFor([&]() { return !a.transport.isTransmitting(); }));
    CHECK(a.transport.status().groupMode == "fsk16r25-r1/3");
}

void testDuetUsesAFastWideModeOrTheNarrowHostsBest()
{
    FakeHostPair hosts;
    hosts.with([&]() {
        hosts.sides[1].modes = narrowModes();
        return 0;
    });
    Station a("W1AW", hosts.sides[0], 5);
    Station b("VK3ABC", hosts.sides[1], 5);
    CHECK(settled(hosts, a, b));
    CHECK(waitFor([&]() { return b.transport.modeForGear(5).name == "n10-16qam-r1/2"; }));
    CHECK(a.transport.modeForGear(5).name == "w48-16qam-r1/2");

    std::string error;
    CHECK(a.protocol.sendMessage("quick one", "", error));
    runBoth(a, b, [&]() { return !b.observer.receivedTexts().empty(); });
    CHECK(b.observer.receivedTexts().size() == 1);
    CHECK(hosts.with([&]() { return hosts.sides[0].burstModes; }) == std::vector<std::string>{"w48-16qam-r1/2"});

    CHECK(b.protocol.sendMessage("and back", "", error));
    runBoth(a, b, [&]() { return !a.observer.receivedTexts().empty(); });
    CHECK(a.observer.receivedTexts().size() == 1);
    CHECK(hosts.with([&]() { return hosts.sides[1].burstModes; }) == std::vector<std::string>{"n10-16qam-r1/2"});
}

void testDirectedMessageGoesThroughASession()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(settled(hosts, a, b));
    CHECK(waitFor([&]() { return hosts.heardCommand(1, "LISTEN ON"); }));

    std::string error;
    CHECK(a.protocol.sendMessage("Hello through a session", "VK3ABC", error));
    runBoth(a, b, [&]() { return a.observer.sawStatus(MessageStatus::Acknowledged); });

    CHECK(a.observer.sawStatus(MessageStatus::Acknowledged));
    std::vector<TextMessage> got = b.observer.receivedTexts();
    CHECK(got.size() == 1 && got[0].text == "Hello through a session");
    CHECK(hosts.heardCommand(0, "CONNECT W1AW VK3ABC"));
    CHECK(hosts.with([&]() { return hosts.sides[0].sessionBursts; }) == 1);  // the message
    CHECK(hosts.with([&]() { return hosts.sides[1].sessionBursts; }) == 0);  // the modem acknowledged it
    CHECK(hosts.with([&]() { return hosts.sides[0].burstModes.size() + hosts.sides[1].burstModes.size(); }) == 0);
    CHECK(!a.observer.sawStatus(MessageStatus::AwaitingAck));
    CHECK(!hosts.heardCommand(0, "BCAST MODE")); // the session picks its own speed
    CHECK(a.transport.status().sessionPeer == "VK3ABC");
    CHECK(b.transport.status().sessionPeer == "W1AW");

    // Quiet for long enough, the caller closes it.
    runBoth(a, b, [&]() { return hosts.heardCommand(0, "DISCONNECT"); });
    CHECK(hosts.heardCommand(0, "DISCONNECT"));
    CHECK(waitFor([&]() { return a.transport.status().sessionPeer.empty(); }));
    CHECK(!hosts.heardCommand(1, "DISCONNECT")); // the called station leaves it to the caller
}

// Three messages written into the session together are each settled as the
// far end's modem acknowledges their bytes.
void testEachMessageIsSettledAsItsBytesAreAcknowledged()
{
    FakeHostPair hosts;
    hosts.with([&]() {
        hosts.sides[0].holdAcks = true;
        return 0;
    });
    Station a("W1AW", hosts.sides[0], 1);
    Station b("VK3ABC", hosts.sides[1], 1);
    CHECK(settled(hosts, a, b));
    CHECK(waitFor([&]() { return hosts.heardCommand(1, "LISTEN ON"); }));

    std::string error;
    int64_t ids[3];
    const char* texts[3] = {"one", "two", "three"};
    for (int i = 0; i < 3; i++)
    {
        CHECK(a.protocol.sendMessage(texts[i], "VK3ABC", error));
        ids[i] = a.observer.lastAddedId();
    }
    const size_t each = 2 + TEXT_FRAME_BYTES; // one frame each, as the session stream carries it
    runBoth(a, b, [&]() { return hosts.unackedBytes(0) == 3 * each; });
    CHECK(hosts.unackedBytes(0) == 3 * each);
    runBoth(a, b, [&]() { return false; }, 20);
    for (int64_t id : ids) CHECK(a.observer.statusOf(id) == MessageStatus::Transmitting);

    hosts.ackSome(0, each);
    runBoth(a, b, [&]() { return a.observer.statusOf(ids[0]) == MessageStatus::Acknowledged; });
    CHECK(a.observer.statusOf(ids[0]) == MessageStatus::Acknowledged);
    CHECK(a.observer.statusOf(ids[1]) == MessageStatus::Transmitting);
    CHECK(a.observer.statusOf(ids[2]) == MessageStatus::Transmitting);

    hosts.ackSome(0, 2 * each);
    runBoth(a, b, [&]() { return a.observer.statusOf(ids[2]) == MessageStatus::Acknowledged; });
    for (int64_t id : ids) CHECK(a.observer.statusOf(id) == MessageStatus::Acknowledged);
    CHECK(b.observer.receivedTexts().size() == 3);
    CHECK(hosts.with([&]() { return hosts.sides[1].sessionBursts; }) == 0);
}

// A host that only says whether anything is unacknowledged settles the
// messages written together all at once, when it says nothing is.
void testAnOlderHostSettlesTheBatchTogether()
{
    FakeHostPair hosts;
    hosts.with([&]() {
        hosts.sides[0].holdAcks = true;
        hosts.sides[0].exactBuffer = false;
        return 0;
    });
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(settled(hosts, a, b));
    CHECK(waitFor([&]() { return hosts.heardCommand(1, "LISTEN ON"); }));

    std::string error;
    CHECK(a.protocol.sendMessage("one", "VK3ABC", error));
    int64_t first = a.observer.lastAddedId();
    CHECK(a.protocol.sendMessage("two", "VK3ABC", error));
    int64_t second = a.observer.lastAddedId();
    const size_t each = 2 + TEXT_FRAME_BYTES;
    runBoth(a, b, [&]() { return hosts.unackedBytes(0) == 2 * each; });

    hosts.ackSome(0, each);
    runBoth(a, b, [&]() { return false; }, 50);
    CHECK(a.observer.statusOf(first) == MessageStatus::Transmitting);

    hosts.ackSome(0, each);
    runBoth(a, b, [&]() { return a.observer.statusOf(second) == MessageStatus::Acknowledged; });
    CHECK(a.observer.statusOf(first) == MessageStatus::Acknowledged);
    CHECK(a.observer.statusOf(second) == MessageStatus::Acknowledged);
}

// The session lost before the far end acknowledged the message fails it,
// even though the host, listening again, then reports an empty buffer.
void testALostSessionFailsTheMessage()
{
    FakeHostPair hosts;
    hosts.with([&]() {
        hosts.sides[0].holdAcks = true;
        return 0;
    });
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(settled(hosts, a, b));
    CHECK(waitFor([&]() { return hosts.heardCommand(1, "LISTEN ON"); }));

    std::string error;
    CHECK(a.protocol.sendMessage("into the static", "VK3ABC", error));
    int64_t id = a.observer.lastAddedId();
    runBoth(a, b, [&]() { return hosts.unackedBytes(0) != 0; });
    CHECK(hosts.unackedBytes(0) != 0);

    hosts.loseSession();
    runBoth(a, b, [&]() { return a.observer.statusOf(id) == MessageStatus::Failed; });
    CHECK(a.observer.statusOf(id) == MessageStatus::Failed);
    CHECK(!a.observer.sawStatus(MessageStatus::Acknowledged));
    CHECK(!a.observer.sawStatus(MessageStatus::Retrying));
    CHECK(b.observer.receivedTexts().empty());
}

// A ping through a session is answered by the modem: no pong of our own.
void testAPingThroughASessionIsAnsweredByTheModem()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(settled(hosts, a, b));
    CHECK(waitFor([&]() { return hosts.heardCommand(1, "LISTEN ON"); }));

    std::string error;
    CHECK(a.protocol.sendPing("VK3ABC", error));
    runBoth(a, b, [&]() { return a.observer.sawSystemLine("PING delivered"); });
    CHECK(a.observer.sawSystemLine("PING delivered"));
    CHECK(b.observer.sawSystemLine("PING!"));
    runBoth(a, b, [&]() { return false; }, 50);
    CHECK(hosts.with([&]() { return hosts.sides[1].sessionBursts + (int)hosts.sides[1].burstModes.size(); }) == 0);
}


void testAStationWithoutSessionsGetsTheGroup()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 2);
    Station b("VK3ABC", hosts.sides[1], 2, false); // does not listen for sessions
    CHECK(settled(hosts, a, b));

    std::string error;
    CHECK(a.protocol.sendMessage("Anyone home", "VK3ABC", error));
    runBoth(a, b, [&]() { return a.observer.sawStatus(MessageStatus::Acknowledged); });

    CHECK(a.observer.sawStatus(MessageStatus::Acknowledged));
    CHECK(b.observer.receivedTexts().size() == 1);
    CHECK(hosts.heardCommand(0, "CONNECT W1AW VK3ABC"));
    CHECK(!hosts.heardCommand(1, "LISTEN ON"));
    CHECK(hosts.with([&]() { return hosts.sides[0].burstModes; }) == std::vector<std::string>{"fsk8r50-r1/3"});

    // The next one goes straight to the group, without calling again.
    int connects = hosts.with([&]() {
        return (int)std::count_if(hosts.sides[0].commands.begin(), hosts.sides[0].commands.end(),
                                  [](const std::string& c) { return c.rfind("CONNECT", 0) == 0; });
    });
    CHECK(a.protocol.sendMessage("Second", "VK3ABC", error));
    runBoth(a, b, [&]() { return b.observer.receivedTexts().size() == 2; });
    CHECK(b.observer.receivedTexts().size() == 2);
    CHECK(hosts.with([&]() {
              return (int)std::count_if(hosts.sides[0].commands.begin(), hosts.sides[0].commands.end(),
                                        [](const std::string& c) { return c.rfind("CONNECT", 0) == 0; });
          }) == connects);
}

void testNotConnectedRefusesToTransmit()
{
    Data2GTransport transport;
    OutgoingBurst burst;
    burst.frame = textFrame("W1AW", "x");
    CHECK(!transport.transmit({burst}));

    int port = 0;
    int fd = listenOnLoopback(port);
    close(fd);
    Data2GTransport::Settings settings;
    settings.kissPort = port;
    settings.commandPort = port;
    transport.start(settings);
    CHECK(waitFor([&]() { return !transport.status().error.empty(); }));
    CHECK(!transport.status().kissConnected);
    CHECK(!transport.transmit({burst}));
    transport.stop();
}

void testLosingTheCommandPortClearsBusyAndReopens()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(settled(hosts, a, b));

    hosts.sayOnCommand(1, "BUSY ON");
    CHECK(waitFor([&]() { return b.transport.isChannelBusy(); }));
    hosts.dropCommandClient(1);
    CHECK(waitFor([&]() { return !b.transport.isChannelBusy(); }));
    CHECK(waitFor([&]() { return b.transport.status().groupPort != 0; }, 6000));
    CHECK(hosts.ready(1));
}

void testACallsignChangeReopensTheGroup()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    CHECK(waitFor([&]() { return hosts.ready(0); }));
    a.transport.setMyCallsign("W1AW-2");
    CHECK(waitFor([&]() { return hosts.heardCommand(0, "BCAST OPEN GLISS FROM W1AW-2"); }, 6000));
    CHECK(hosts.heardCommand(0, "MYCALL W1AW-2"));
}

} // namespace

int main()
{
    testKissPortsAndAckMode();
    testCommandLines();
    testTemposMapToModes();
    testSessionStream();
    testCommandCallsign();
    testNotConnectedRefusesToTransmit();
    testBroadcastGoesToTheGroupAtAdagio();
    testDuetUsesAFastWideModeOrTheNarrowHostsBest();
    testDirectedMessageGoesThroughASession();
    testEachMessageIsSettledAsItsBytesAreAcknowledged();
    testAnOlderHostSettlesTheBatchTogether();
    testALostSessionFailsTheMessage();
    testAPingThroughASessionIsAnsweredByTheModem();
    testAStationWithoutSessionsGetsTheGroup();
    testLosingTheCommandPortClearsBusyAndReopens();
    testACallsignChangeReopensTheGroup();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    printf("all Data2G checks passed\n");
    return 0;
}
