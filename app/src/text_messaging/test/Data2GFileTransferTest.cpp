//=========================================================================
// Name:            Data2GFileTransferTest.cpp
// Purpose:         The file transfer engine on its own: two engines whose
//                  records are carried between them by hand, standing in
//                  for a session, with the files in a scratch folder.
//=========================================================================

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "../Data2GFileTransfer.h"
#include "../Data2GLink.h"

using namespace TextMessaging;
using namespace TextMessaging::Data2G;
using State = FileTransfer::State;

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

// A folder of its own for each test, removed afterwards.
struct Scratch
{
    explicit Scratch(const std::string& name)
    {
        std::random_device random;
        dir = fs::temp_directory_path() / ("glissando-file-test-" + name + "-" + std::to_string(random()));
        fs::create_directories(dir);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string path(const std::string& name) const { return utf8FromPath(dir / pathFromUtf8(name)); }

    std::string write(const std::string& name, size_t size)
    {
        std::string content(size, '\0');
        for (size_t i = 0; i < size; i++) content[i] = (char)((i * 7 + i / 251) & 0xFF);
        std::ofstream out(dir / pathFromUtf8(name), std::ios::binary);
        out.write(content.data(), (std::streamsize)content.size());
        return path(name);
    }

    fs::path dir;
};

std::string readFile(const std::string& path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool exists(const std::string& path)
{
    std::error_code ec;
    return fs::exists(pathFromUtf8(path), ec);
}

FileTransfer transferOf(const FileTransferEngine& engine, uint64_t id)
{
    for (const FileTransfer& t : engine.transfers())
    {
        if (t.id == id) return t;
    }
    return FileTransfer();
}

// The one transfer the engine has from (or to) the far end, newest.
FileTransfer last(const FileTransferEngine& engine, bool outgoing)
{
    FileTransfer found;
    for (const FileTransfer& t : engine.transfers())
    {
        if (t.outgoing == outgoing) found = t;
    }
    return found;
}

// Two stations in a session. What one writes reaches the other through a
// stream decoder, as it would through data2g-host, and the far end's
// modem acknowledges it at once unless told to hold.
struct Link
{
    FileTransferEngine a; // W1AW
    FileTransferEngine b; // VK3ABC
    StreamDecoder toA, toB;
    uint64_t writtenByA = 0, writtenByB = 0;
    uint64_t nowMs = 1000;
    bool holdPieces = false;   // only records, no pieces
    int pieces = 0;

    // One turn each: records, then a piece; then everything is delivered.
    void turn()
    {
        std::vector<uint8_t> fromA = a.takeRecords("VK3ABC", nowMs);
        if (!holdPieces && a.takePiece("VK3ABC", writtenByA + fromA.size(), fromA)) pieces++;
        writtenByA += fromA.size();
        std::vector<uint8_t> fromB = b.takeRecords("W1AW", nowMs);
        b.takePiece("W1AW", writtenByB + fromB.size(), fromB);
        writtenByB += fromB.size();

        deliver(fromA, toB, b, "W1AW");
        deliver(fromB, toA, a, "VK3ABC");
        a.acknowledged("VK3ABC", writtenByA);
        b.acknowledged("W1AW", writtenByB);
        a.tick(nowMs);
        b.tick(nowMs);
    }

    void deliver(const std::vector<uint8_t>& bytes, StreamDecoder& decoder, FileTransferEngine& to,
                 const std::string& from)
    {
        if (bytes.empty()) return;
        std::vector<std::vector<uint8_t>> frames;
        std::vector<FileRecord> records;
        // In uneven reads, as a socket would hand them over.
        for (size_t at = 0; at < bytes.size(); at += 1000)
        {
            size_t n = std::min<size_t>(1000, bytes.size() - at);
            CHECK(decoder.feed(bytes.data() + at, (int)n, frames, records));
        }
        CHECK(frames.empty());
        for (const FileRecord& record : records) to.onRecord(from, record, nowMs);
    }

    void turns(int n)
    {
        for (int i = 0; i < n; i++) turn();
    }
};

void testSafeNames()
{
    CHECK(safeFileName("FRED.TXT") == "FRED.TXT");
    CHECK(safeFileName("../../x") == "x");
    CHECK(safeFileName("..\\..\\windows\\system32\\evil.dll") == "evil.dll");
    CHECK(safeFileName("/etc/passwd") == "passwd");
    CHECK(safeFileName("") == "received-file");
    CHECK(safeFileName("..") == "received-file");
    CHECK(safeFileName("dir/") == "received-file");
    CHECK(safeFileName("a\x01" "b\x1F" "c\x7F.txt") == "abc.txt");
    CHECK(safeFileName("what?<is>:this|\"*.txt") == "whatisthis.txt");
    CHECK(safeFileName(".bashrc") == "bashrc");
    CHECK(safeFileName("trailing. . ") == "trailing");
    CHECK(safeFileName("CON") == "received-file");
    CHECK(safeFileName("con.txt") == "received-file.txt");
    CHECK(safeFileName("Lpt1.tar.gz") == "received-file.tar.gz");
    CHECK(safeFileName("COM10.txt") == "COM10.txt");
    CHECK(safeFileName("COM\xC2\xB9.txt") == "received-file.txt");    // COM superscript one
    CHECK(safeFileName("lpt\xC2\xB2") == "received-file");            // superscript two
    CHECK(safeFileName("COM\xC2\xB3.log") == "received-file.log");    // superscript three
    CHECK(safeFileName("COM\xC2\xB4.txt") == "COM\xC2\xB4.txt");      // an acute accent: not a port
    CHECK(safeFileName("console.txt") == "console.txt");
    CHECK(safeFileName("caf\xC3\xA9.txt") == "caf\xC3\xA9.txt");     // UTF-8 kept
    CHECK(safeFileName("bad\xFF\xC3.txt") == "bad.txt");             // not UTF-8: dropped
    CHECK(safeFileName("c1\xC2\x85" "ctl") == "c1ctl");              // NEL, a C1 control
    // Direction controls, which could show another extension than the real one
    CHECK(safeFileName("invoice\xE2\x80\xAE" "fdp.exe") == "invoicefdp.exe");      // U+202E
    CHECK(safeFileName("a\xE2\x80\x8E" "b\xE2\x80\x8F" "c\xE2\x80\xAA.txt") == "abc.txt");
    CHECK(safeFileName("x\xE2\x81\xA6" "y\xE2\x81\xA9.txt") == "xy.txt");       // isolates
    CHECK(safeFileName("it\xE2\x80\x99s.txt") == "it\xE2\x80\x99s.txt");        // a quote is kept
    std::string longName(300, 'a');
    CHECK(safeFileName(longName).size() == (size_t)FILE_NAME_BYTES);
    // Cut where a character ends, not in the middle of one.
    std::string accents;
    for (int i = 0; i < 150; i++) accents += "\xC3\xA9";
    std::string cut = safeFileName(accents);
    CHECK(cut.size() == (size_t)FILE_NAME_BYTES && cut.substr(cut.size() - 2) == "\xC3\xA9");
}

void testAFileEndToEnd()
{
    Scratch scratch("end-to-end");
    Link link;
    std::string source = scratch.write("FRED.TXT", 7000);
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", source, link.nowMs, error);
    CHECK(id != 0);
    CHECK(transferOf(link.a, id).state == State::Waiting);
    CHECK(link.a.busyWith("VK3ABC"));
    std::string peer;
    uint64_t since = 0;
    CHECK(link.a.wantsSession(peer, since) && peer == "VK3ABC" && since == 1000);

    link.turn();
    CHECK(transferOf(link.a, id).state == State::Offered);
    FileTransfer offered = last(link.b, false);
    CHECK(offered.state == State::Asking && offered.name == "FRED.TXT" && offered.size == 7000 &&
          offered.peer == "W1AW");
    CHECK(link.b.busyWith("W1AW"));
    CHECK(!link.a.wantsSession(peer, since));

    std::string saveAs = scratch.path("saved.txt");
    CHECK(link.b.accept(offered.id, saveAs, error));
    CHECK(exists(saveAs + ".part"));
    link.turn(); // the Accept reaches A
    CHECK(transferOf(link.a, id).state == State::Sending && transferOf(link.a, id).done == 0);
    link.turn(); // its first piece goes
    CHECK(transferOf(link.a, id).done == 4096);
    CHECK(last(link.b, false).done == 4096 && last(link.b, false).state == State::Receiving);
    link.turns(2);
    CHECK(transferOf(link.a, id).state == State::Delivered && transferOf(link.a, id).done == 7000);
    CHECK(last(link.b, false).state == State::Saved && last(link.b, false).path == saveAs);
    CHECK(readFile(saveAs) == readFile(source));
    CHECK(!exists(saveAs + ".part"));
    CHECK(link.pieces == 2);
    CHECK(!link.a.busyWith("VK3ABC") && !link.b.busyWith("W1AW"));
}

void testAnEmptyFile()
{
    Scratch scratch("empty");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("empty.bin", 0), link.nowMs, error);
    link.turn();
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("got.bin"), error));
    link.turns(2);
    CHECK(transferOf(link.a, id).state == State::Delivered);
    CHECK(last(link.b, false).state == State::Saved && exists(scratch.path("got.bin")));
    CHECK(link.pieces == 0);
}

void testDeclined()
{
    Scratch scratch("decline");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 10), link.nowMs, error);
    link.turn();
    CHECK(link.b.decline(last(link.b, false).id));
    CHECK(last(link.b, false).state == State::Declined);
    link.turn();
    CHECK(transferOf(link.a, id).state == State::Declined);
    CHECK(!link.b.accept(last(link.b, false).id, scratch.path("late.bin"), error));
}

void testTheSenderCancels()
{
    Scratch scratch("sender-cancels");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("big.bin", 20000), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("big.bin.got");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(3);
    CHECK(last(link.b, false).state == State::Receiving && last(link.b, false).done == 8192);

    // A piece already written when the sender cancels arrives after it in
    // the stream would be fine; here one arrives after the Cancel is acted on.
    std::vector<uint8_t> late;
    CHECK(link.a.takePiece("VK3ABC", link.writtenByA, late));
    CHECK(link.a.cancel(id));
    CHECK(transferOf(link.a, id).state == State::Cancelled);
    link.turn();
    CHECK(last(link.b, false).state == State::CancelledThere);
    CHECK(!exists(saveAs + ".part") && !exists(saveAs));
    link.deliver(late, link.toB, link.b, "W1AW"); // dropped
    CHECK(last(link.b, false).state == State::CancelledThere && last(link.b, false).done == 8192);
    CHECK(!exists(saveAs + ".part"));
    std::vector<uint8_t> none;
    CHECK(!link.a.takePiece("VK3ABC", link.writtenByA, none) && none.empty());
}

void testTheReceiverCancels()
{
    Scratch scratch("receiver-cancels");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("big.bin", 20000), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("got.bin");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(2);
    CHECK(link.b.cancel(last(link.b, false).id));
    CHECK(last(link.b, false).state == State::Cancelled);
    CHECK(!exists(saveAs + ".part"));
    link.turn();
    CHECK(transferOf(link.a, id).state == State::CancelledThere);
}

void testAnOfferExpiresOnBothSides()
{
    Scratch scratch("expiry");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 10), link.nowMs, error);
    link.turn();
    CHECK(last(link.b, false).state == State::Asking);
    link.nowMs += FileTransferEngine::OFFER_EXPIRY_MS - 1;
    link.turn();
    CHECK(transferOf(link.a, id).state == State::Offered);
    link.nowMs += 1;
    link.turn(); // A expires and says so
    CHECK(transferOf(link.a, id).state == State::Expired);
    link.turn();
    CHECK(last(link.b, false).state == State::Expired);
    CHECK(!link.b.accept(last(link.b, false).id, scratch.path("late.bin"), error));
    CHECK(error == "The offer has expired.");

    // The receiver's own clock runs out too, without a word from the sender.
    Link quiet;
    uint64_t second = quiet.a.offer("VK3ABC", scratch.write("y.bin", 10), quiet.nowMs, error);
    quiet.turn();
    quiet.b.tick(quiet.nowMs + FileTransferEngine::OFFER_EXPIRY_MS);
    CHECK(last(quiet.b, false).state == State::Expired);
    quiet.turn(); // its Cancel reaches the sender first
    CHECK(transferOf(quiet.a, second).state == State::Expired);
}

void testALostSession()
{
    Scratch scratch("lost");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("big.bin", 20000), link.nowMs, error);
    uint64_t next = link.a.offer("VK3ABC", scratch.write("next.bin", 5), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("got.bin");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(2);
    CHECK(exists(saveAs + ".part"));

    link.a.sessionEnded("VK3ABC", link.nowMs + 5, true);
    link.b.sessionEnded("W1AW", link.nowMs + 5, true);
    CHECK(transferOf(link.a, id).state == State::Failed);
    CHECK(last(link.b, false).state == State::Failed);
    CHECK(!exists(saveAs + ".part"));

    // The next file waits for the next session, from now.
    std::string peer;
    uint64_t since = 0;
    CHECK(transferOf(link.a, next).state == State::Waiting);
    CHECK(link.a.wantsSession(peer, since) && since == link.nowMs + 5);
    link.a.noSession("VK3ABC", "No answer.");
    CHECK(transferOf(link.a, next).state == State::Failed && transferOf(link.a, next).error == "No answer.");
}

void testAnOlderGlissandoCantTakeFiles()
{
    Scratch scratch("older");
    FileTransferEngine a;
    std::string error;
    uint64_t id = a.offer("VK3ABC", scratch.write("x.bin", 10), 1000, error);
    CHECK(!a.takeRecords("VK3ABC", 1000).empty());
    a.sessionEnded("VK3ABC", 2000, true); // it closed the session on the offer
    CHECK(transferOf(a, id).state == State::NotSupported);

    // A session that ended on this side (data2g-host's port lost, say)
    // says nothing of the far end's Glissando.
    uint64_t second = a.offer("VK3ABC", scratch.write("y.bin", 10), 3000, error);
    CHECK(!a.takeRecords("VK3ABC", 3000).empty());
    a.sessionEnded("VK3ABC", 4000, false);
    CHECK(transferOf(a, second).state == State::Failed && !transferOf(a, second).error.empty());
}

// The sender cancels once every piece is written, but the receiver had
// saved it already and ignores the Cancel: its Saved, crossing the
// Cancel, makes it delivered on both sides.
void testACancelCrossingSaved()
{
    Scratch scratch("cancel-crossing-saved");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 5000), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("x.got");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turn(); // the Accept reaches A

    // Both pieces written; B has them and has saved it.
    std::vector<uint8_t> pieces;
    CHECK(link.a.takePiece("VK3ABC", link.writtenByA, pieces));
    CHECK(link.a.takePiece("VK3ABC", link.writtenByA + pieces.size(), pieces));
    link.writtenByA += pieces.size();
    link.deliver(pieces, link.toB, link.b, "W1AW");
    CHECK(last(link.b, false).state == State::Saved);
    CHECK(transferOf(link.a, id).state == State::Sending);

    CHECK(link.a.cancel(id));
    CHECK(transferOf(link.a, id).state == State::Cancelled);
    link.turn(); // A's Cancel and B's Saved cross
    CHECK(last(link.b, false).state == State::Saved && readFile(saveAs).size() == 5000);
    CHECK(transferOf(link.a, id).state == State::Delivered && transferOf(link.a, id).done == 5000);

    // Cancelled with only part of it written, a Saved can't come, and
    // one that did is not taken.
    uint64_t second = link.a.offer("VK3ABC", scratch.write("y.bin", 5000), link.nowMs, error);
    link.turn();
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("y.got"), error));
    link.turn();
    std::vector<uint8_t> one;
    CHECK(link.a.takePiece("VK3ABC", link.writtenByA, one));
    CHECK(link.a.cancel(second));
    FileRecord saved;
    saved.type = (uint8_t)FileRecordType::Saved;
    saved.body = {2};
    link.a.onRecord("VK3ABC", saved, link.nowMs);
    CHECK(transferOf(link.a, second).state == State::Cancelled);
}

void testOneFileAtATime()
{
    Scratch scratch("queue");
    Link link;
    std::string error;
    uint64_t first = link.a.offer("VK3ABC", scratch.write("one.bin", 5000), link.nowMs, error);
    uint64_t second = link.a.offer("VK3ABC", scratch.write("two.bin", 100), link.nowMs, error);
    link.turn();
    CHECK(transferOf(link.a, first).state == State::Offered);
    CHECK(transferOf(link.a, second).state == State::Waiting);
    CHECK(link.b.transfers().size() == 1);
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("one.got"), error));
    link.turns(4);
    CHECK(transferOf(link.a, first).state == State::Delivered);
    link.turn();
    CHECK(transferOf(link.a, second).state == State::Offered);
    CHECK(last(link.b, false).name == "two.bin");
}

void testAutoAcceptNumbersAClash()
{
    Scratch scratch("auto");
    Link link;
    fs::create_directories(scratch.dir / "in");
    std::string folder = utf8FromPath(scratch.dir / "in");
    { std::ofstream(scratch.dir / "in" / "FRED.TXT") << "already here"; }
    link.b.setAutoAccept(folder, {"w1aw", "K1ABC"});
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("FRED.TXT", 3000), link.nowMs, error);
    link.turns(4);
    FileTransfer got = last(link.b, false);
    CHECK(got.autoAccepted && got.state == State::Saved);
    CHECK(got.path == utf8FromPath(scratch.dir / "in" / "FRED (2).TXT"));
    CHECK(readFile(got.path) == readFile(scratch.path("FRED.TXT")));
    CHECK(transferOf(link.a, id).state == State::Delivered);

    // Not from a station on the list: asked.
    link.b.setAutoAccept(folder, {"K1ABC"});
    link.a.offer("VK3ABC", scratch.write("other.bin", 10), link.nowMs, error);
    link.turn();
    CHECK(last(link.b, false).state == State::Asking && !last(link.b, false).autoAccepted);
}

FileRecord offerRecord(uint8_t number, uint8_t size, const std::string& name)
{
    FileRecord offer;
    offer.type = (uint8_t)FileRecordType::Offer;
    offer.body = {number, 0, 0, 0, size};
    offer.body.insert(offer.body.end(), name.begin(), name.end());
    return offer;
}

FileRecord dataRecord(uint8_t number, const std::string& bytes)
{
    FileRecord data;
    data.type = (uint8_t)FileRecordType::Data;
    data.body = {number};
    data.body.insert(data.body.end(), bytes.begin(), bytes.end());
    return data;
}

// A file of the operator's that happens to have the part file's name is
// neither written over nor deleted.
void testAPartFileOfTheOperatorsIsLeftAlone()
{
    Scratch scratch("own-part");
    { std::ofstream(scratch.dir / "notes.part") << "mine"; }
    FileTransferEngine b;
    std::string error;
    b.onRecord("W1AW", offerRecord(1, 3, "notes"), 1000);
    CHECK(b.accept(last(b, false).id, scratch.path("notes"), error));
    CHECK(readFile(scratch.path("notes.part")) == "mine");
    CHECK(b.cancel(last(b, false).id));
    CHECK(readFile(scratch.path("notes.part")) == "mine");
    CHECK(!exists(scratch.path("notes.2.part")));

    b.onRecord("W1AW", offerRecord(2, 3, "notes"), 1000);
    CHECK(b.accept(last(b, false).id, scratch.path("notes"), error));
    b.onRecord("W1AW", dataRecord(2, "abc"), 1000);
    CHECK(last(b, false).state == State::Saved);
    CHECK(readFile(scratch.path("notes")) == "abc");
    CHECK(readFile(scratch.path("notes.part")) == "mine");
}

// Two files can't be saved under one name at once: the second is refused
// and its offer left open.
void testTwoFilesCantBeSavedAsOne()
{
    Scratch scratch("same-name");
    FileTransferEngine b;
    std::string error;
    b.onRecord("W1AW", offerRecord(1, 3, "log.adi"), 1000);
    uint64_t first = last(b, false).id;
    b.onRecord("K1ABC", offerRecord(1, 3, "log.adi"), 1000);
    uint64_t second = last(b, false).id;
    CHECK(b.accept(first, scratch.path("log.adi"), error));
    CHECK(!b.accept(second, scratch.path("log.adi"), error) && !error.empty());
    CHECK(transferOf(b, second).state == State::Asking);
    CHECK(b.accept(second, scratch.path("log2.adi"), error));
    b.onRecord("K1ABC", dataRecord(1, "two"), 1000);
    b.onRecord("W1AW", dataRecord(1, "one"), 1000);
    CHECK(readFile(scratch.path("log.adi")) == "one" && readFile(scratch.path("log2.adi")) == "two");
}

// With every numbered name taken, a file from the auto-accept list is
// asked about rather than written over one.
void testAutoAcceptNeverOverwrites()
{
    Scratch scratch("auto-full");
    { std::ofstream(scratch.dir / "beacon.txt") << "first"; }
    for (int n = 2; n < 1000; n++)
    {
        std::ofstream(scratch.dir / ("beacon (" + std::to_string(n) + ").txt"));
    }
    FileTransferEngine b;
    b.setAutoAccept(utf8FromPath(scratch.dir), {"W1AW"});
    b.onRecord("W1AW", offerRecord(1, 3, "beacon.txt"), 1000);
    FileTransfer got = last(b, false);
    CHECK(got.state == State::Asking && !got.autoAccepted && !got.error.empty());
    CHECK(readFile(scratch.path("beacon.txt")) == "first");
    CHECK(!exists(scratch.path("beacon.txt.part")));
}

// A file that turns up under the auto-accepted name while the transfer
// runs is not replaced: the file gets the next free name. One the
// operator chose in the save dialog is replaced, as they agreed to.
void testAutoAcceptDoesntReplaceAFileMadeMeanwhile()
{
    Scratch scratch("auto-meanwhile");
    FileTransferEngine b;
    b.setAutoAccept(utf8FromPath(scratch.dir), {"W1AW"});
    b.onRecord("W1AW", offerRecord(1, 3, "notes.txt"), 1000);
    CHECK(last(b, false).state == State::Receiving && last(b, false).path == scratch.path("notes.txt"));
    { std::ofstream(scratch.dir / "notes.txt") << "the operator's"; }
    b.onRecord("W1AW", dataRecord(1, "abc"), 1000);
    FileTransfer got = last(b, false);
    CHECK(got.state == State::Saved && got.path == scratch.path("notes (2).txt"));
    CHECK(readFile(scratch.path("notes.txt")) == "the operator's");
    CHECK(readFile(got.path) == "abc");

    // A file named for another's part file, finishing first, leaves that
    // part file alone.
    b.onRecord("W1AW", offerRecord(2, 3, "log.txt.part"), 1000);
    b.onRecord("W1AW", offerRecord(4, 3, "log.txt"), 1000);
    CHECK(last(b, false).path == scratch.path("log.txt") && exists(scratch.path("log.txt.part")));
    b.onRecord("W1AW", dataRecord(2, "one"), 1000);
    b.onRecord("W1AW", dataRecord(4, "two"), 1000);
    CHECK(readFile(scratch.path("log.txt")) == "two");
    CHECK(readFile(scratch.path("log.txt (2).part")) == "one");

    b.setAutoAccept("", {});
    { std::ofstream(scratch.dir / "chosen.txt") << "old"; }
    b.onRecord("W1AW", offerRecord(3, 3, "chosen.txt"), 1000);
    std::string error;
    CHECK(b.accept(last(b, false).id, scratch.path("chosen.txt"), error));
    b.onRecord("W1AW", dataRecord(3, "new"), 1000);
    CHECK(last(b, false).state == State::Saved && readFile(scratch.path("chosen.txt")) == "new");
}

void testAHostileNameIsSavedSafely()
{
    Scratch scratch("hostile");
    FileTransferEngine b;
    b.setAutoAccept(utf8FromPath(scratch.dir), {"W1AW"});
    std::string name = "../../evil\x01.sh";
    std::vector<uint8_t> body = {7, 0, 0, 0, 3};
    body.insert(body.end(), name.begin(), name.end());
    FileRecord offer;
    offer.type = (uint8_t)FileRecordType::Offer;
    offer.body = body;
    b.onRecord("W1AW", offer, 1000);
    FileRecord data;
    data.type = (uint8_t)FileRecordType::Data;
    data.body = {7, 'a', 'b', 'c'};
    b.onRecord("W1AW", data, 1000);
    FileTransfer got = last(b, false);
    CHECK(got.name == "evil.sh");
    CHECK(got.state == State::Saved && got.path == utf8FromPath(scratch.dir / "evil.sh"));
    CHECK(readFile(got.path) == "abc");

    // Nothing was written outside the folder, and nothing runs it.
    CHECK(!exists(utf8FromPath(scratch.dir.parent_path().parent_path() / "evil.sh")));
}

void testTheReceiverCantWrite()
{
    Scratch scratch("cant-write");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 10), link.nowMs, error);
    link.turn();
    CHECK(!link.b.accept(last(link.b, false).id, scratch.path("no/such/folder/x.bin"), error));
    CHECK(!error.empty());
    CHECK(last(link.b, false).state == State::Failed);
    link.turn();
    CHECK(transferOf(link.a, id).state == State::FailedThere);
}

void testMoreThanOfferedIsRefused()
{
    Scratch scratch("too-much");
    FileTransferEngine b;
    FileRecord offer;
    offer.type = (uint8_t)FileRecordType::Offer;
    offer.body = {3, 0, 0, 0, 2, 'x'};
    b.onRecord("W1AW", offer, 1000);
    std::string error;
    CHECK(b.accept(last(b, false).id, scratch.path("x"), error));
    FileRecord data;
    data.type = (uint8_t)FileRecordType::Data;
    data.body = {3, 1, 2, 3};
    b.onRecord("W1AW", data, 1000);
    CHECK(last(b, false).state == State::Failed);
    CHECK(!exists(scratch.path("x")) && !exists(scratch.path("x.part")));
    std::vector<uint8_t> out = b.takeRecords("W1AW", 1000);
    // Accept, then Cancel with the receiver's failure.
    std::vector<uint8_t> accept = fileRecordEncode(FileRecordType::Accept, {3});
    std::vector<uint8_t> cancel = fileRecordEncode(FileRecordType::Cancel, {3, (uint8_t)CancelReason::ReceiverFailed});
    accept.insert(accept.end(), cancel.begin(), cancel.end());
    CHECK(out == accept);
}

void testReleasingTheStationCancelsHere()
{
    Scratch scratch("release");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 9000), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("x.got");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turn();
    link.a.release("VK3ABC");
    link.b.release("W1AW");
    CHECK(transferOf(link.a, id).state == State::Cancelled);
    CHECK(last(link.b, false).state == State::Cancelled);
    CHECK(!exists(saveAs + ".part"));
    CHECK(link.a.takeRecords("VK3ABC", link.nowMs).empty()); // the session is ending: nothing to say
}

void testOfferRefusesWhatCantBeSent()
{
    Scratch scratch("refuse");
    FileTransferEngine a;
    std::string error;
    CHECK(a.offer("VK3ABC", scratch.path("missing.bin"), 1000, error) == 0 && !error.empty());
    error.clear();
    CHECK(a.offer("VK3ABC", utf8FromPath(scratch.dir), 1000, error) == 0 && !error.empty());
    CHECK(a.transfers().empty());
}

} // namespace

int main()
{
    testSafeNames();
    testAFileEndToEnd();
    testAnEmptyFile();
    testDeclined();
    testTheSenderCancels();
    testTheReceiverCancels();
    testAnOfferExpiresOnBothSides();
    testALostSession();
    testAnOlderGlissandoCantTakeFiles();
    testOneFileAtATime();
    testACancelCrossingSaved();
    testAutoAcceptDoesntReplaceAFileMadeMeanwhile();
    testAutoAcceptNumbersAClash();
    testAHostileNameIsSavedSafely();
    testAPartFileOfTheOperatorsIsLeftAlone();
    testTwoFilesCantBeSavedAsOne();
    testAutoAcceptNeverOverwrites();
    testTheReceiverCantWrite();
    testMoreThanOfferedIsRefused();
    testReleasingTheStationCancelsHere();
    testOfferRefusesWhatCantBeSent();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    printf("all Data2G file transfer checks passed\n");
    return 0;
}
