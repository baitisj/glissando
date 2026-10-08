//=========================================================================
// Name:            Data2GBroadcastTest.cpp
// Purpose:         Files broadcast to the GLISS group, without sockets: the
//                  frames on their own, then whole transfers between a
//                  sender and several listeners on a simulated channel,
//                  with a simulated clock, losses chosen by each test, and
//                  every keying taking the airtime its Data2G mode says.
//
// Written for Glissando; no Data2G code is used (see docs/DATA2G.md).
//=========================================================================

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../Data2GBroadcast.h"
#include "../Data2GFileTransfer.h"
#include "../Data2GLink.h"
#include "../FrameCodec.h"
#include "../Sha256.h"

using namespace TextMessaging;
using namespace TextMessaging::Data2G;
using State = GroupFile::State;

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

namespace fs = std::filesystem;

struct Scratch
{
    explicit Scratch(const std::string& name)
    {
        std::random_device random;
        dir = fs::temp_directory_path() / ("glissando-group-test-" + name + "-" + std::to_string(random()));
        fs::create_directories(dir);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string path(const std::string& name) const { return utf8FromPath(dir / pathFromUtf8(name)); }

    std::string write(const std::string& name, size_t size, int salt = 0)
    {
        std::string content(size, '\0');
        for (size_t i = 0; i < size; i++) content[i] = (char)((i * 7 + i / 251 + (size_t)salt) & 0xFF);
        std::ofstream out(dir / pathFromUtf8(name), std::ios::binary);
        out.write(content.data(), (std::streamsize)content.size());
        return path(name);
    }

    std::string folder(const std::string& name) const
    {
        fs::create_directories(dir / name);
        return utf8FromPath(dir / name);
    }

    fs::path dir;
};

std::string readFile(const std::string& path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

//-------------------------------------------------------------------------
// Frames
//-------------------------------------------------------------------------

void testFramesRoundTrip()
{
    Hash8 hash = {1, 2, 3, 4, 5, 6, 7, 8};

    GroupAnnounce a;
    a.fileId = 0x3A7F01;
    a.size = 7000;
    a.pieceBytes = 106;
    a.pieces = 67;
    a.hash = hash;
    a.tempo = 0;
    a.airSeconds = 2555;
    a.call = "AG7EW";
    a.name = "FRED.TXT";
    std::vector<uint8_t> bytes = encodeGroupAnnounce(a);
    CHECK(bytes.size() == 37); // as the design has it for AG7EW and FRED.TXT
    GroupAnnounce a2;
    CHECK(decodeGroupAnnounce(bytes.data(), bytes.size(), a2));
    CHECK(a2.fileId == a.fileId && a2.size == 7000 && a2.pieceBytes == 106 && a2.pieces == 67 && a2.hash == hash &&
          a2.tempo == 0 && a2.airSeconds == 2555 && a2.call == "AG7EW" && a2.name == "FRED.TXT");
    // A piece count that doesn't match the size is not a file.
    a.pieces = 66;
    bytes = encodeGroupAnnounce(a);
    CHECK(!decodeGroupAnnounce(bytes.data(), bytes.size(), a2));

    GroupData d;
    d.fileId = 0x3A7F01;
    d.index = 23;
    d.bytes.assign(106, 0xC0);
    bytes = encodeGroupData(d);
    CHECK(bytes.size() == 112); // 3 Adagio codewords with Data2G's 2-byte length
    GroupData d2;
    CHECK(decodeGroupData(bytes.data(), bytes.size(), d2) && d2.index == 23 && d2.bytes == d.bytes);

    GroupWindow w;
    w.fileId = 0x3A7F01;
    w.round = 0;
    w.size = 7000;
    w.pieceBytes = 106;
    w.pieces = 67;
    w.hash = hash;
    w.slots = 4;
    w.slotHalfSeconds = 40;
    w.serviceLeftTens = 258;
    bytes = encodeGroupWindow(w);
    CHECK(bytes.size() == 24);
    GroupWindow w2;
    CHECK(decodeGroupWindow(bytes.data(), bytes.size(), w2) && w2.slots == 4 && w2.slotHalfSeconds == 40 &&
          w2.serviceLeftTens == 258 && w2.priorityCall.empty() && w2.tempo == -1);
    w.priorityCall = "K7ABC";
    w.tempo = 4;
    bytes = encodeGroupWindow(w);
    CHECK(bytes.size() == 30 && bytes.size() <= 36); // one Adagio codeword
    CHECK(decodeGroupWindow(bytes.data(), bytes.size(), w2) && w2.priorityCall == "K7ABC" && w2.tempo == 4);

    GroupRequest r;
    r.fileId = 0x3A7F01;
    r.round = 2;
    r.missingTotal = 4;
    r.call = "K7ABC";
    r.pieces = {4, 9, 17, 18};
    size_t listed = 0;
    bytes = encodeGroupRequest(r, 36, listed);
    CHECK(listed == 4 && bytes.size() <= 36);
    GroupRequest r2;
    CHECK(decodeGroupRequest(bytes.data(), bytes.size(), r2) && r2.pieces == r.pieces && r2.call == "K7ABC" &&
          r2.round == 2 && r2.missingTotal == 4 && (r2.flags & REQUEST_TRUNCATED) == 0);

    GroupGrant g;
    g.fileId = 0x3A7F01;
    g.round = 2;
    g.call = "K7ABC";
    g.pieces = {17, 4, 9};
    bytes = encodeGroupGrant(g, 255, listed);
    GroupGrant g2;
    CHECK(decodeGroupGrant(bytes.data(), bytes.size(), g2) && g2.call == "K7ABC" &&
          (g2.pieces == std::vector<int>{4, 9, 17}));

    GroupEnd e;
    e.fileId = 0x3A7F01;
    e.reason = GroupFileEnd::Cancelled;
    e.rounds = 3;
    bytes = encodeGroupEnd(e);
    CHECK(bytes.size() == 6);
    GroupEnd e2;
    CHECK(decodeGroupEnd(bytes.data(), bytes.size(), e2) && e2.reason == GroupFileEnd::Cancelled && e2.rounds == 3);
}

// Every file frame has a first byte whose top nibble is 0: chat never
// decodes one, and every chat frame's is something else.
void testChatNeverSeesThem()
{
    Hash8 hash{};
    std::vector<std::vector<uint8_t>> frames;
    GroupAnnounce a;
    a.fileId = 1;
    a.size = 10;
    a.pieceBytes = 10;
    a.pieces = 1;
    a.hash = hash;
    a.call = "W1AW";
    a.name = "x";
    frames.push_back(encodeGroupAnnounce(a));
    frames.push_back(encodeGroupData({1, 0, std::vector<uint8_t>(10, 0xFF)}));
    GroupWindow w;
    w.fileId = 1;
    w.size = 10;
    w.pieceBytes = 10;
    w.pieces = 1;
    w.slots = 4;
    w.slotHalfSeconds = 12;
    frames.push_back(encodeGroupWindow(w));
    size_t listed;
    frames.push_back(encodeGroupRequest({1, 0, 0, 1, "W1AW", {0}}, 36, listed));
    frames.push_back(encodeGroupGrant({1, 0, 0, "W1AW", {0}}, 255, listed));
    frames.push_back(encodeGroupEnd({1, GroupFileEnd::Quiet, 0}));
    for (const auto& f : frames)
    {
        CHECK((f[0] >> 4) == 0);
        CHECK(isGroupFileFrame(f.data(), f.size()));
        Frame chat;
        CHECK(!FrameCodec::decode(f.data(), (int)f.size(), chat));
    }

    // Chat frames of every kind are not file frames.
    Frame message;
    message.type = FrameType::Broadcast;
    message.originCallsign = "W1AW";
    message.payload = {'h', 'i'};
    std::vector<uint8_t> chat = FrameCodec::encode(message, TEXT_FRAME_BYTES);
    CHECK(!isGroupFileFrame(chat.data(), chat.size()));
    for (FrameType type : {FrameType::Ping, FrameType::PingAck, FrameType::MessageAck, FrameType::Message})
    {
        Frame f;
        f.type = type;
        f.originCallsign = "W1AW";
        f.destinationCrc = 0x1234;
        std::vector<uint8_t> b = FrameCodec::encode(f, TEXT_FRAME_BYTES);
        CHECK(!b.empty() && !isGroupFileFrame(b.data(), b.size()));
    }
}

void testPieceSets()
{
    // A run is shorter as a range; scattered pieces as a bitmap.
    std::vector<uint8_t> out;
    std::vector<int> run;
    for (int i = 100; i < 160; i++) run.push_back(i);
    CHECK(encodePieceSet(run, 50, out) == run.size());
    CHECK(out[0] == 1 && out.size() == 4);
    std::vector<int> back;
    CHECK(decodePieceSet(out.data(), out.size(), back) && back == run);

    std::vector<int> scattered = {3, 5, 8, 13, 21, 22, 30};
    CHECK(encodePieceSet(scattered, 50, out) == scattered.size());
    CHECK(out[0] == 0 && out.size() == 1 + 2 + 4);
    CHECK(decodePieceSet(out.data(), out.size(), back) && back == scattered);

    // Too many for the room: the lowest that fit.
    std::vector<int> many;
    for (int i = 0; i < 600; i += 3) many.push_back(i);
    size_t listed = encodePieceSet(many, 21, out);
    CHECK(listed > 0 && listed < many.size() && out.size() <= 21);
    CHECK(decodePieceSet(out.data(), out.size(), back) && back.size() == listed && back.back() == many[listed - 1]);

    // A Request that can't list them all says so.
    GroupRequest r;
    r.call = "VK3ABC";
    r.pieces = many;
    r.missingTotal = (int)many.size();
    std::vector<uint8_t> frame = encodeGroupRequest(r, 36, listed);
    CHECK(frame.size() <= 36 && (frame[5] & REQUEST_TRUNCATED) != 0);

    // A range longer than 256 is split.
    std::vector<int> long_;
    for (int i = 0; i < 300; i++) long_.push_back(i);
    CHECK(encodePieceSet(long_, 50, out) == 300);
    CHECK(decodePieceSet(out.data(), out.size(), back) && back == long_);

    // Garbage is refused.
    uint8_t bad[] = {1, 0, 0};
    CHECK(!decodePieceSet(bad, sizeof(bad), back));
    uint8_t badEncoding[] = {7, 0, 0, 0};
    CHECK(!decodePieceSet(badEncoding, sizeof(badEncoding), back));
}

void testGeometry()
{
    const auto& modes = knownModes();
    ModeInfo adagio = modeForGear(1, modes);
    ModeInfo presto = modeForGear(4, modes);
    ModeInfo duet = modeForGear(5, modes);
    CHECK(isCpmMode(adagio) && !isCpmMode(duet));
    CHECK(groupPieceBytes(adagio) == 106);
    CHECK(groupPieceBytes(duet) == 228);
    CHECK(groupPieceBytes(presto) == 220);
    CHECK(groupPiecesPerKeying(adagio, 112) == 1);
    CHECK(groupPiecesPerKeying(duet, 234) == 8);
    CHECK(groupSlotHalfSeconds(adagio) == 40); // 20 s
    CHECK(groupSlotHalfSeconds(duet) == 12);   // 6 s
    for (int gear = 1; gear <= 5; gear++)
    {
        ModeInfo mode = modeForGear(gear, modes);
        CHECK(groupPieceBytes(mode) + 6 <= 255);
    }

    // 7,000 bytes at Adagio: 67 pieces and about 43 min on the air; at
    // Duet 31 pieces in about half a minute.
    GroupFileEstimate slow = estimateGroupFile(7000, adagio);
    CHECK(slow.pieces == 67);
    CHECK(slow.airSeconds > 2400 && slow.airSeconds < 2700);
    CHECK(slow.wallSeconds > slow.airSeconds);
    CHECK(slow.serviceSeconds == slow.airSeconds || std::fabs(slow.serviceSeconds - slow.airSeconds) < 1.0);
    GroupFileEstimate fast = estimateGroupFile(7000, duet);
    CHECK(fast.pieces == 31);
    CHECK(fast.airSeconds > 20 && fast.airSeconds < 40);
    CHECK(fast.serviceSeconds == 600);

    // The id: the same file from the same station, the same id.
    std::vector<uint8_t> content(500, 7);
    Sha256::Digest digest = Sha256::of(content);
    CHECK(groupFileId("AG7EW", digest, 500) == groupFileId("ag7ew", digest, 500));
    CHECK(groupFileId("AG7EW", digest, 500) != groupFileId("K7ABC", digest, 500));
    CHECK(groupFileId("AG7EW", digest, 500) <= 0xFFFFFF);
}

//-------------------------------------------------------------------------
// A simulated GLISS group: stations take turns on one channel (a station
// starts only when it is clear, as data2g-host's carrier sense has it), a
// keying takes its mode's airtime, and when it ends each frame reaches
// each other station unless the test's loss rule says otherwise.
//-------------------------------------------------------------------------

struct Air
{
    int from = 0;
    int gear = 0;
    std::vector<std::vector<uint8_t>> frames;
    uint64_t endsMs = 0;
};

class Group
{
public:
    struct Node
    {
        std::string call;
        GroupFileEngine engine;
        bool listening = true;
        int keyings = 0;
        std::vector<int> gears;     // of every keying it sent
    };

    // from, to, the frame: true to lose it.
    using LossRule = std::function<bool(int from, int to, const std::vector<uint8_t>& frame)>;

    Node& add(const std::string& call, int gear = 5)
    {
        auto node = std::make_unique<Node>();
        node->call = call;
        node->engine.setMyCallsign(call);
        node->engine.setGear(gear);
        node->engine.setRandomSeed((uint32_t)(nodes.size() * 7919 + 17));
        nodes.push_back(std::move(node));
        return *nodes.back();
    }

    Node& operator[](size_t i) { return *nodes[i]; }

    void step(uint64_t ms = 100)
    {
        nowMs += ms;
        bool busy = onAir.frames.size() > 0;
        for (size_t i = 0; i < nodes.size(); i++)
        {
            bool hearing = busy && onAir.from != (int)i;
            nodes[i]->engine.tick(nowMs, hearing, false);
        }

        if (busy && nowMs >= onAir.endsMs)
        {
            Air air = onAir;
            onAir = Air();
            for (size_t to = 0; to < nodes.size(); to++)
            {
                if ((int)to == air.from || !nodes[to]->listening) continue;
                for (const auto& frame : air.frames)
                {
                    if (loss && loss(air.from, (int)to, frame)) continue;
                    log.push_back({air.from, (int)to, frame});
                    nodes[to]->engine.onFrame(frame.data(), frame.size(), nowMs);
                }
            }
            sent.push_back(air);
            nodes[(size_t)air.from]->engine.keyingSent(nowMs);
        }
        if (!onAir.frames.empty()) return;

        // The first station with something due takes the channel.
        for (size_t i = 0; i < nodes.size(); i++)
        {
            Node& node = *nodes[i];
            int gear = node.engine.dueGear(nowMs);
            if (gear == 0) continue;
            std::vector<std::vector<uint8_t>> frames;
            if (!node.engine.takeKeying(nowMs, frames)) continue;
            ModeInfo mode = modeForGear(gear, knownModes());
            std::vector<int> sizes;
            for (const auto& f : frames) sizes.push_back((int)f.size());
            onAir.from = (int)i;
            onAir.gear = gear;
            onAir.frames = frames;
            onAir.endsMs = nowMs + (uint64_t)(mode.burstSeconds(sizes) * 1000.0) + 500; // and PTT
            node.keyings++;
            node.gears.push_back(gear);
            break;
        }
    }

    // Runs until done says so, or for at most this long.
    bool runUntil(const std::function<bool()>& done, uint64_t maxMs = 60ull * 60 * 1000)
    {
        uint64_t until = nowMs + maxMs;
        while (nowMs < until)
        {
            if (done()) return true;
            step();
        }
        return done();
    }

    GroupFile file(size_t node, bool outgoing, size_t nth = 0)
    {
        size_t seen = 0;
        for (const GroupFile& f : nodes[node]->engine.files(nowMs))
        {
            if (f.outgoing == outgoing && seen++ == nth) return f;
        }
        GroupFile none;
        none.id = 0;
        none.state = State::Heard;
        return none;
    }

    // The keyings a station sent whose first frame is of a type.
    int sentOfType(int from, GroupFileFrame type) const
    {
        int n = 0;
        for (const Air& air : sent)
        {
            if (air.from == from && !air.frames.empty() && air.frames[0][0] == (uint8_t)type) n++;
        }
        return n;
    }

    std::vector<GroupGrant> grants() const
    {
        std::vector<GroupGrant> out;
        for (const Air& air : sent)
        {
            for (const auto& f : air.frames)
            {
                GroupGrant g;
                if (decodeGroupGrant(f.data(), f.size(), g)) out.push_back(g);
            }
        }
        return out;
    }

    std::vector<GroupRequest> requestsFrom(int from) const
    {
        std::vector<GroupRequest> out;
        for (const Air& air : sent)
        {
            if (air.from != from) continue;
            for (const auto& f : air.frames)
            {
                GroupRequest r;
                if (decodeGroupRequest(f.data(), f.size(), r)) out.push_back(r);
            }
        }
        return out;
    }

    struct Heard
    {
        int from;
        int to;
        std::vector<uint8_t> frame;
    };

    std::vector<std::unique_ptr<Node>> nodes;
    LossRule loss;
    uint64_t nowMs = 1000;
    Air onAir;
    std::vector<Air> sent;
    std::vector<Heard> log;
};

bool isData(const std::vector<uint8_t>& frame, int index = -1)
{
    GroupData d;
    return decodeGroupData(frame.data(), frame.size(), d) && (index < 0 || d.index == index);
}

bool isType(const std::vector<uint8_t>& frame, GroupFileFrame type)
{
    return !frame.empty() && frame[0] == (uint8_t)type;
}

// Clean channel at Duet: both listeners have it without asking, the sender
// closes after three quiet windows, and the listeners never key.
void testCleanChannel()
{
    Scratch scratch("clean");
    std::string source = scratch.write("FRED.TXT", 7000);
    Group group;
    group.add("AG7EW");
    group.add("K7ABC").engine.setAutoReceive(scratch.folder("k7abc"));
    group.add("VK3XYZ").engine.setAutoReceive(scratch.folder("vk3xyz"));

    std::string error;
    uint64_t id = group[0].engine.send(source, group.nowMs, error);
    CHECK(id != 0 && error.empty());
    CHECK(group.file(0, true).state == State::Sending && group.file(0, true).pieces == 31);

    CHECK(group.runUntil([&]() { return group.file(0, true).state == State::Ended; }));
    GroupFile sent = group.file(0, true);
    CHECK(sent.endReason == GroupFileEnd::Quiet);
    for (size_t n : {1, 2})
    {
        GroupFile got = group.file(n, false);
        CHECK(got.state == State::Saved && got.name == "FRED.TXT" && got.sender == "AG7EW" && got.size == 7000);
        CHECK(readFile(got.path) == readFile(source));
        CHECK(got.autoReceived);
        CHECK(group[n].keyings == 0); // strictly silent
    }
    // Two Announces (before pieces 0 and 24), four keyings of pieces, three
    // windows and the End twice.
    CHECK(group.sentOfType(0, GroupFileFrame::Announce) == 2);
    CHECK(group.sentOfType(0, GroupFileFrame::Data) == 4);
    CHECK(group.sentOfType(0, GroupFileFrame::Window) == 3);
    CHECK(group.sentOfType(0, GroupFileFrame::End) == 2);
    // A yield of at least 4 s between our keyings.
    for (size_t i = 1; i < group.sent.size(); i++)
    {
        CHECK(group.sent[i].endsMs - group.sent[i - 1].endsMs >= GroupFileEngine::YIELD_MS);
    }
}

// Two listeners losing different pieces: they ask in the window, one is
// named first in line, and the Grant resends both stations' pieces.
void testTwoLossyListeners()
{
    Scratch scratch("lossy");
    std::string source = scratch.write("LOG.TXT", 5000);
    Group group;
    group.add("AG7EW");
    group.add("K7ABC").engine.setAutoReceive(scratch.folder("a"));
    group.add("VK3XYZ").engine.setAutoReceive(scratch.folder("b"));

    // First pass only: K7ABC loses 3 and 7, VK3XYZ loses 7, 12 and the
    // first Announce.
    std::set<std::pair<int, int>> lost = {{1, 3}, {1, 7}, {2, 7}, {2, 12}};
    std::set<int> seenOnce;
    bool firstAnnounceLost = false;
    group.loss = [&](int from, int to, const std::vector<uint8_t>& frame) {
        if (from != 0) return false;
        if (isType(frame, GroupFileFrame::Announce) && to == 2 && !firstAnnounceLost)
        {
            firstAnnounceLost = true;
            return true;
        }
        GroupData d;
        if (!decodeGroupData(frame.data(), frame.size(), d)) return false;
        if (lost.count({to, d.index}) == 0) return false;
        lost.erase({to, d.index}); // resends get through
        return true;
    };

    std::string error;
    CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
    CHECK(group.runUntil([&]() {
        return group.file(1, false).state == State::Saved && group.file(2, false).state == State::Saved;
    }));
    for (size_t n : {1, 2}) CHECK(readFile(group.file(n, false).path) == readFile(source));

    // Each asked; one was first in line, and one Grant carried all four
    // pieces, so one round did it.
    std::vector<GroupGrant> grants = group.grants();
    CHECK(grants.size() == 1);
    if (!grants.empty())
    {
        CHECK(grants[0].call == "K7ABC" || grants[0].call == "VK3XYZ");
        CHECK((grants[0].pieces == std::vector<int>{3, 7, 12}));
    }
    CHECK(group.requestsFrom(1).size() + group.requestsFrom(2).size() >= 1);
    // The station that asked second left out 7, already asked for.
    std::vector<GroupRequest> second = group.requestsFrom(2);
    std::vector<GroupRequest> first = group.requestsFrom(1);
    if (first.size() == 1 && second.size() == 1)
    {
        bool oneSuppressed = std::count(first[0].pieces.begin(), first[0].pieces.end(), 7) +
                                 std::count(second[0].pieces.begin(), second[0].pieces.end(), 7) ==
                             1;
        CHECK(oneSuppressed);
    }
    // VK3XYZ missed the first Announce but heard the second, so it has
    // the name.
    CHECK(group.file(2, false).name == "LOG.TXT");

    CHECK(group.runUntil([&]() { return group.file(0, true).state == State::Ended; }));
    GroupFile sent = group.file(0, true);
    CHECK(sent.endReason == GroupFileEnd::Quiet);
    CHECK(sent.askers.size() == 2);
}

// A station that comes on part way through: it keeps what it hears, asks
// for the rest, including the name it never heard, and has it all.
void testLateJoiner()
{
    Scratch scratch("late");
    std::string source = scratch.write("MAP.BIN", 9000); // 40 pieces at Duet: Announces before 0 and 24
    Group group;
    group.add("AG7EW");
    Group::Node& late = group.add("K7ABC");
    late.engine.setAutoReceive(scratch.folder("late"));
    late.listening = false;

    std::string error;
    CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
    // On after the second Announce has gone.
    CHECK(group.runUntil([&]() { return group.sentOfType(0, GroupFileFrame::Announce) == 2 &&
                                        group.sentOfType(0, GroupFileFrame::Data) >= 4; }));
    late.listening = true;

    CHECK(group.runUntil([&]() { return group.file(1, false).state == State::Saved; }));
    GroupFile got = group.file(1, false);
    CHECK(readFile(got.path) == readFile(source));
    CHECK(got.name == "MAP.BIN");
    std::vector<GroupRequest> asked = group.requestsFrom(1);
    CHECK(!asked.empty());
    if (!asked.empty()) CHECK((asked[0].flags & REQUEST_NEEDS_ANNOUNCE) != 0);
    // The Grant said an Announce follows.
    std::vector<GroupGrant> grants = group.grants();
    CHECK(!grants.empty() && (grants[0].flags & GRANT_ANNOUNCE_FOLLOWS) != 0);
}

// The same file sent again: the station that kept its pieces finishes it
// from the new pass, even missing pieces it already had.
void testSendingTheSameFileAgain()
{
    Scratch scratch("again");
    std::string source = scratch.write("NET.TXT", 4000);
    Group group;
    group.add("AG7EW");
    group.add("K7ABC").engine.setAutoReceive(scratch.folder("k"));

    // The first time it loses the even pieces and can't be heard asking.
    bool firstTime = true;
    group.loss = [&](int from, int to, const std::vector<uint8_t>& frame) {
        if (!firstTime) return from == 0 && to == 1 && isData(frame) && frame[5] % 2 == 1; // the odd ones now
        if (from == 1) return true;
        return isData(frame) && frame[5] % 2 == 0;
    };
    std::string error;
    CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
    CHECK(group.runUntil([&]() { return group.file(0, true).state == State::Ended; }));
    GroupFile kept = group.file(1, false);
    CHECK(kept.state == State::Incomplete && kept.have > 0 && kept.have < kept.pieces);
    uint32_t firstId = kept.fileId;

    firstTime = false;
    CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
    CHECK(group.runUntil([&]() { return group.file(1, false).state == State::Saved; }));
    GroupFile got = group.file(1, false);
    CHECK(got.fileId == firstId);
    CHECK(readFile(got.path) == readFile(source));
}

// Cancelled mid-stream: the End goes next, and listeners delete what came.
void testCancel()
{
    Scratch scratch("cancel");
    std::string source = scratch.write("BIG.BIN", 20000);
    Group group;
    group.add("AG7EW");
    group.add("K7ABC").engine.setAutoReceive(scratch.folder("k"));

    std::string error;
    uint64_t id = group[0].engine.send(source, group.nowMs, error);
    CHECK(group.runUntil([&]() { return group.file(1, false).have >= 16; }));
    CHECK(group[0].engine.cancel(id));
    size_t before = group.sent.size();
    CHECK(group.runUntil([&]() { return group.file(0, true).state == State::Ended; }));
    CHECK(group.file(0, true).endReason == GroupFileEnd::Cancelled);
    // Only the keying already on the air, then the End twice.
    int data = 0;
    for (size_t i = before; i < group.sent.size(); i++)
    {
        if (isData(group.sent[i].frames[0])) data++;
    }
    CHECK(data <= 1);
    CHECK(group.sentOfType(0, GroupFileFrame::End) == 2);
    GroupFile got = group.file(1, false);
    CHECK(got.state == State::CancelledThere && got.have == 0);
    CHECK(fs::is_empty(scratch.dir / "k"));
}

// A listener the sender hears asking but whose resends never get through:
// repairs go on until the deadline, ten minutes at Duet, and then end.
void testDeadline()
{
    Scratch scratch("deadline");
    std::string source = scratch.write("X.BIN", 3000);
    Group group;
    group.add("AG7EW");
    group.add("K7ABC").engine.setAutoReceive(scratch.folder("k"));

    bool streamOver = false;
    group.loss = [&](int from, int, const std::vector<uint8_t>& frame) {
        if (from != 0) return false;
        if (isType(frame, GroupFileFrame::Window)) streamOver = true;
        return isData(frame, 5) || (streamOver && isData(frame));
    };
    std::string error;
    CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
    uint64_t started = group.nowMs;
    CHECK(group.runUntil([&]() { return group.file(0, true).state == State::Ended; }, 30 * 60 * 1000));
    CHECK(group.file(0, true).endReason == GroupFileEnd::Deadline);
    CHECK(group.nowMs - started >= GroupFileEngine::MIN_SERVICE_MS);
    CHECK(group.nowMs - started < GroupFileEngine::MIN_SERVICE_MS + 5 * 60 * 1000);
    CHECK(group.grants().size() >= 5);
    GroupFile got = group.file(1, false);
    CHECK(got.state == State::Incomplete && got.have == got.pieces - 1);
}

// The console's tempo changes mid-transfer: the keyings follow it, the
// pieces stay the size they were announced at.
void testTempoFollowsTheConsole()
{
    Scratch scratch("tempo");
    std::string source = scratch.write("T.BIN", 4000); // 18 pieces of 228
    Group group;
    group.add("AG7EW", 5);
    group.add("K7ABC").engine.setAutoReceive(scratch.folder("k"));

    std::string error;
    CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
    CHECK(group.runUntil([&]() { return group.sentOfType(0, GroupFileFrame::Data) == 1; }));
    group[0].engine.setGear(4); // Presto: OFDM still, fewer per keying
    CHECK(group.runUntil([&]() { return group.file(1, false).state == State::Saved; }));
    CHECK(readFile(group.file(1, false).path) == readFile(source));
    CHECK(group[0].gears.front() == 5 && group[0].gears.back() == 4);
    for (const Air& air : group.sent)
    {
        for (const auto& f : air.frames)
        {
            GroupData d;
            if (decodeGroupData(f.data(), f.size(), d) && d.index < 17) CHECK(d.bytes.size() == 228);
        }
    }

    // And to Adagio: one piece a keying.
    Scratch scratch2("tempo2");
    std::string source2 = scratch2.write("U.BIN", 1500);
    Group slow;
    slow.add("AG7EW", 5);
    slow.add("K7ABC").engine.setAutoReceive(scratch2.folder("k"));
    CHECK(slow[0].engine.send(source2, slow.nowMs, error) != 0);
    slow[0].engine.setGear(1);
    CHECK(slow.runUntil([&]() { return slow.file(1, false).state == State::Saved; }));
    for (const Air& air : slow.sent) CHECK(air.frames.size() == 1);
    CHECK(slow.sentOfType(0, GroupFileFrame::Data) == 7);
}

// A listener that hasn't said Receive keeps the pieces but never keys; once
// the operator says Receive it asks, and saves where it was told.
void testReceiveOnlyWhenTold()
{
    Scratch scratch("told");
    std::string source = scratch.write("DOC.TXT", 3000);
    Group group;
    group.add("AG7EW");
    group.add("K7ABC");
    group.loss = [&](int from, int, const std::vector<uint8_t>& frame) { return from == 0 && isData(frame, 2); };

    std::string error;
    CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
    CHECK(group.runUntil([&]() { return group.sentOfType(0, GroupFileFrame::Window) >= 1; }));
    CHECK(group.runUntil([&]() { return group.sentOfType(0, GroupFileFrame::Window) >= 2; }));
    GroupFile heard = group.file(1, false);
    CHECK(heard.state == State::Heard && heard.name == "DOC.TXT" && heard.have == heard.pieces - 1);
    CHECK(group[1].keyings == 0);

    group.loss = nullptr;
    std::string saveAs = scratch.path("mine.txt");
    CHECK(group[1].engine.receive(heard.id, saveAs, group.nowMs, error));
    CHECK(group.runUntil([&]() { return group.file(1, false).state == State::Saved; }));
    CHECK(group.file(1, false).path == saveAs && readFile(saveAs) == readFile(source));
    CHECK(group[1].keyings >= 1);

    // Ignored: nothing kept, and nothing asked.
    Group other;
    other.add("AG7EW");
    other.add("K7ABC");
    other.loss = [&](int from, int, const std::vector<uint8_t>& frame) { return from == 0 && isData(frame, 2); };
    CHECK(other[0].engine.send(source, other.nowMs, error) != 0);
    CHECK(other.runUntil([&]() { return other.file(1, false).have > 3; }));
    CHECK(other[1].engine.ignore(other.file(1, false).id));
    CHECK(other.runUntil([&]() { return other.file(0, true).state == State::Ended; }));
    CHECK(other.file(1, false).state == State::Ignored && other.file(1, false).have == 0);
    CHECK(other[1].keyings == 0);
}

// A piece that passed the modem's checks wrongly: the hash catches it, all
// of it is asked for again, and it is saved the second time; wrong twice,
// nothing is saved.
void testHashFailure()
{
    for (int times : {1, 2})
    {
        Scratch scratch("hash" + std::to_string(times));
        std::string source = scratch.write("H.BIN", 2000);
        Group group;
        group.add("AG7EW");
        group.add("K7ABC").engine.setAutoReceive(scratch.folder("k"));
        int corrupted = 0;
        std::vector<uint8_t> bad;
        group.loss = [&](int from, int to, const std::vector<uint8_t>& frame) {
            if (from != 0 || !isData(frame, 4) || corrupted >= times) return false;
            corrupted++;
            bad = frame;
            bad.back() ^= 0x55;
            group[(size_t)to].engine.onFrame(bad.data(), bad.size(), group.nowMs);
            return true;
        };
        std::string error;
        CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
        CHECK(group.runUntil([&]() { return !group.file(1, false).live() || group.file(0, true).state == State::Ended; }));
        group.runUntil([&]() { return group.file(0, true).state == State::Ended; });
        GroupFile got = group.file(1, false);
        if (times == 1)
        {
            CHECK(got.state == State::Saved && readFile(got.path) == readFile(source));
            std::vector<GroupRequest> asked = group.requestsFrom(1);
            CHECK(!asked.empty() && (asked[0].flags & REQUEST_RESTART) != 0);
        }
        else
        {
            CHECK(got.state == State::FailedVerification);
            CHECK(fs::is_empty(scratch.dir / "k"));
        }
    }
}

// The end of the stream lost, and every Window after it: the listener
// asks unbidden, and the sender takes it before it gives up.
void testLostWindows()
{
    Scratch scratch("lostwin");
    std::string source = scratch.write("W.BIN", 2000);
    Group group;
    group.add("AG7EW");
    group.add("K7ABC").engine.setAutoReceive(scratch.folder("k"));
    int windows = 0;
    group.loss = [&](int from, int, const std::vector<uint8_t>& frame) {
        if (from != 0) return false;
        if (isType(frame, GroupFileFrame::Window)) return ++windows <= 3;
        return isData(frame, 1) && windows == 0;
    };
    std::string error;
    CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
    CHECK(group.runUntil([&]() { return group.file(1, false).state == State::Saved; }));
    std::vector<GroupRequest> asked = group.requestsFrom(1);
    CHECK(!asked.empty() && asked[0].round == ROUND_LATE);
    CHECK(readFile(group.file(1, false).path) == readFile(source));
}

// Two stations asking every round: the first in line takes turns.
void testTurnsGoRound()
{
    Scratch scratch("turns");
    std::string source = scratch.write("R.BIN", 6000);
    Group group;
    group.add("AG7EW");
    group.add("K7ABC").engine.setAutoReceive(scratch.folder("a"));
    group.add("VK3XYZ").engine.setAutoReceive(scratch.folder("b"));
    // Each loses its own pieces the first three times they are sent.
    std::map<std::pair<int, int>, int> sentTo;
    group.loss = [&](int from, int to, const std::vector<uint8_t>& frame) {
        GroupData d;
        if (from != 0 || !decodeGroupData(frame.data(), frame.size(), d)) return false;
        bool mine = (to == 1 && (d.index == 2 || d.index == 9)) || (to == 2 && (d.index == 5 || d.index == 11));
        return mine && sentTo[{to, d.index}]++ < 3;
    };
    std::string error;
    CHECK(group[0].engine.send(source, group.nowMs, error) != 0);
    CHECK(group.runUntil([&]() {
        return group.file(1, false).state == State::Saved && group.file(2, false).state == State::Saved;
    }));
    std::vector<GroupGrant> grants = group.grants();
    CHECK(grants.size() >= 3);
    for (size_t i = 1; i < grants.size(); i++) CHECK(grants[i].call != grants[i - 1].call);
}

// Stop serving during repairs: the End says so, and a listener still
// missing pieces keeps them.
void testStopServing()
{
    Scratch scratch("stop");
    std::string source = scratch.write("S.BIN", 2000);
    Group group;
    group.add("AG7EW");
    group.add("K7ABC").engine.setAutoReceive(scratch.folder("k"));
    group.loss = [&](int from, int, const std::vector<uint8_t>& frame) { return from == 0 && isData(frame, 3); };
    std::string error;
    uint64_t id = group[0].engine.send(source, group.nowMs, error);
    CHECK(group.runUntil([&]() { return group.file(0, true).phase == GroupFile::Phase::WindowOpen; }));
    CHECK(group[0].engine.stopServing(id));
    CHECK(group.runUntil([&]() { return group.file(0, true).state == State::Ended; }));
    CHECK(group.file(0, true).endReason == GroupFileEnd::Stopped);
    CHECK(group.sentOfType(0, GroupFileFrame::Grant) == 0);
    GroupFile got = group.file(1, false);
    CHECK(got.state == State::Incomplete && got.have == got.pieces - 1);
}

// Too big, or too long on the air at this tempo: refused.
void testLimits()
{
    Scratch scratch("limits");
    GroupFileEngine engine;
    engine.setMyCallsign("AG7EW");
    engine.setGear(5);
    std::string error;
    CHECK(engine.send(scratch.write("big.bin", 64 * 1024 + 1), 0, error) == 0 && !error.empty());
    CHECK(engine.send(scratch.write("empty.bin", 0), 0, error) == 0);
    engine.setGear(1);
    CHECK(engine.estimate(10000, 1).airSeconds > GroupFileEngine::REFUSE_AIR_SECONDS);
    CHECK(engine.send(scratch.write("slow.bin", 10000), 0, error) == 0 && !error.empty());
    CHECK(engine.estimate(3000, 1).airSeconds > GroupFileEngine::WARN_AIR_SECONDS);
    engine.setGear(5);
    CHECK(engine.send(scratch.write("ok.bin", 64 * 1024), 0, error) != 0);
    CHECK(engine.send(scratch.write("two.bin", 100), 0, error) == 0); // one at a time

    // Not allowed to send: the file of ours ends, without a word.
    engine.setInhibited(true);
    CHECK(engine.files(0)[0].state == State::Ended && engine.files(0)[0].endReason == GroupFileEnd::Failed);
    CHECK(engine.dueGear(100000) == 0);
}

} // namespace

int main()
{
    testFramesRoundTrip();
    testChatNeverSeesThem();
    testPieceSets();
    testGeometry();
    testCleanChannel();
    testTwoLossyListeners();
    testLateJoiner();
    testSendingTheSameFileAgain();
    testCancel();
    testDeadline();
    testTempoFollowsTheConsole();
    testReceiveOnlyWhenTold();
    testHashFailure();
    testLostWindows();
    testTurnsGoRound();
    testStopServing();
    testLimits();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all Data2G group file checks passed\n");
    return 0;
}
