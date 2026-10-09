//=========================================================================
// Name:            Data2GTest.cpp
// Purpose:         Chat over an external data2g-host: the KISS, command,
//                  mode and session-stream formats, and two chat stations
//                  talking through a fake pair of hosts on localhost, over
//                  the GLISS broadcast group and over connected sessions,
//                  and files sent through those sessions; and files sent
//                  to everybody on the group, through a fake group of
//                  hosts losing what each test says.
//=========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../Data2GBroadcast.h"
#include "../Data2GFileTransfer.h"
#include "../Data2GLink.h"
#include "../Data2GTransport.h"
#include "../DeliveryChip.h"
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

// File records beside chat frames: every type, bodies of odd lengths,
// records split across reads, and the two kinds interleaved.
void testFileRecordsInTheStream()
{
    CHECK((Data2G::fileRecordEncode(Data2G::FileRecordType::Data, {9, 8}) ==
           std::vector<uint8_t>{'F', 4, 0, 2, 9, 8}));
    CHECK(Data2G::fileRecordEncode(Data2G::FileRecordType::Accept, {}).empty());
    CHECK(Data2G::fileRecordEncode(Data2G::FileRecordType::Data, std::vector<uint8_t>(0x10000, 1)).empty());

    std::vector<uint8_t> chatA = textFrame("W1AW", "before");
    std::vector<uint8_t> chatB = textFrame("W1AW", "between");
    std::vector<std::pair<Data2G::FileRecordType, std::vector<uint8_t>>> records = {
        {Data2G::FileRecordType::Offer, {1, 0, 0, 0x1B, 0x58, 'F', 'R', 'E', 'D', '.', 'T', 'X', 'T'}},
        {Data2G::FileRecordType::Accept, {1}},
        {Data2G::FileRecordType::Decline, {1}},
        {Data2G::FileRecordType::Data, std::vector<uint8_t>(1 + Data2G::FILE_PIECE_BYTES, 'G')},
        {Data2G::FileRecordType::Data, std::vector<uint8_t>(256, 0xC0)},   // a length with a low byte of 0
        {Data2G::FileRecordType::Data, std::vector<uint8_t>(255, 'F')},
        {Data2G::FileRecordType::Saved, {1}},
        {Data2G::FileRecordType::Cancel, {1, (uint8_t)Data2G::CancelReason::ReceiverStopped}},
        {(Data2G::FileRecordType)99, {5, 6}},   // a later version's, passed on to be ignored
    };

    std::vector<uint8_t> stream = Data2G::streamEncode(chatA);
    for (size_t i = 0; i < records.size(); i++)
    {
        std::vector<uint8_t> bytes = Data2G::fileRecordEncode(records[i].first, records[i].second);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        if (i == 3)
        {
            std::vector<uint8_t> chat = Data2G::streamEncode(chatB);
            stream.insert(stream.end(), chat.begin(), chat.end());
        }
    }

    auto expect = [&](const std::vector<std::vector<uint8_t>>& frames, const std::vector<Data2G::FileRecord>& got) {
        CHECK(frames.size() == 2 && frames[0] == chatA && frames[1] == chatB);
        CHECK(got.size() == records.size());
        for (size_t i = 0; i < got.size() && i < records.size(); i++)
        {
            CHECK(got[i].type == (uint8_t)records[i].first && got[i].body == records[i].second);
        }
    };

    // Whole, a byte at a time, and in uneven reads.
    for (size_t step : {stream.size(), (size_t)1, (size_t)3, (size_t)1000, (size_t)4096})
    {
        Data2G::StreamDecoder decoder;
        std::vector<std::vector<uint8_t>> frames;
        std::vector<Data2G::FileRecord> got;
        for (size_t at = 0; at < stream.size(); at += step)
        {
            size_t n = std::min(step, stream.size() - at);
            CHECK(decoder.feed(stream.data() + at, (int)n, frames, got));
        }
        expect(frames, got);
    }

    // The chat-only feed passes the frames and drops the records.
    Data2G::StreamDecoder chatOnly;
    std::vector<std::vector<uint8_t>> frames;
    CHECK(chatOnly.feed(stream.data(), (int)stream.size(), frames));
    CHECK(frames.size() == 2);

    // A record of type 0, or of no length, is not ours.
    for (std::vector<uint8_t> bad : {std::vector<uint8_t>{'F', 0, 0, 1, 7}, std::vector<uint8_t>{'F', 4, 0, 0},
                                     std::vector<uint8_t>{'X', 1}})
    {
        Data2G::StreamDecoder decoder;
        std::vector<Data2G::FileRecord> got;
        frames.clear();
        CHECK(!decoder.feed(bad.data(), (int)bad.size(), frames, got));
        CHECK(got.empty());
    }
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
        // The far end's modem acknowledges this many bytes in the same host
        // step as the next write is read, so one BUFFER answers both.
        size_t foldAck = 0;
        // DISCONNECT is answered but the session goes on until the test
        // ends it (loseSession), as while data2g-host closes it cleanly.
        bool holdDisconnect = false;
        // Session bytes written to this side are left unread (and so
        // unanswered) until the test lets the host read them, while the
        // far end's acknowledgements of what it read before go on.
        bool holdReads = false;
        std::vector<uint8_t> unread;
        // Group bursts are held while a session is up, as data2g-host does,
        // and go once it ends (or the test lets them, as Data2G's PR #59
        // does in a lull); with holdGroup, for good, as on a channel never
        // clear.
        bool holdGroupInSession = true;
        bool holdGroup = false;
        std::vector<std::vector<Data2G::KissFrame>> heldGroup;
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

    void foldNextAck(int side, size_t n)
    {
        with([&]() {
            sides[side].foldAck = n;
            return 0;
        });
    }

    size_t unackedBytes(int side)
    {
        return with([&]() { return sides[side].unacked.size(); });
    }

    void holdReads(int side)
    {
        with([&]() {
            sides[side].holdReads = true;
            return 0;
        });
    }

    size_t unreadBytes(int side)
    {
        return with([&]() { return sides[side].unread.size(); });
    }

    // The host reads, and answers, what it left unread.
    void releaseReads(int side)
    {
        with([&]() {
            Side& held = sides[side];
            held.holdReads = false;
            std::vector<uint8_t> bytes;
            bytes.swap(held.unread);
            if (!bytes.empty()) dataIn(side, bytes.data(), (int)bytes.size());
            return 0;
        });
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
            releaseHeld(0);
            releaseHeld(1);
            return 0;
        });
    }

    size_t heldGroupBursts(int side)
    {
        return with([&]() { return sides[side].heldGroup.size(); });
    }

    // The session idles and data2g-host lets the group bursts it held go
    // (Data2G's PR #59).
    void releaseGroup(int side)
    {
        with([&]() {
            releaseHeld(side);
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
            releaseHeld(0);
            releaseHeld(1);
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
            if (sessionWith >= 0 && !(side.holdDisconnect && w[0] == "DISCONNECT"))
            {
                say(side, "DISCONNECTED");
                say(other, "DISCONNECTED");
                sessionWith = -1;
                releaseHeld(0);
                releaseHeld(1);
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
        std::vector<Data2G::KissFrame> frames;
        side.kiss.feed(bytes, length, frames);
        if (frames.empty()) return;
        if (side.holdGroup || (side.holdGroupInSession && sessionWith >= 0))
        {
            side.heldGroup.push_back(frames);
            return;
        }
        transmitGroup(s, frames);
    }

    void releaseHeld(int s)
    {
        Side& side = sides[s];
        if (side.holdGroup) return;
        std::vector<std::vector<Data2G::KissFrame>> held;
        held.swap(side.heldGroup);
        for (const auto& frames : held) transmitGroup(s, frames);
    }

    void transmitGroup(int s, const std::vector<Data2G::KissFrame>& frames)
    {
        Side& side = sides[s];
        Side& other = sides[1 - s];

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
        if (side.holdReads)
        {
            side.unread.insert(side.unread.end(), bytes, bytes + length);
            return;
        }
        side.unacked.insert(side.unacked.end(), bytes, bytes + length);
        if (side.foldAck > 0)
        {
            deliverPending(s, side.foldAck, false);
            side.foldAck = 0;
        }
        sayBuffer(side); // data2g-host answers every write with one
        if (!side.holdAcks) deliverPending(s, side.unacked.size());
    }

    // A burst carrying the first n waiting bytes, and the far end's
    // acknowledgement of them.
    void deliverPending(int s, size_t n, bool answer = true)
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
        if (answer) sayBuffer(side);
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
                    // Pieces of a file reach the far end as the test lets
                    // them through, not held back to coalesce.
                    int one = 1;
                    setsockopt(side.client[which], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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

    // The message as it was last updated.
    TextMessage lastUpdate(int64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex);
        TextMessage last;
        for (const TextMessage& m : updated)
        {
            if (m.id == id) last = m;
        }
        return last;
    }

    bool sawStatusOf(int64_t id, MessageStatus status)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const TextMessage& m : updated)
        {
            if (m.id == id && m.status == status) return true;
        }
        return false;
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
    // Any fake host with a KISS and a command port.
    template <typename Host>
    Station(const std::string& callsign, const Host& host, int gear, bool useSessions = true)
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

        settings.kissPort = host.kissPort;
        settings.commandPort = host.commandPort;
        settings.useSessions = useSessions;
        transport.start(settings);
    }

    ~Station() { transport.stop(); }

    Data2GTransport::Settings settings;

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
    // The far end's modem acknowledges before VK3ABC's chat has read it.
    runBoth(a, b, [&]() {
        return a.observer.sawStatus(MessageStatus::Acknowledged) && !b.observer.receivedTexts().empty();
    });

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
    CHECK(!b.transport.isTransmitting()); // no chat acknowledgement of its own waiting

    // Quiet for long enough, the caller closes it.
    runBoth(a, b, [&]() { return hosts.heardCommand(0, "DISCONNECT"); });
    CHECK(hosts.heardCommand(0, "DISCONNECT"));
    CHECK(waitFor([&]() { return a.transport.status().sessionPeer.empty(); }));
    CHECK(!hosts.heardCommand(1, "DISCONNECT")); // the called station leaves it to the caller

    // Nor does one go on the group once the session has gone.
    runBoth(a, b, [&]() { return false; }, 100);
    CHECK(hosts.with([&]() { return hosts.sides[1].burstModes.size(); }) == 0);
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

// Deselecting the station ends the session at once and drops what is
// outstanding for it; the group is free again straight after.
void testDeselectingTheStationEndsTheSession()
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
    CHECK(a.protocol.sendMessage("never mind", "VK3ABC", error));
    int64_t first = a.observer.lastAddedId();
    CHECK(a.protocol.sendMessage("this neither", "VK3ABC", error));
    int64_t second = a.observer.lastAddedId();
    runBoth(a, b, [&]() { return hosts.unackedBytes(0) != 0; });
    CHECK(hosts.unackedBytes(0) != 0);

    // Something of ours unacknowledged: ABORT, as DISCONNECT would wait.
    CHECK(a.protocol.releaseStation("VK3ABC") == 2);
    CHECK(a.observer.statusOf(first) == MessageStatus::Aborted);
    CHECK(a.observer.statusOf(second) == MessageStatus::Aborted);
    runBoth(a, b, [&]() { return hosts.heardCommand(0, "ABORT"); });
    CHECK(hosts.heardCommand(0, "ABORT"));

    CHECK(a.protocol.sendMessage("back on the group", "", error));
    runBoth(a, b, [&]() { return !b.observer.receivedTexts().empty(); });
    std::vector<TextMessage> got = b.observer.receivedTexts();
    CHECK(got.size() == 1 && got[0].text == "back on the group");
    CHECK(!a.observer.sawStatus(MessageStatus::Failed));
    CHECK(!a.observer.sawStatus(MessageStatus::Acknowledged));
}

// With nothing unacknowledged the session is closed with DISCONNECT, which
// tells the far end, rather than left to time out.
void testDeselectingAQuietSessionDisconnects()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(settled(hosts, a, b));
    CHECK(waitFor([&]() { return hosts.heardCommand(1, "LISTEN ON"); }));

    std::string error;
    CHECK(a.protocol.sendMessage("all done", "VK3ABC", error));
    runBoth(a, b, [&]() { return a.observer.sawStatus(MessageStatus::Acknowledged); });
    CHECK(a.observer.sawStatus(MessageStatus::Acknowledged));
    CHECK(!hosts.heardCommand(0, "DISCONNECT")); // it would stay open 45 s

    CHECK(a.protocol.releaseStation("VK3ABC") == 0);
    runBoth(a, b, [&]() { return hosts.heardCommand(0, "DISCONNECT"); });
    CHECK(hosts.heardCommand(0, "DISCONNECT"));
    CHECK(!hosts.heardCommand(0, "ABORT"));
    CHECK(waitFor([&]() { return a.transport.status().sessionPeer.empty(); }));
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
    runBoth(a, b, [&]() { return a.observer.sawSystemLine("PING delivered") && b.observer.sawSystemLine("PING!"); });
    CHECK(a.observer.sawSystemLine("PING delivered"));
    CHECK(b.observer.sawSystemLine("PING!"));
    runBoth(a, b, [&]() { return false; }, 50);
    CHECK(hosts.with([&]() { return hosts.sides[1].sessionBursts + (int)hosts.sides[1].burstModes.size(); }) == 0);
    CHECK(!b.transport.isTransmitting()); // no pong waiting for the group
}

// Restarting the transport (new Data2G settings) ends what its session held:
// the message is not left showing SENDING for good.
void testRestartingTheTransportEndsWhatItHeld()
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
    CHECK(a.protocol.sendMessage("caught by a restart", "VK3ABC", error));
    int64_t id = a.observer.lastAddedId();
    runBoth(a, b, [&]() { return hosts.unackedBytes(0) != 0; });
    CHECK(hosts.unackedBytes(0) != 0);

    a.transport.start(a.settings);
    a.protocol.setTransport(&a.transport);
    runBoth(a, b, [&]() { return a.observer.statusOf(id) == MessageStatus::Failed; });
    CHECK(a.observer.statusOf(id) == MessageStatus::Failed);
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

//-------------------------------------------------------------------------
// A broadcast while a session holds the group
//-------------------------------------------------------------------------

// A session up, a broadcast of ours is written to data2g-host anyway, which
// holds it; the chip says who holds it, and through a session longer than
// the not-sent timeout it is not given up. When the session idles the host
// lets it go (Data2G's PR #59), and it ends SENT.
void testABroadcastHeldByASessionSaysSo()
{
    FakeHostPair hosts;
    hosts.with([&]() {
        hosts.sides[1].holdAcks = true;         // the session has something of VK3ABC's in it
        hosts.sides[0].holdDisconnect = true;   // and stays up however W1AW tries to close it
        return 0;
    });
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(settled(hosts, a, b));
    CHECK(waitFor([&]() { return hosts.heardCommand(0, "LISTEN ON"); }));

    std::string error;
    CHECK(b.protocol.sendMessage("are you there", "W1AW", error));
    runBoth(a, b, [&]() { return a.transport.status().sessionPeer == "VK3ABC"; });
    CHECK(a.transport.status().sessionPeer == "VK3ABC");

    CHECK(a.protocol.sendMessage("CQ the group", "", error));
    int64_t id = a.observer.lastAddedId();
    runBoth(a, b, [&]() { return a.observer.lastUpdate(id).heldBy == "VK3ABC"; });
    CHECK(hosts.heldGroupBursts(0) == 1);
    TextMessage held = a.observer.lastUpdate(id);
    CHECK(held.status == MessageStatus::Transmitting && held.heldBy == "VK3ABC");
    CHECK(deliveryChipState(held).kind == DeliveryChipKind::Held);
    CHECK(deliveryChipState(held).heldBy == "VK3ABC");

    // Held well past the not-sent timeout: it waits for the session.
    uint64_t until = a.nowMs.load() + Data2GTransport::NOT_SENT_TIMEOUT_MS + 20000;
    runBoth(a, b, [&]() { return a.nowMs.load() >= until; }, 4000);
    CHECK(a.nowMs.load() >= until);
    CHECK(a.observer.statusOf(id) == MessageStatus::Transmitting);
    CHECK(a.observer.lastUpdate(id).heldBy == "VK3ABC");
    CHECK(!a.observer.sawStatusOf(id, MessageStatus::NotSent));
    CHECK(!a.observer.sawStatusOf(id, MessageStatus::Sent));

    // A lull in the session: data2g-host lets it go.
    hosts.releaseGroup(0);
    runBoth(a, b, [&]() { return a.observer.statusOf(id) == MessageStatus::Sent; });
    CHECK(a.observer.statusOf(id) == MessageStatus::Sent);
    CHECK(a.observer.lastUpdate(id).heldBy.empty());
    CHECK(!a.observer.sawStatusOf(id, MessageStatus::NotSent));
    std::vector<TextMessage> got = b.observer.receivedTexts();
    CHECK(std::any_of(got.begin(), got.end(), [](const TextMessage& m) { return m.text == "CQ the group"; }));
}

// With a host that holds the group for the whole session, the broadcast
// goes once the session ends, and the chip goes from HELD back to SENDING.
void testABroadcastGoesWhenTheSessionEnds()
{
    FakeHostPair hosts;
    hosts.with([&]() {
        hosts.sides[1].holdAcks = true;
        return 0;
    });
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(settled(hosts, a, b));
    CHECK(waitFor([&]() { return hosts.heardCommand(0, "LISTEN ON"); }));

    std::string error;
    CHECK(b.protocol.sendMessage("a long one", "W1AW", error));
    runBoth(a, b, [&]() { return a.transport.status().sessionPeer == "VK3ABC"; });

    CHECK(a.protocol.sendMessage("waiting my turn", "", error));
    int64_t id = a.observer.lastAddedId();
    runBoth(a, b, [&]() { return a.observer.lastUpdate(id).heldBy == "VK3ABC"; });
    CHECK(a.observer.lastUpdate(id).heldBy == "VK3ABC");

    hosts.loseSession();
    runBoth(a, b, [&]() { return a.observer.statusOf(id) == MessageStatus::Sent; });
    CHECK(a.observer.statusOf(id) == MessageStatus::Sent);
    CHECK(!a.observer.sawStatusOf(id, MessageStatus::NotSent));
    std::vector<TextMessage> got = b.observer.receivedTexts();
    CHECK(std::any_of(got.begin(), got.end(), [](const TextMessage& m) { return m.text == "waiting my turn"; }));
}

// A keying data2g-host never sends, with no session to wait for, is given
// up after the not-sent timeout and ends NOT SENT: never SENT.
void testABroadcastNeverSentEndsNotSent()
{
    FakeHostPair hosts;
    hosts.with([&]() {
        hosts.sides[0].holdGroup = true;
        return 0;
    });
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(settled(hosts, a, b));

    std::string error;
    CHECK(a.protocol.sendMessage("into the void", "", error));
    int64_t id = a.observer.lastAddedId();
    runBoth(a, b, [&]() { return hosts.heldGroupBursts(0) == 1; });
    CHECK(hosts.heldGroupBursts(0) == 1);
    CHECK(a.observer.lastUpdate(id).heldBy.empty()); // nobody's session holds it

    runBoth(a, b, [&]() { return a.observer.statusOf(id) == MessageStatus::NotSent; }, 4000);
    CHECK(a.observer.statusOf(id) == MessageStatus::NotSent);
    CHECK(!a.observer.sawStatusOf(id, MessageStatus::Sent));
    CHECK(!a.transport.isTransmitting());
    CHECK(a.nowMs.load() >= 1000 + Data2GTransport::NOT_SENT_TIMEOUT_MS);
}

//-------------------------------------------------------------------------
// Files through a session
//-------------------------------------------------------------------------

using FileState = Data2G::FileTransfer::State;
namespace fs = std::filesystem;

// A folder of its own for each test, removed afterwards.
struct FileScratch
{
    explicit FileScratch(const std::string& name)
    {
        std::random_device random;
        dir = fs::temp_directory_path() / ("glissando-d2g-test-" + name + "-" + std::to_string(random()));
        fs::create_directories(dir);
    }
    ~FileScratch()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string path(const std::string& name) const { return (dir / name).string(); }

    std::string write(const std::string& name, size_t size)
    {
        std::string content(size, '\0');
        for (size_t i = 0; i < size; i++) content[i] = (char)((i * 13 + i / 509) & 0xFF);
        std::ofstream out(dir / name, std::ios::binary);
        out.write(content.data(), (std::streamsize)content.size());
        return path(name);
    }

    fs::path dir;
};

std::string fileContents(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool fileExists(const std::string& path)
{
    std::error_code ec;
    return fs::exists(path, ec);
}

// The newest transfer one way, or a blank one in Waiting with id 0.
Data2G::FileTransfer newestFile(Data2GTransport& transport, bool outgoing)
{
    Data2G::FileTransfer found;
    for (const Data2G::FileTransfer& t : transport.fileTransfers())
    {
        if (t.outgoing == outgoing) found = t;
    }
    return found;
}

FileState fileState(Data2GTransport& transport, uint64_t id)
{
    for (const Data2G::FileTransfer& t : transport.fileTransfers())
    {
        if (t.id == id) return t.state;
    }
    return FileState::Waiting;
}

bool sessionsReady(FakeHostPair& hosts, Station& a, Station& b)
{
    return settled(hosts, a, b) && waitFor([&]() { return hosts.heardCommand(1, "LISTEN ON"); }) &&
           waitFor([&]() { return a.transport.sendsFiles() && b.transport.sendsFiles(); });
}

// Side 0 holds its session bytes; each step the far end's modem takes in
// and acknowledges up to perStep of them.
void runAcking(FakeHostPair& hosts, Station& a, Station& b, size_t perStep, const std::function<bool()>& done,
               int steps = 6000)
{
    for (int i = 0; i < steps && !done(); i++)
    {
        hosts.ackSome(0, perStep);
        a.step();
        b.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void holdSide0(FakeHostPair& hosts)
{
    hosts.with([&]() {
        hosts.sides[0].holdAcks = true;
        return 0;
    });
}

void testAFileGoesThroughASession()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("end-to-end");
    std::string source = scratch.write("FRED.TXT", 7000);

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", source, error);
    CHECK(id != 0);
    runBoth(a, b, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
    Data2G::FileTransfer offer = newestFile(b.transport, false);
    CHECK(offer.state == FileState::Asking && offer.name == "FRED.TXT" && offer.size == 7000 && offer.peer == "W1AW");
    CHECK(hosts.heardCommand(0, "CONNECT W1AW VK3ABC"));
    CHECK(fileState(a.transport, id) == FileState::Offered);

    // An offer waiting for its answer keeps the session open past the
    // usual 45 s of quiet.
    runBoth(a, b, [&]() { return false; }, 600);
    CHECK(!hosts.heardCommand(0, "DISCONNECT") && !hosts.heardCommand(1, "DISCONNECT"));
    CHECK(a.transport.status().sessionPeer == "VK3ABC");

    std::string saveAs = scratch.path("saved.txt");
    CHECK(b.transport.acceptFile(offer.id, saveAs, error));
    runBoth(a, b, [&]() { return fileState(a.transport, id) == FileState::Delivered; });
    CHECK(fileState(a.transport, id) == FileState::Delivered);
    Data2G::FileTransfer got = newestFile(b.transport, false);
    CHECK(got.state == FileState::Saved && got.done == 7000 && got.path == saveAs);
    CHECK(fileContents(saveAs) == fileContents(source));
    CHECK(!fileExists(saveAs + ".part"));
    CHECK(newestFile(a.transport, true).done == 7000);
    CHECK(hosts.with([&]() { return hosts.sides[0].burstModes.size() + hosts.sides[1].burstModes.size(); }) == 0);

    // Done, and quiet: the caller closes the session as usual.
    runBoth(a, b, [&]() { return hosts.heardCommand(0, "DISCONNECT"); });
    CHECK(hosts.heardCommand(0, "DISCONNECT"));
}

void testAFileDeclined()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("decline");

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("x.bin", 500), error);
    runBoth(a, b, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
    CHECK(b.transport.declineFile(newestFile(b.transport, false).id));
    runBoth(a, b, [&]() { return fileState(a.transport, id) == FileState::Declined; });
    CHECK(fileState(a.transport, id) == FileState::Declined);
    CHECK(newestFile(b.transport, false).state == FileState::Declined);
}

// The sender cancels part way: no more pieces go, and the receiver, told,
// deletes what it has and drops anything still coming.
void testTheSenderCancelsAFile()
{
    FakeHostPair hosts;
    holdSide0(hosts);
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("sender-cancels");

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("big.bin", 30000), error);
    runAcking(hosts, a, b, 2000, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
    std::string saveAs = scratch.path("big.got");
    CHECK(b.transport.acceptFile(newestFile(b.transport, false).id, saveAs, error));
    runAcking(hosts, a, b, 1000, [&]() { return newestFile(b.transport, false).done >= 4096; });
    CHECK(newestFile(b.transport, false).state == FileState::Receiving);
    CHECK(fileExists(saveAs + ".part"));

    CHECK(a.transport.cancelFile(id));
    CHECK(fileState(a.transport, id) == FileState::Cancelled);
    runAcking(hosts, a, b, 1000, [&]() { return newestFile(b.transport, false).state == FileState::CancelledThere; });
    CHECK(newestFile(b.transport, false).state == FileState::CancelledThere);
    CHECK(newestFile(b.transport, false).done < 30000);
    CHECK(!fileExists(saveAs + ".part") && !fileExists(saveAs));
    runAcking(hosts, a, b, 100000, [&]() { return false; }, 50);
    CHECK(hosts.unackedBytes(0) == 0);
    CHECK(!fileExists(saveAs + ".part"));
}

void testTheReceiverCancelsAFile()
{
    FakeHostPair hosts;
    holdSide0(hosts);
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("receiver-cancels");

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("big.bin", 30000), error);
    runAcking(hosts, a, b, 2000, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
    std::string saveAs = scratch.path("big.got");
    CHECK(b.transport.acceptFile(newestFile(b.transport, false).id, saveAs, error));
    runAcking(hosts, a, b, 1000, [&]() { return newestFile(b.transport, false).done >= 4096; });

    CHECK(b.transport.cancelFile(newestFile(b.transport, false).id));
    CHECK(!fileExists(saveAs + ".part"));
    runAcking(hosts, a, b, 1000, [&]() { return fileState(a.transport, id) == FileState::CancelledThere; });
    CHECK(fileState(a.transport, id) == FileState::CancelledThere);
    CHECK(newestFile(b.transport, false).state == FileState::Cancelled);
}

// The link fails part way: both sides fail it, and the part file goes.
void testALostSessionFailsTheFile()
{
    FakeHostPair hosts;
    holdSide0(hosts);
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("lost");

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("big.bin", 30000), error);
    runAcking(hosts, a, b, 2000, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
    std::string saveAs = scratch.path("big.got");
    CHECK(b.transport.acceptFile(newestFile(b.transport, false).id, saveAs, error));
    runAcking(hosts, a, b, 1000, [&]() { return newestFile(b.transport, false).done >= 4096; });
    CHECK(fileExists(saveAs + ".part"));

    hosts.loseSession();
    runBoth(a, b, [&]() {
        return fileState(a.transport, id) == FileState::Failed &&
               newestFile(b.transport, false).state == FileState::Failed;
    });
    CHECK(fileState(a.transport, id) == FileState::Failed);
    CHECK(newestFile(b.transport, false).state == FileState::Failed);
    CHECK(!fileExists(saveAs + ".part"));
}

// Deselecting the station mid-file ends the session at once: cancelled
// here, failed there.
void testDeselectingTheStationCancelsTheFile()
{
    FakeHostPair hosts;
    holdSide0(hosts);
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("deselect");

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("big.bin", 30000), error);
    runAcking(hosts, a, b, 2000, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
    CHECK(b.transport.acceptFile(newestFile(b.transport, false).id, scratch.path("big.got"), error));
    runAcking(hosts, a, b, 1000, [&]() { return newestFile(b.transport, false).done >= 4096; });

    a.protocol.releaseStation("VK3ABC");
    CHECK(fileState(a.transport, id) == FileState::Cancelled);
    runBoth(a, b, [&]() { return newestFile(b.transport, false).state == FileState::Failed; });
    CHECK(newestFile(b.transport, false).state == FileState::Failed);
    CHECK(hosts.heardCommand(0, "ABORT"));
}

// The receiver lets go of the station while the offer is up: the sender
// hears it cancelled (not that the receiver can't take files), and the
// session then ends cleanly.
void testDeselectingWhileAnOfferIsUp()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("deselect-offer");

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("FRED.TXT", 500), error);
    runBoth(a, b, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
    CHECK(fileState(a.transport, id) == FileState::Offered);
    b.protocol.releaseStation("W1AW");
    CHECK(newestFile(b.transport, false).state == FileState::Cancelled);
    runBoth(a, b, [&]() { return fileState(a.transport, id) != FileState::Offered; });
    CHECK(fileState(a.transport, id) == FileState::CancelledThere);
    runBoth(a, b, [&]() { return hosts.heardCommand(1, "DISCONNECT"); });
    CHECK(hosts.heardCommand(1, "DISCONNECT"));
    CHECK(!hosts.heardCommand(1, "ABORT"));
}

// Sending stops being allowed mid-file (out of the data segments): no more
// pieces, the far end told, and a file waiting for another station fails
// rather than call it.
void testInhibitedStopsSessionFiles()
{
    FakeHostPair hosts;
    holdSide0(hosts);
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("inhibit");
    b.transport.setFileAutoAccept(scratch.dir.string(), {"W1AW"});

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("big.bin", 60000), error);
    uint64_t waiting = a.transport.sendFile("K7ABC", scratch.write("next.bin", 100), error);
    CHECK(id != 0 && waiting != 0);
    runAcking(hosts, a, b, 1500, [&]() { return newestFile(b.transport, false).done >= 4096; });
    CHECK(fileState(a.transport, id) == FileState::Sending);

    a.transport.setFilesInhibited(true);
    CHECK(fileState(a.transport, id) == FileState::Failed);
    CHECK(fileState(a.transport, waiting) == FileState::Failed);
    CHECK(a.transport.sendFile("K7ABC", scratch.write("more.bin", 100), error) == 0);
    runAcking(hosts, a, b, 100000,
              [&]() { return newestFile(b.transport, false).state == FileState::FailedThere; });
    CHECK(newestFile(b.transport, false).state == FileState::FailedThere);
    CHECK(newestFile(b.transport, false).done < 60000);
    for (int i = 0; i < 200; i++)
    {
        hosts.ackSome(0, 100000);
        a.step();
        b.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!hosts.heardCommand(0, "CONNECT W1AW K7ABC"));
}

void testAnUnansweredOfferExpires()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("expiry");

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("x.bin", 500), error);
    runBoth(a, b, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
    a.nowMs += Data2G::FileTransferEngine::OFFER_EXPIRY_MS;
    b.nowMs += Data2G::FileTransferEngine::OFFER_EXPIRY_MS;
    runBoth(a, b, [&]() {
        return fileState(a.transport, id) == FileState::Expired &&
               newestFile(b.transport, false).state == FileState::Expired;
    });
    CHECK(fileState(a.transport, id) == FileState::Expired);
    CHECK(newestFile(b.transport, false).state == FileState::Expired);
    CHECK(!b.transport.acceptFile(newestFile(b.transport, false).id, scratch.path("late.bin"), error));
}

// A name that would be trouble on the receiver's disk, taken without
// asking from a station on the auto-accept list, is saved under a safe
// one in the received-files folder.
void testAnOddNameIsSavedSafely()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("odd-name");
    fs::create_directories(scratch.dir / "received");
    std::string folder = (scratch.dir / "received").string();
    b.transport.setFileAutoAccept(folder, {"w1aw"});

    std::string error;
    std::string source = scratch.write(" .evil\x01:name?.txt", 1234);
    uint64_t id = a.transport.sendFile("VK3ABC", source, error);
    runBoth(a, b, [&]() { return fileState(a.transport, id) == FileState::Delivered; });
    CHECK(fileState(a.transport, id) == FileState::Delivered);
    Data2G::FileTransfer got = newestFile(b.transport, false);
    CHECK(got.autoAccepted && got.state == FileState::Saved);
    CHECK(got.name == "evilname.txt");
    CHECK(got.path == (fs::path(folder) / "evilname.txt").string());
    CHECK(fileContents(got.path) == fileContents(source));
}

// A message typed while a large file is going goes in after the piece
// under way, not after the whole file.
void testChatOvertakesALargeFile()
{
    FakeHostPair hosts;
    holdSide0(hosts);
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("overtake");
    b.transport.setFileAutoAccept(scratch.dir.string(), {"W1AW"});

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("large.bin", 40000), error);
    runAcking(hosts, a, b, 1500, [&]() { return newestFile(b.transport, false).done >= 4096; });
    CHECK(newestFile(b.transport, false).state == FileState::Receiving);

    // No more of the file is waiting at the host than a piece and a bit.
    CHECK(hosts.unackedBytes(0) <= 2 * (Data2G::FILE_PIECE_BYTES + 5));

    CHECK(a.protocol.sendMessage("Overtaking", "VK3ABC", error));
    int64_t message = a.observer.lastAddedId();
    uint64_t fileDoneWhenHeard = 0;
    runAcking(hosts, a, b, 1500, [&]() {
        if (fileDoneWhenHeard == 0 && !b.observer.receivedTexts().empty())
        {
            fileDoneWhenHeard = std::max<uint64_t>(1, newestFile(b.transport, false).done);
        }
        return fileState(a.transport, id) == FileState::Delivered;
    });
    CHECK(fileState(a.transport, id) == FileState::Delivered);
    CHECK(a.observer.statusOf(message) == MessageStatus::Acknowledged);
    std::vector<TextMessage> got = b.observer.receivedTexts();
    CHECK(got.size() == 1 && got[0].text == "Overtaking");
    CHECK(fileDoneWhenHeard > 0 && fileDoneWhenHeard <= 2 * (uint64_t)Data2G::FILE_PIECE_BYTES);
    CHECK(newestFile(b.transport, false).state == FileState::Saved);
}

// A message written while the tail of a piece is unacknowledged, read by
// the host in the same step as an acknowledgement bigger than it: the one
// BUFFER that answers both falls rather than rises. The message and the
// file still go, rather than waiting on a read that is never seen.
void testAWriteReadWithAnAckStillMovesOn()
{
    FakeHostPair hosts;
    holdSide0(hosts);
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("folded");
    b.transport.setFileAutoAccept(scratch.dir.string(), {"W1AW"});

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("large.bin", 40000), error);
    runAcking(hosts, a, b, 1500, [&]() { return newestFile(b.transport, false).done >= 4096; });
    runBoth(a, b, [&]() { return false; }, 20);
    size_t waiting = hosts.unackedBytes(0);
    CHECK(waiting > Data2GTransport::FILE_PIECE_LOW_WATER);
    CHECK(a.transport.status().sessionUnacked == (int64_t)waiting);

    hosts.foldNextAck(0, 300);
    CHECK(a.protocol.sendMessage("Folded in", "VK3ABC", error));
    int64_t message = a.observer.lastAddedId();
    const int64_t folded = (int64_t)(waiting - 300 + 2 + TEXT_FRAME_BYTES);
    runBoth(a, b, [&]() { return a.transport.status().sessionUnacked == folded; }, 500);
    CHECK(hosts.with([&]() { return hosts.sides[0].foldAck; }) == 0);
    CHECK(a.transport.status().sessionUnacked == folded);

    runAcking(hosts, a, b, 1500, [&]() {
        return fileState(a.transport, id) == FileState::Delivered &&
               a.observer.statusOf(message) == MessageStatus::Acknowledged;
    });
    CHECK(fileState(a.transport, id) == FileState::Delivered);
    CHECK(a.observer.statusOf(message) == MessageStatus::Acknowledged);
    CHECK(newestFile(b.transport, false).state == FileState::Saved);
}

// An offer that crosses the far end's DISCONNECT, closing a quiet session
// it opened, is still seen there, and fails with the session.
void testAnOfferCrossingADisconnectIsSeen()
{
    FakeHostPair hosts;
    hosts.with([&]() {
        hosts.sides[1].holdDisconnect = true;
        return 0;
    });
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    CHECK(waitFor([&]() { return hosts.heardCommand(0, "LISTEN ON"); }));

    std::string error;
    CHECK(b.protocol.sendMessage("Quick hello", "W1AW", error));
    runBoth(a, b, [&]() { return hosts.heardCommand(1, "DISCONNECT"); });
    CHECK(hosts.heardCommand(1, "DISCONNECT"));

    FileScratch scratch("crossing");
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("x.bin", 500), error);
    CHECK(id != 0);
    runBoth(a, b, [&]() { return newestFile(b.transport, false).state == FileState::Asking; }, 300);
    CHECK(newestFile(b.transport, false).state == FileState::Asking);

    hosts.loseSession();
    runBoth(a, b, [&]() { return newestFile(b.transport, false).state == FileState::Failed; });
    CHECK(newestFile(b.transport, false).state == FileState::Failed);
}

// A file sent to a station whose Glissando is older, without file
// transfer: its stream decoder takes the Offer for something that is not
// chat and closes the session, and the file fails as not supported. The
// far end here is a raw client of the second host, doing just that.
int connectLoopback(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

void testAnOlderGlissandoCantTakeTheFile()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    int command = connectLoopback(hosts.sides[1].commandPort);
    int data = connectLoopback(hosts.sides[1].dataPort);
    CHECK(command >= 0 && data >= 0);
    std::string hello = "MYCALL VK3ABC\rCHAT ON\rLISTEN ON\r";
    send(command, hello.data(), hello.size(), MSG_NOSIGNAL);
    CHECK(waitFor([&]() { return hosts.ready(0) && hosts.heardCommand(1, "LISTEN ON"); }));
    CHECK(waitFor([&]() { return a.transport.sendsFiles(); }));

    FileScratch scratch("older");
    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("x.bin", 500), error);
    CHECK(id != 0);
    bool closed = false;
    for (int i = 0; i < 3000 && fileState(a.transport, id) != FileState::NotSupported; i++)
    {
        a.step();
        uint8_t byte = 0;
        // Its decoder knows only chat records, 'G'.
        if (!closed && recv(data, &byte, 1, MSG_DONTWAIT) == 1 && byte != 'G')
        {
            std::string bye = "DISCONNECT\r";
            send(command, bye.data(), bye.size(), MSG_NOSIGNAL);
            closed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(closed);
    CHECK(fileState(a.transport, id) == FileState::NotSupported);
    close(command);
    close(data);
}

// data2g-host's command port lost while the offer waits for its answer:
// the session ends on this side, which says nothing of the far end's
// Glissando, so the file fails rather than being called not supported.
void testLosingTheCommandPortFailsAnOffer()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("lost-port");

    std::string error;
    uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("x.bin", 500), error);
    runBoth(a, b, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
    CHECK(fileState(a.transport, id) == FileState::Offered);
    hosts.dropCommandClient(0);
    runBoth(a, b, [&]() { return fileState(a.transport, id) != FileState::Offered; });
    CHECK(fileState(a.transport, id) == FileState::Failed);
}

// Two files for one station: the second is offered only once the first
// has been delivered, in the same session.
void testTwoFilesForOneStationGoInTurn()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("in-turn");
    fs::create_directories(scratch.dir / "in");
    b.transport.setFileAutoAccept((scratch.dir / "in").string(), {"W1AW"});

    std::string error;
    std::string one = scratch.write("one.bin", 9000);
    std::string two = scratch.write("two.bin", 3000);
    uint64_t first = a.transport.sendFile("VK3ABC", one, error);
    uint64_t second = a.transport.sendFile("VK3ABC", two, error);
    CHECK(first != 0 && second != 0);
    bool early = false;
    runBoth(a, b, [&]() {
        if (fileState(a.transport, second) != FileState::Waiting &&
            fileState(a.transport, first) != FileState::Delivered)
        {
            early = true;
        }
        return fileState(a.transport, second) == FileState::Delivered;
    });
    CHECK(!early);
    CHECK(fileState(a.transport, first) == FileState::Delivered);
    CHECK(fileState(a.transport, second) == FileState::Delivered);
    CHECK(fileContents((scratch.dir / "in" / "one.bin").string()) == fileContents(one));
    CHECK(fileContents((scratch.dir / "in" / "two.bin").string()) == fileContents(two));
    int connects = hosts.with([&]() {
        return (int)std::count_if(hosts.sides[0].commands.begin(), hosts.sides[0].commands.end(),
                                  [](const std::string& c) { return c.rfind("CONNECT", 0) == 0; });
    });
    CHECK(connects == 1);
}

// A host that only says 1 for "some": each piece goes once everything
// before it is acknowledged, and the file still arrives whole.
void testAFileThroughAnOlderHost()
{
    FakeHostPair hosts;
    hosts.with([&]() {
        hosts.sides[0].holdAcks = true;
        hosts.sides[0].exactBuffer = false;
        return 0;
    });
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("older-host");
    fs::create_directories(scratch.dir / "in");
    b.transport.setFileAutoAccept((scratch.dir / "in").string(), {"W1AW"});

    std::string error;
    std::string source = scratch.write("big.bin", 20000);
    uint64_t id = a.transport.sendFile("VK3ABC", source, error);
    size_t most = 0;
    runAcking(hosts, a, b, 1500, [&]() {
        most = std::max(most, hosts.unackedBytes(0));
        return fileState(a.transport, id) == FileState::Delivered;
    });
    CHECK(fileState(a.transport, id) == FileState::Delivered);
    CHECK(!a.transport.status().sessionUnackedExact);
    CHECK(most > 0 && most <= (size_t)Data2G::FILE_PIECE_BYTES + 16); // one piece at a time
    Data2G::FileTransfer got = newestFile(b.transport, false);
    CHECK(got.state == FileState::Saved && fileContents(got.path) == fileContents(source));
}

// Each side's clock runs on its own: the side whose clock runs out first
// expires the offer, and its Cancel expires it on the other.
void testAnOfferExpiresOnOneClock()
{
    for (int expiring = 0; expiring < 2; expiring++)
    {
        FakeHostPair hosts;
        Station a("W1AW", hosts.sides[0], 3);
        Station b("VK3ABC", hosts.sides[1], 3);
        CHECK(sessionsReady(hosts, a, b));
        FileScratch scratch("one-clock");

        std::string error;
        uint64_t id = a.transport.sendFile("VK3ABC", scratch.write("x.bin", 500), error);
        runBoth(a, b, [&]() { return newestFile(b.transport, false).state == FileState::Asking; });
        CHECK(newestFile(b.transport, false).state == FileState::Asking);
        (expiring == 0 ? a : b).nowMs += Data2G::FileTransferEngine::OFFER_EXPIRY_MS;
        // Far less than the other side's own clock would take.
        runBoth(a, b, [&]() {
            return fileState(a.transport, id) == FileState::Expired &&
                   newestFile(b.transport, false).state == FileState::Expired;
        }, 300);
        CHECK(fileState(a.transport, id) == FileState::Expired);
        CHECK(newestFile(b.transport, false).state == FileState::Expired);
    }
}

// A piece written while some of the last is unacknowledged. Before the
// host reads it, it reports the far end's acknowledgements of the last
// one, down to 0: those settle only the last piece, not the one unread,
// and nothing more is written until the host has read it.
void testCountsBeforeTheHostReadsAWriteSettleNothingOfIt()
{
    FakeHostPair hosts;
    holdSide0(hosts);
    Station a("W1AW", hosts.sides[0], 3);
    Station b("VK3ABC", hosts.sides[1], 3);
    CHECK(sessionsReady(hosts, a, b));
    FileScratch scratch("unread");
    b.transport.setFileAutoAccept(scratch.dir.string(), {"W1AW"});

    std::string error;
    std::string source = scratch.write("large.bin", 40000);
    uint64_t id = a.transport.sendFile("VK3ABC", source, error);
    runAcking(hosts, a, b, 1500, [&]() { return newestFile(b.transport, false).done >= 4096; });
    runBoth(a, b, [&]() { return false; }, 20);
    size_t waiting = hosts.unackedBytes(0);
    CHECK(waiting > Data2GTransport::FILE_PIECE_LOW_WATER);
    CHECK(a.transport.status().sessionUnacked == (int64_t)waiting);

    hosts.holdReads(0);
    hosts.ackSome(0, waiting - 800); // BUFFER 800: the next piece goes
    runBoth(a, b, [&]() { return hosts.unreadBytes(0) > 0; }, 500);
    size_t unread = hosts.unreadBytes(0);
    CHECK(unread > (size_t)Data2G::FILE_PIECE_BYTES);
    hosts.ackSome(0, 500); // BUFFER 300
    runBoth(a, b, [&]() { return a.transport.status().sessionUnacked == 300; }, 500);
    hosts.ackSome(0, 300); // BUFFER 0
    runBoth(a, b, [&]() { return a.transport.status().sessionUnacked == 0; }, 500);
    CHECK(a.transport.status().sessionUnacked == 0);
    runBoth(a, b, [&]() { return false; }, 50);
    CHECK(hosts.unreadBytes(0) == unread);  // nothing more written
    CHECK(newestFile(a.transport, true).done == newestFile(b.transport, false).done);

    hosts.releaseReads(0);
    runAcking(hosts, a, b, 1500, [&]() { return fileState(a.transport, id) == FileState::Delivered; });
    CHECK(fileState(a.transport, id) == FileState::Delivered);
    Data2G::FileTransfer got = newestFile(b.transport, false);
    CHECK(got.state == FileState::Saved && fileContents(got.path) == fileContents(source));
}

// Without sessions there are no files.
void testNoFilesWithoutSessions()
{
    FakeHostPair hosts;
    Station a("W1AW", hosts.sides[0], 3, false);
    CHECK(waitFor([&]() { return hosts.ready(0); }));
    CHECK(!a.transport.sendsFiles());
    FileScratch scratch("no-sessions");
    std::string error;
    CHECK(a.transport.sendFile("VK3ABC", scratch.write("x.bin", 10), error) == 0);
    CHECK(!error.empty());
}


//-------------------------------------------------------------------------
// Files to the whole group: a fake group of data2g-hosts, one a station,
// each with a KISS and a command port. A burst one station sends reaches
// every other station that is listening, less the frames the test's loss
// rule takes, and is acknowledged to the sender at once.
//-------------------------------------------------------------------------

class FakeGroup
{
public:
    struct Side
    {
        int listen[2] = {-1, -1};   // KISS, command
        int client[2] = {-1, -1};
        int kissPort = 0, commandPort = 0;
        std::string call;
        int groupPort = 0;
        std::string groupMode = "qpsk-r1/5";
        bool listening = true;      // false: hears nothing on the group
        bool holding = false;       // true: keeps what it is given, unsent
        Data2G::KissDecoder kiss;
        Data2G::LineSplitter lines;
    };

    struct Burst
    {
        int from = 0;
        std::string mode;
        std::vector<std::vector<uint8_t>> frames;
    };

    // from, to, the frame: true to lose it.
    using LossRule = std::function<bool(int from, int to, const std::vector<uint8_t>& frame)>;

    explicit FakeGroup(int count)
        : sides(count)
    {
        for (Side& side : sides)
        {
            side.listen[0] = listenOnLoopback(side.kissPort);
            side.listen[1] = listenOnLoopback(side.commandPort);
        }
        thread = std::thread([this]() { run(); });
    }

    ~FakeGroup()
    {
        stopping = true;
        thread.join();
        for (Side& side : sides)
        {
            for (int i = 0; i < 2; i++)
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

    bool ready(int side)
    {
        return with([&]() { return sides[(size_t)side].groupPort != 0 && sides[(size_t)side].client[0] >= 0; });
    }

    void setLoss(LossRule rule)
    {
        with([&]() {
            loss = std::move(rule);
            return 0;
        });
    }

    void setListening(int side, bool on)
    {
        with([&]() {
            sides[(size_t)side].listening = on;
            return 0;
        });
    }

    std::vector<Burst> sent()
    {
        return with([&]() { return bursts; });
    }

    // A side's host keeps its keyings, as when the channel stays busy.
    void setHolding(int side, bool on)
    {
        with([&]() {
            sides[(size_t)side].holding = on;
            return 0;
        });
    }

    size_t heldCount()
    {
        return with([&]() { return held.size(); });
    }

    // The first keying held is dropped, and the host says so.
    void dropFirstHeld()
    {
        with([&]() {
            if (held.empty()) return 0;
            Held h = held.front();
            held.erase(held.begin());
            Side& side = sides[(size_t)h.burst.from];
            say(side, "BCAST " + std::to_string(side.groupPort) + " DROPPED " + std::to_string(h.burst.frames.size()));
            return 0;
        });
    }

    // The rest go, in turn.
    void releaseHeld()
    {
        with([&]() {
            for (Held& h : held) transmit(h.burst, h.tags);
            held.clear();
            return 0;
        });
    }

    // Keyings from a station whose first frame is of this type.
    int sentOfType(int from, Data2G::GroupFileFrame type)
    {
        int n = 0;
        for (const Burst& b : sent())
        {
            if (b.from == from && !b.frames.empty() && b.frames[0][0] == (uint8_t)type) n++;
        }
        return n;
    }

    std::vector<Side> sides;

private:
    void say(Side& side, const std::string& line)
    {
        if (side.client[1] < 0) return;
        std::string text = line + "\r";
        send(side.client[1], text.data(), text.size(), MSG_NOSIGNAL);
    }

    void command(int s, const std::string& line)
    {
        Side& side = sides[(size_t)s];
        std::vector<std::string> w;
        std::string word;
        for (char c : line + " ")
        {
            if (c != ' ')
            {
                word.push_back(c);
                continue;
            }
            if (!word.empty()) w.push_back(word);
            word.clear();
        }
        if (w.empty()) return;

        if (w[0] == "MYCALL" && w.size() == 2)
        {
            side.call = w[1];
            say(side, "OK");
        }
        else if (w[0] == "BCAST" && w.size() >= 3 && w[1] == "OPEN")
        {
            side.groupPort = 1;
            say(side, "BCAST PORT 1");
        }
        else if (w[0] == "BCAST" && w.size() == 4 && w[1] == "MODE")
        {
            side.groupMode = w[3];
            say(side, "OK");
        }
        else if (w[0] == "MODES")
        {
            for (const Data2G::ModeInfo& m : Data2G::knownModes())
            {
                char text[200];
                snprintf(text, sizeof(text), "MODE %s %d %d %d %.2f %.2f", m.name.c_str(), m.bandwidthHz,
                         m.bytesPerCodeword, m.maxCodewords, m.secondsAtOne, m.secondsAtMax);
                say(side, text);
            }
            say(side, "OK");
        }
        else
        {
            say(side, "OK");
        }
    }

    void kissIn(int s, const uint8_t* bytes, int length)
    {
        Side& side = sides[(size_t)s];
        std::vector<Data2G::KissFrame> frames;
        side.kiss.feed(bytes, length, frames);
        if (frames.empty()) return;

        Burst burst;
        burst.from = s;
        burst.mode = side.groupMode;
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
            burst.frames.push_back(data);
        }
        if (side.holding)
        {
            held.push_back({burst, tags});
            return;
        }
        transmit(burst, tags);
    }

    void transmit(const Burst& burst, const std::vector<uint16_t>& tags)
    {
        int s = burst.from;
        Side& side = sides[(size_t)s];
        bursts.push_back(burst);

        say(side, "PTT ON");
        say(side, "MODE " + side.groupMode);
        for (size_t to = 0; to < sides.size(); to++)
        {
            Side& other = sides[to];
            if ((int)to == s || other.groupPort == 0 || !other.listening) continue;
            say(other, "BUSY ON");
            int lost = 0;
            for (const auto& data : burst.frames)
            {
                if (loss && loss(s, (int)to, data))
                {
                    lost++;
                    continue;
                }
                std::vector<uint8_t> kiss = Data2G::kissEncode(other.groupPort, data);
                if (other.client[0] >= 0) send(other.client[0], kiss.data(), kiss.size(), MSG_NOSIGNAL);
            }
            say(other, "BCAST " + std::to_string(other.groupPort) + " HEARD " + side.call);
            if (lost > 0) say(other, "BCAST " + std::to_string(other.groupPort) + " LOST " + std::to_string(lost));
            say(other, "BUSY OFF");
        }
        say(side, "PTT OFF");
        for (uint16_t tag : tags)
        {
            std::vector<uint8_t> ack = Data2G::kissEncodeAckMode(side.groupPort, tag, {});
            send(side.client[0], ack.data(), ack.size(), MSG_NOSIGNAL);
        }
    }

    void run()
    {
        std::vector<uint8_t> buffer(4096);
        while (!stopping)
        {
            std::vector<pollfd> fds;
            std::vector<std::pair<int, int>> what; // side, socket (0-1 listen, 2-3 client)
            with([&]() {
                for (size_t s = 0; s < sides.size(); s++)
                {
                    for (int i = 0; i < 2; i++)
                    {
                        fds.push_back({sides[s].listen[i], POLLIN, 0});
                        what.push_back({(int)s, i});
                        if (sides[s].client[i] >= 0)
                        {
                            fds.push_back({sides[s].client[i], POLLIN, 0});
                            what.push_back({(int)s, 2 + i});
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
                Side& side = sides[(size_t)s];
                int which = what[i].second;
                if (which < 2)
                {
                    if (side.client[which] >= 0) close(side.client[which]);
                    side.client[which] = accept(side.listen[which], nullptr, nullptr);
                    int one = 1;
                    setsockopt(side.client[which], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    continue;
                }
                int c = which - 2;
                if (side.client[c] < 0) continue;
                ssize_t got = recv(side.client[c], buffer.data(), buffer.size(), 0);
                if (got <= 0)
                {
                    close(side.client[c]);
                    side.client[c] = -1;
                    if (c == 1) side.groupPort = 0;
                    continue;
                }
                if (c == 0)
                {
                    kissIn(s, buffer.data(), (int)got);
                }
                else
                {
                    std::vector<std::string> lines;
                    side.lines.feed((const char*)buffer.data(), (int)got, lines);
                    for (const std::string& line : lines) command(s, line);
                }
            }
        }
    }

    struct Held
    {
        Burst burst;
        std::vector<uint16_t> tags;
    };

    std::mutex mutex;
    std::atomic<bool> stopping{false};
    LossRule loss;
    std::vector<Burst> bursts;
    std::vector<Held> held;
    std::thread thread;
};

using GroupState = Data2G::GroupFile::State;

// Stations on the group, without sessions, each stepping its own clock.
struct GroupStations
{
    GroupStations(FakeGroup& hosts, const std::vector<std::string>& calls, int gear = 5)
    {
        for (size_t i = 0; i < calls.size(); i++)
        {
            stations.push_back(std::make_unique<Station>(calls[i], hosts.sides[i], gear, false));
        }
        CHECK(waitFor(
            [&]() {
                for (size_t i = 0; i < calls.size(); i++)
                {
                    if (!hosts.ready((int)i) || stations[i]->transport.status().groupPort == 0) return false;
                    std::string why;
                    if (!stations[i]->transport.sendsGroupFiles(why)) return false;
                }
                return true;
            },
            5000));
    }

    Station& operator[](size_t i) { return *stations[i]; }

    // The clocks run about 50 times faster than the transports' threads
    // poll, so a request slot of 6 s spans a few of their turns.
    void step(uint64_t ms = 100)
    {
        for (auto& s : stations) s->step(ms);
        std::this_thread::sleep_for(std::chrono::microseconds(2000));
    }

    bool runUntil(const std::function<bool()>& done, int steps = 20000, uint64_t ms = 100)
    {
        for (int i = 0; i < steps && !done(); i++) step(ms);
        return done();
    }

    std::vector<std::unique_ptr<Station>> stations;
};

Data2G::GroupFile groupFile(Station& station, bool outgoing)
{
    Data2G::GroupFile found;
    found.id = 0;
    for (const Data2G::GroupFile& f : station.transport.groupFiles())
    {
        if (f.outgoing == outgoing) found = f;
    }
    return found;
}

bool isGroupData(const std::vector<uint8_t>& frame, int index = -1)
{
    Data2G::GroupData d;
    return Data2G::decodeGroupData(frame.data(), frame.size(), d) && (index < 0 || d.index == index);
}

// One sender, two listeners losing different pieces: both ask, both are
// served in one round, and both save the file; chat never sees a file
// frame; every keying goes in Duet's mode.
void testAFileToTheGroup()
{
    FakeGroup hosts(3);
    GroupStations group(hosts, {"AG7EW", "K7ABC", "VK3XYZ"});
    FileScratch scratch("group");
    std::string source = scratch.write("FRED.TXT", 7000);
    group[1].transport.setGroupFileAutoReceive((scratch.dir / "k7abc").string());
    group[2].transport.setGroupFileAutoReceive((scratch.dir / "vk3xyz").string());

    std::set<std::pair<int, int>> lose = {{1, 2}, {2, 5}, {2, 6}};
    hosts.setLoss([&](int from, int to, const std::vector<uint8_t>& frame) {
        Data2G::GroupData d;
        if (from != 0 || !Data2G::decodeGroupData(frame.data(), frame.size(), d)) return false;
        return lose.erase({to, d.index}) > 0; // once each
    });

    // The estimate the window shows: 31 pieces at Duet, about half a minute.
    Data2G::GroupFileEstimate estimate = group[0].transport.groupFileEstimate(7000, 5);
    CHECK(estimate.pieces == 31 && estimate.airSeconds < 60);

    std::string error;
    uint64_t id = group[0].transport.sendGroupFile(source, error);
    CHECK(id != 0);
    CHECK(group.runUntil([&]() {
        return groupFile(group[1], false).state == GroupState::Saved &&
               groupFile(group[2], false).state == GroupState::Saved;
    }));
    for (size_t n : {1, 2})
    {
        Data2G::GroupFile got = groupFile(group[n], false);
        CHECK(got.name == "FRED.TXT" && got.sender == "AG7EW" && got.autoReceived);
        CHECK(fileContents(got.path) == fileContents(source));
        CHECK(group[n].observer.receivedTexts().empty()); // chat saw none of it
    }
    CHECK(hosts.sentOfType(1, Data2G::GroupFileFrame::Request) + hosts.sentOfType(2, Data2G::GroupFileFrame::Request) >=
          1);
    CHECK(hosts.sentOfType(0, Data2G::GroupFileFrame::Grant) == 1);

    CHECK(group.runUntil([&]() { return groupFile(group[0], true).state == GroupState::Ended; }));
    CHECK(groupFile(group[0], true).endReason == Data2G::GroupFileEnd::Quiet);
    for (const FakeGroup::Burst& b : hosts.sent()) CHECK(b.mode == "w48-16qam-r1/2");
}

// A listener tuned in part way through finishes the file from the
// repairs, and learns its name from the Announce the Grant brings.
void testALateJoinerOnTheGroup()
{
    FakeGroup hosts(2);
    GroupStations group(hosts, {"AG7EW", "K7ABC"});
    FileScratch scratch("group-late");
    std::string source = scratch.write("MAP.BIN", 9000);
    group[1].transport.setGroupFileAutoReceive((scratch.dir / "in").string());
    hosts.setListening(1, false);

    std::string error;
    CHECK(group[0].transport.sendGroupFile(source, error) != 0);
    CHECK(group.runUntil([&]() {
        return hosts.sentOfType(0, Data2G::GroupFileFrame::Announce) == 2 &&
               hosts.sentOfType(0, Data2G::GroupFileFrame::Data) >= 4;
    }));
    hosts.setListening(1, true);
    CHECK(group.runUntil([&]() { return groupFile(group[1], false).state == GroupState::Saved; }));
    Data2G::GroupFile got = groupFile(group[1], false);
    CHECK(got.name == "MAP.BIN" && fileContents(got.path) == fileContents(source));
}

// Sent again, the same file is the same transfer: the station that kept
// its pieces from the first time finishes it.
void testTheSameFileAgainOnTheGroup()
{
    FakeGroup hosts(2);
    GroupStations group(hosts, {"AG7EW", "K7ABC"});
    FileScratch scratch("group-again");
    std::string source = scratch.write("NET.TXT", 4000);
    group[1].transport.setGroupFileAutoReceive((scratch.dir / "in").string());

    // The first time half of it is lost and its requests are not heard.
    std::atomic<bool> second{false};
    hosts.setLoss([&](int from, int, const std::vector<uint8_t>& frame) {
        if (second) return from == 0 && isGroupData(frame) && frame[5] % 2 == 1;
        return from == 1 || (isGroupData(frame) && frame[5] % 2 == 0);
    });
    std::string error;
    CHECK(group[0].transport.sendGroupFile(source, error) != 0);
    CHECK(group.runUntil([&]() { return groupFile(group[0], true).state == GroupState::Ended; }));
    Data2G::GroupFile kept = groupFile(group[1], false);
    CHECK(kept.state == GroupState::Incomplete && kept.have > 0 && kept.have < kept.pieces);

    second = true;
    CHECK(group[0].transport.sendGroupFile(source, error) != 0);
    CHECK(group.runUntil([&]() { return groupFile(group[1], false).state == GroupState::Saved; }));
    Data2G::GroupFile got = groupFile(group[1], false);
    CHECK(got.fileId == kept.fileId && fileContents(got.path) == fileContents(source));
}

// Cancelled mid-stream: no more pieces, the End twice, and the listener
// deletes what it had.
void testCancellingAGroupFile()
{
    FakeGroup hosts(2);
    GroupStations group(hosts, {"AG7EW", "K7ABC"});
    FileScratch scratch("group-cancel");
    std::string source = scratch.write("BIG.BIN", 20000);
    group[1].transport.setGroupFileAutoReceive((scratch.dir / "in").string());

    std::string error;
    uint64_t id = group[0].transport.sendGroupFile(source, error);
    CHECK(group.runUntil([&]() { return groupFile(group[1], false).have >= 16; }));
    CHECK(group[0].transport.cancelGroupFile(id));
    size_t before = hosts.sent().size();
    CHECK(group.runUntil([&]() { return groupFile(group[0], true).state == GroupState::Ended; }));
    CHECK(groupFile(group[0], true).endReason == Data2G::GroupFileEnd::Cancelled);
    std::vector<FakeGroup::Burst> after = hosts.sent();
    int pieces = 0;
    for (size_t i = before; i < after.size(); i++)
    {
        if (after[i].from == 0 && isGroupData(after[i].frames[0])) pieces++;
    }
    CHECK(pieces <= 1); // at most the keying already with the host
    CHECK(hosts.sentOfType(0, Data2G::GroupFileFrame::End) == 2);
    CHECK(group.runUntil([&]() { return groupFile(group[1], false).state == GroupState::CancelledThere; }));
    CHECK(groupFile(group[1], false).have == 0);
    std::error_code ec;
    CHECK(!fs::exists(scratch.dir / "in" / "BIG.BIN", ec));
}

// A listener the sender hears asking, whose resends never get through:
// repairs go on until the deadline, ten minutes, then end.
void testAGroupFileDeadline()
{
    FakeGroup hosts(2);
    GroupStations group(hosts, {"AG7EW", "K7ABC"});
    FileScratch scratch("group-deadline");
    std::string source = scratch.write("X.BIN", 3000);
    group[1].transport.setGroupFileAutoReceive((scratch.dir / "in").string());

    std::atomic<bool> streamed{false};
    hosts.setLoss([&](int from, int, const std::vector<uint8_t>& frame) {
        if (from != 0) return false;
        if (!frame.empty() && frame[0] == (uint8_t)Data2G::GroupFileFrame::Window) streamed = true;
        return isGroupData(frame, 5) || (streamed && isGroupData(frame));
    });
    std::string error;
    CHECK(group[0].transport.sendGroupFile(source, error) != 0);
    uint64_t started = group[0].nowMs;
    CHECK(group.runUntil([&]() { return groupFile(group[0], true).state == GroupState::Ended; }, 12000));
    CHECK(groupFile(group[0], true).endReason == Data2G::GroupFileEnd::Deadline);
    CHECK(group[0].nowMs - started >= Data2G::GroupFileEngine::MIN_SERVICE_MS);
    CHECK(hosts.sentOfType(0, Data2G::GroupFileFrame::Grant) >= 3);
    CHECK(group.runUntil([&]() { return groupFile(group[1], false).state == GroupState::Incomplete; }));
}

// A message typed while a file streams goes out before the next piece
// keying, at once, with no pause of its own.
void testChatGoesAheadOfTheFile()
{
    FakeGroup hosts(2);
    GroupStations group(hosts, {"AG7EW", "K7ABC"});
    FileScratch scratch("group-chat");
    std::string source = scratch.write("BIG.BIN", 20000);

    std::string error;
    CHECK(group[0].transport.sendGroupFile(source, error) != 0);
    CHECK(group.runUntil([&]() { return hosts.sentOfType(0, Data2G::GroupFileFrame::Data) >= 2; }));
    std::string why;
    CHECK(group[0].protocol.sendMessage("hello while a file goes", "", why));
    CHECK(group.runUntil([&]() { return !group[1].observer.receivedTexts().empty(); }));
    std::vector<TextMessage> got = group[1].observer.receivedTexts();
    CHECK(!got.empty() && got[0].text == "hello while a file goes");

    // The chat keying went between two file keyings.
    std::vector<FakeGroup::Burst> bursts = hosts.sent();
    int chatAt = -1;
    for (size_t i = 0; i < bursts.size(); i++)
    {
        if (bursts[i].from == 0 && !bursts[i].frames.empty() &&
            !Data2G::isGroupFileFrame(bursts[i].frames[0].data(), bursts[i].frames[0].size()))
        {
            chatAt = (int)i;
            break;
        }
    }
    CHECK(chatAt > 0);
    int dataBefore = 0;
    for (int i = 0; i < chatAt; i++)
    {
        if (isGroupData(bursts[(size_t)i].frames[0])) dataBefore++;
    }
    CHECK(dataBefore >= 2 && dataBefore <= 3);
}

// Without a callsign, or with sending not allowed, nothing goes.
void testGroupFilesNeedTheGroup()
{
    FakeGroup hosts(1);
    GroupStations group(hosts, {"AG7EW"});
    FileScratch scratch("group-inhibit");
    std::string source = scratch.write("A.BIN", 500);
    group[0].transport.setFilesInhibited(true);
    std::string error;
    CHECK(group[0].transport.sendGroupFile(source, error) == 0 && !error.empty());
    group[0].transport.setFilesInhibited(false);
    CHECK(group[0].transport.sendGroupFile(source, error) != 0);
    group[0].transport.stop();
    CHECK(groupFile(group[0], true).state == GroupState::Ended);
}

// A chat keying given up on while data2g-host still holds it, dropped by
// the host while a file keying waits behind it: the drop is the chat's,
// so the file keying is not sent a second time.
void testAStaleChatDropIsNotTheFiles()
{
    FakeGroup hosts(2);
    GroupStations group(hosts, {"AG7EW", "K7ABC"});
    FileScratch scratch("group-stale");
    std::string source = scratch.write("FRED.TXT", 7000);
    group[1].transport.setGroupFileAutoReceive((scratch.dir / "in").string());

    hosts.setHolding(0, true);
    std::string why;
    CHECK(group[0].protocol.sendMessage("held up", "", why));
    CHECK(group.runUntil([&]() { return hosts.heldCount() == 1; }));
    // Past the time the chat keying is given up on.
    group.runUntil([]() { return false; }, 1300);
    CHECK(hosts.heldCount() == 1);

    std::string error;
    CHECK(group[0].transport.sendGroupFile(source, error) != 0);
    CHECK(group.runUntil([&]() { return hosts.heldCount() == 2; }));
    hosts.dropFirstHeld();
    group.runUntil([]() { return false; }, 20);
    hosts.setHolding(0, false);
    hosts.releaseHeld();

    CHECK(group.runUntil([&]() { return groupFile(group[1], false).state == GroupState::Saved; }));
    CHECK(group.runUntil([&]() { return groupFile(group[0], true).state == GroupState::Ended; }));
    CHECK(fileContents(groupFile(group[1], false).path) == fileContents(source));
    // One before piece 0 and one before piece 24, as on a clear channel.
    CHECK(hosts.sentOfType(0, Data2G::GroupFileFrame::Announce) == 2);
}

} // namespace

int main()
{
    testKissPortsAndAckMode();
    testCommandLines();
    testTemposMapToModes();
    testSessionStream();
    testFileRecordsInTheStream();
    testCommandCallsign();
    testNotConnectedRefusesToTransmit();
    testBroadcastGoesToTheGroupAtAdagio();
    testDuetUsesAFastWideModeOrTheNarrowHostsBest();
    testDirectedMessageGoesThroughASession();
    testEachMessageIsSettledAsItsBytesAreAcknowledged();
    testAnOlderHostSettlesTheBatchTogether();
    testALostSessionFailsTheMessage();
    testAPingThroughASessionIsAnsweredByTheModem();
    testDeselectingTheStationEndsTheSession();
    testDeselectingAQuietSessionDisconnects();
    testRestartingTheTransportEndsWhatItHeld();
    testAStationWithoutSessionsGetsTheGroup();
    testLosingTheCommandPortClearsBusyAndReopens();
    testACallsignChangeReopensTheGroup();
    testABroadcastHeldByASessionSaysSo();
    testABroadcastGoesWhenTheSessionEnds();
    testABroadcastNeverSentEndsNotSent();
    testAFileGoesThroughASession();
    testAFileDeclined();
    testTheSenderCancelsAFile();
    testTheReceiverCancelsAFile();
    testALostSessionFailsTheFile();
    testDeselectingTheStationCancelsTheFile();
    testDeselectingWhileAnOfferIsUp();
    testInhibitedStopsSessionFiles();
    testAnUnansweredOfferExpires();
    testAnOddNameIsSavedSafely();
    testChatOvertakesALargeFile();
    testAWriteReadWithAnAckStillMovesOn();
    testAnOfferCrossingADisconnectIsSeen();
    testAnOlderGlissandoCantTakeTheFile();
    testLosingTheCommandPortFailsAnOffer();
    testTwoFilesForOneStationGoInTurn();
    testAFileThroughAnOlderHost();
    testAnOfferExpiresOnOneClock();
    testCountsBeforeTheHostReadsAWriteSettleNothingOfIt();
    testNoFilesWithoutSessions();
    testAFileToTheGroup();
    testALateJoinerOnTheGroup();
    testTheSameFileAgainOnTheGroup();
    testCancellingAGroupFile();
    testAGroupFileDeadline();
    testChatGoesAheadOfTheFile();
    testGroupFilesNeedTheGroup();
    testAStaleChatDropIsNotTheFiles();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    printf("all Data2G checks passed\n");
    return 0;
}
