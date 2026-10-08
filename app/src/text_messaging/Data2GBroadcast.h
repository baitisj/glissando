//=========================================================================
// Name:            Data2GBroadcast.h
// Purpose:         Files broadcast to everybody on the GLISS group. The
//                  sender announces a file and streams it in numbered
//                  pieces; every station keeps the pieces it hears; after
//                  the stream, request windows let the stations that are
//                  missing pieces ask for them in random slots, and the
//                  sender resends them, naming one station first in line
//                  each round, until the windows go quiet or its deadline
//                  passes. Nothing acknowledges completion: a station that
//                  has all of it checks the file's SHA-256, saves it and
//                  keeps quiet.
//
//                  The frames go on the group as chat frames do, with a
//                  first byte whose top nibble is 0, which chat frames never
//                  use, so chat never sees them and older Glissandos drop
//                  them. No sockets here: the transport hands over the
//                  frames heard on the group and sends the keyings this
//                  gives it, one at a time, so all of it is tested on its
//                  own with a simulated clock and channel.
//
// Written for Glissando; no Data2G code is used (see docs/DATA2G.md).
//=========================================================================

#ifndef TEXT_MESSAGING__DATA2G_BROADCAST_H
#define TEXT_MESSAGING__DATA2G_BROADCAST_H

#include <array>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "Data2GLink.h"

namespace TextMessaging
{
namespace Data2G
{

//-------------------------------------------------------------------------
// Frames
//-------------------------------------------------------------------------

// The first byte of each frame. Its top nibble is 0, which FrameCodec
// never uses (it takes 3 to 8 and B to F, and Glissando 0.3 and older took
// 1, 2, 9 and A), so chat frames and file frames can't be confused.
enum class GroupFileFrame : uint8_t
{
    Announce = 0x01,    // a file exists: its name, size, pieces and hash
    Data = 0x02,        // one numbered piece
    Window = 0x03,      // the stream (round 0) or a resend is over: ask now
    Request = 0x04,     // a station's missing pieces
    Grant = 0x05,       // who is first in line, and the pieces that follow
    End = 0x06,         // no more repairs (reason 3: cancelled, spool deleted)
};

// Whether a frame heard on the group is a file frame rather than chat.
bool isGroupFileFrame(const uint8_t* bytes, size_t length);

constexpr uint8_t GROUP_FILE_VERSION = 1;
constexpr uint8_t ROUND_LATE = 0xFF;            // a Request outside any window
constexpr uint64_t GROUP_FILE_MAX_BYTES = 64 * 1024;
constexpr size_t GROUP_FILE_NAME_BYTES = 64;    // the longest name an Announce carries
constexpr size_t GROUP_FRAME_MAX_BYTES = 255;   // the transport's frame limit

// Request flags.
constexpr uint8_t REQUEST_NEEDS_ANNOUNCE = 0x01;   // the name was never heard
constexpr uint8_t REQUEST_TRUNCATED = 0x02;        // more missing than listed
constexpr uint8_t REQUEST_RESTART = 0x04;          // the hash failed: all of it again
// Grant flags.
constexpr uint8_t GRANT_ANNOUNCE_FOLLOWS = 0x01;

// Why an End was sent.
enum class GroupFileEnd : uint8_t
{
    Quiet = 0,          // three request windows in a row went quiet
    Deadline = 1,       // the sender's service time ran out
    Stopped = 2,        // the sending operator stopped serving repairs
    Cancelled = 3,      // the sending operator cancelled: listeners delete what they have
    Failed = 4,         // the file could not be read, or sending is not allowed
};

using Hash8 = std::array<uint8_t, 8>;

struct GroupAnnounce
{
    uint32_t fileId = 0;    // 24 bits
    uint8_t version = GROUP_FILE_VERSION;
    uint32_t size = 0;      // 24 bits
    int pieceBytes = 0;     // P: bytes of the file in every piece but the last
    int pieces = 0;         // N = ceil(size / P)
    Hash8 hash{};           // the first 8 bytes of the file's SHA-256
    int tempo = 0;          // 0 Adagio .. 4 Duet, at the time it was sent
    int airSeconds = 0;     // the first pass's estimated airtime
    std::string call;       // the sender
    std::string name;       // UTF-8, up to GROUP_FILE_NAME_BYTES
};

struct GroupData
{
    uint32_t fileId = 0;
    int index = 0;
    std::vector<uint8_t> bytes;
};

struct GroupWindow
{
    uint32_t fileId = 0;
    int round = 0;              // 0: the end of the stream
    uint32_t size = 0;
    int pieceBytes = 0;
    int pieces = 0;
    Hash8 hash{};
    int slots = 0;              // M
    int slotHalfSeconds = 0;    // each slot's length, in 0.5 s
    int serviceLeftTens = 0;    // the sender's time left, in 10 s
    std::string priorityCall;   // a station given slot 0, or empty
    int tempo = -1;             // the tempo to ask in, 0..4; -1 when not said
};

struct GroupRequest
{
    uint32_t fileId = 0;
    int round = 0;
    uint8_t flags = 0;
    int missingTotal = 0;
    std::string call;
    std::vector<int> pieces;    // ascending
};

struct GroupGrant
{
    uint32_t fileId = 0;
    int round = 0;
    uint8_t flags = 0;
    std::string call;           // first in line
    std::vector<int> pieces;    // in the order they follow
};

struct GroupEnd
{
    uint32_t fileId = 0;
    GroupFileEnd reason = GroupFileEnd::Quiet;
    int rounds = 0;
};

std::vector<uint8_t> encodeGroupAnnounce(const GroupAnnounce& frame);
std::vector<uint8_t> encodeGroupData(const GroupData& frame);
std::vector<uint8_t> encodeGroupWindow(const GroupWindow& frame);
std::vector<uint8_t> encodeGroupEnd(const GroupEnd& frame);

// A Request or Grant no longer than maxBytes: when the pieces don't all
// fit, only the lowest are listed (a Request then sets REQUEST_TRUNCATED;
// a Grant's list is what follows, so the caller trims it to the count
// listed). listedOut says how many went in.
std::vector<uint8_t> encodeGroupRequest(const GroupRequest& frame, size_t maxBytes, size_t& listedOut);
std::vector<uint8_t> encodeGroupGrant(const GroupGrant& frame, size_t maxBytes, size_t& listedOut);

// False for a frame that isn't the type asked for, or isn't whole.
bool decodeGroupAnnounce(const uint8_t* bytes, size_t length, GroupAnnounce& out);
bool decodeGroupData(const uint8_t* bytes, size_t length, GroupData& out);
bool decodeGroupWindow(const uint8_t* bytes, size_t length, GroupWindow& out);
bool decodeGroupRequest(const uint8_t* bytes, size_t length, GroupRequest& out);
bool decodeGroupGrant(const uint8_t* bytes, size_t length, GroupGrant& out);
bool decodeGroupEnd(const uint8_t* bytes, size_t length, GroupEnd& out);

// A list of pieces as a Request or Grant carries it: an encoding byte, then
// either a bitmap (0: a 2-byte first piece, then a bit for each piece from
// it, most significant first, trailing zero bytes left off) or ranges (1:
// a 2-byte first piece and a byte of count - 1 each), whichever is
// shorter. At most room bytes; the lowest pieces that fit when not all
// do. Returns how many were listed.
size_t encodePieceSet(const std::vector<int>& ascending, size_t room, std::vector<uint8_t>& out);
bool decodePieceSet(const uint8_t* bytes, size_t length, std::vector<int>& out);

// The transfer's id: the first 3 bytes of SHA-256 over the sender's call,
// the file's whole SHA-256 and its size (4 bytes, big-endian). The same
// file from the same station gets the same id, so a station that missed
// pieces finishes it from the next time it is sent.
uint32_t groupFileId(const std::string& call, const std::array<uint8_t, 32>& fileHash, uint64_t size);

//-------------------------------------------------------------------------
// Piece geometry and airtime
//-------------------------------------------------------------------------

// Data2G's CPM modes (the FSK ones) send one frame per burst in practice;
// its OFDM modes put several in one.
bool isCpmMode(const ModeInfo& mode);

// The bytes of the file each piece carries in a mode: a Data frame of a
// whole number of codewords less Data2G's 2-byte length (3 codewords at
// CPM, as many as fit 255 bytes at OFDM), less the 6-byte header.
int groupPieceBytes(const ModeInfo& mode);

// How many Data frames of frameBytes one keying carries in a mode: 1 at
// CPM; at OFDM up to 8, as many as fit a 12 s burst.
int groupPiecesPerKeying(const ModeInfo& mode, int frameBytes);

// A request slot in a mode: a one-codeword Request's airtime plus 4 s,
// rounded up to 0.5 s, in 0.5 s units.
int groupSlotHalfSeconds(const ModeInfo& mode);

// What sending a file of this size at this mode takes, for the operator to
// agree to before it goes.
struct GroupFileEstimate
{
    int pieceBytes = 0;
    int pieces = 0;
    double airSeconds = 0.0;    // the first pass: announces, pieces and the end of stream
    double wallSeconds = 0.0;   // that with the pauses between keyings
    double serviceSeconds = 0.0;  // how long repairs are served afterwards at most
};
GroupFileEstimate estimateGroupFile(uint64_t size, const ModeInfo& mode);

//-------------------------------------------------------------------------
// Transfers
//-------------------------------------------------------------------------

// One file going to or coming from the group, as the chat window shows it.
struct GroupFile
{
    enum class State
    {
        // Ours.
        Sending,            // announced; the first pass of pieces going
        Repairing,          // request windows and resends
        Ended,              // over: endReason says why

        // Theirs.
        Heard,              // pieces kept as they come; the operator has not said
        Receiving,          // the operator said Receive (or files are received automatically)
        Ignored,
        Saved,
        Incomplete,         // the sender stopped with pieces missing; they are kept a while
        FailedVerification, // all of it came twice with the wrong hash; nothing saved
        CancelledThere,     // the sender cancelled it; what had come is deleted
        Failed,             // it could not be saved
    };

    // What a transfer of ours is doing just now.
    enum class Phase
    {
        None,
        Streaming,      // the first pass
        WindowOpen,     // listening for requests
        Resending,      // a Grant and its pieces
        Waiting,        // between windows, after a quiet one
        Ending,         // the End going twice
    };

    struct Asker
    {
        std::string call;
        int missing = 0;
        uint64_t secondsAgo = 0;
    };

    uint64_t id = 0;            // this run's own, for the window to name it by
    bool outgoing = true;
    uint32_t fileId = 0;
    std::string sender;         // the sender's call; "?" until heard
    std::string name;           // empty until an Announce is heard; made safe to save under
    uint64_t size = 0;          // 0 until known
    int pieces = 0;             // 0 until known
    int have = 0;               // ours: pieces sent in the first pass; theirs: pieces held
    int tempo = 0;              // 0..4: the tempo it was announced at
    State state = State::Heard;
    Phase phase = Phase::None;
    int round = 0;
    int secondsLeftInPhase = 0; // a window still open, or the wait before the next
    int serviceSecondsLeft = -1;// the sender's time left, -1 until known
    int resending = 0;          // pieces in the round's Grant
    std::string firstInLine;    // the station the round's Grant named
    int othersAsking = 0;       // stations besides it whose pieces follow
    std::vector<Asker> askers;  // ours: every station that asked
    GroupFileEnd endReason = GroupFileEnd::Quiet;
    bool verified = false;      // theirs: all of it, with the right hash
    int comingForUs = 0;        // theirs: pieces the round's Grant brings that we lack
    int slot = -1;              // theirs: the slot we will ask in this round, -1 for none
    int slotInSeconds = 0;
    bool autoReceived = false;
    std::string path;           // where it was saved, or is to be
    std::string error;

    // Still going: being sent, or being heard or received.
    bool live() const;
};

// Every file going to or coming from the group. Not thread safe: the
// transport holds its lock around every call.
class GroupFileEngine
{
public:
    static constexpr int ANNOUNCE_EVERY = 20;           // pieces between repeated Announces
    static constexpr uint64_t YIELD_MS = 4000;          // after each file keying of ours
    static constexpr uint64_t AFTER_BUSY_MS = 2000;     // and this long after a busy channel clears
    static constexpr int FIRST_SLOTS = 4;
    static constexpr int MIN_SLOTS = 3;
    static constexpr int MAX_SLOTS_FROM_STATIONS = 8;
    static constexpr int MAX_SLOTS = 12;
    static constexpr uint64_t WINDOW_GUARD_MS = 4000;
    static constexpr uint64_t JITTER_MS = 2000;         // a Request goes this much into its slot at most
    static constexpr double RESEND_CAP_SECONDS = 600.0; // of airtime in one round
    static constexpr int QUIET_WINDOWS_TO_END = 3;
    static constexpr int TIMES_NAMED_WITHOUT_PROGRESS = 3;
    static constexpr uint64_t MIN_SERVICE_MS = 10 * 60 * 1000;
    static constexpr uint64_t MAX_SERVICE_MS = 2 * 60 * 60 * 1000;
    static constexpr double REFUSE_AIR_SECONDS = 60 * 60;
    static constexpr double WARN_AIR_SECONDS = 15 * 60;
    static constexpr uint64_t KEYING_GAP_MS = 5000;     // yield and turnaround, for estimates
    static constexpr uint64_t LOST_WINDOW_MIN_MS = 60 * 1000;
    static constexpr uint64_t LATE_REQUEST_SPREAD_MS = 30 * 1000;
    static constexpr uint64_t AFTER_SERVICE_MS = 2 * 60 * 1000;
    static constexpr uint64_t SPOOL_KEEP_MS = 24ull * 60 * 60 * 1000;
    static constexpr size_t SPOOL_TRANSFERS = 4;

    GroupFileEngine();

    GroupFileEngine(const GroupFileEngine&) = delete;
    GroupFileEngine& operator=(const GroupFileEngine&) = delete;

    void setMyCallsign(const std::string& callsign);
    // The host's MODES (empty for the known ones) and the tempo the console
    // has set (1 Adagio .. 5 Duet), which our keyings follow.
    void setModes(const std::vector<ModeInfo>& offered);
    void setGear(int gear);
    void setRandomSeed(uint32_t seed);

    // Files are received without asking into this folder; empty: they are
    // not. The caller passes one only while the preference is on and Auto
    // acknowledge is lit.
    void setAutoReceive(const std::string& folder);

    // Sending not allowed here (outside the US data segments, say): a
    // file of ours ends without a word, and nobody asks for pieces.
    void setInhibited(bool inhibited);
    // "Woah!": no file keying before this.
    void holdUntil(uint64_t nowMs, uint64_t holdMs);

    //---------------------------------------------------------------------
    // The operator's side
    //---------------------------------------------------------------------

    // Announces and sends a file at the console's tempo. 0, with the reason,
    // if it can't go (too large, too long on the air, one already going).
    uint64_t send(const std::string& path, uint64_t nowMs, std::string& error);

    // Ours: no more repairs once the keying on the air is done (End, reason
    // 2). Cancel: End reason 3 ahead of everything; listeners delete it.
    bool stopServing(uint64_t id);
    bool cancel(uint64_t id);

    // Theirs: save it at path once all of it is here (at once if it is),
    // asking for what is missing until then. Ignore: delete what came and
    // keep quiet about it.
    bool receive(uint64_t id, const std::string& path, uint64_t nowMs, std::string& error);
    bool ignore(uint64_t id);

    std::vector<GroupFile> files(uint64_t nowMs) const;
    uint64_t changes() const { return changes_; }

    GroupFileEstimate estimate(uint64_t size, int gear) const;

    //---------------------------------------------------------------------
    // The transport's side
    //---------------------------------------------------------------------

    // A frame heard on the group whose first byte's top nibble is 0.
    void onFrame(const uint8_t* bytes, size_t length, uint64_t nowMs);

    // data2g-host heard a burst from call with codewords lost: maybe a
    // Request that didn't make it.
    void onHeardLost(const std::string& call, int lost, uint64_t nowMs);

    // Runs the timers. busy: the channel is busy; held: a session holds
    // the group, which pauses the sender's deadline and windows.
    void tick(uint64_t nowMs, bool busy, bool held);

    // The tempo the next keying wants (1..5), once one is due; 0 while none
    // is. The transport sets the group's mode for it, then takes it.
    int dueGear(uint64_t nowMs) const;

    // The frames of the next keying, which the transport writes at once.
    bool takeKeying(uint64_t nowMs, std::vector<std::vector<uint8_t>>& framesOut);

    // The keying taken last has been transmitted; or data2g-host dropped it
    // or never sent it, so it goes once more, and then is given up on.
    void keyingSent(uint64_t nowMs);
    void keyingLost(uint64_t nowMs);

    // Anything of ours that wants the group soon, so a session of ours
    // should not stay open idle.
    bool wantsGroup() const;

    // The transport stopped: a file of ours ends; nothing is transmitted.
    void stopAll(const std::string& why);

private:
    enum class Step
    {
        Stream,         // announces and pieces
        Done,           // the end of stream (Window 0) next
        WindowOpen,
        Grant,          // the round's Grant (and Announce) and pieces next
        NextWindow,     // a Window next, once nextWindowAtMs comes
        End,            // the End, twice
        Over,
    };

    struct Ask
    {
        std::string call;
        std::vector<int> pieces;
        int missingTotal = 0;
        uint8_t flags = 0;
        uint64_t seq = 0;
    };

    struct Outgoing
    {
        uint64_t id = 0;
        uint32_t fileId = 0;
        std::vector<uint8_t> content;   // freed once it has ended
        uint64_t size = 0;
        Hash8 hash{};
        std::string name;
        std::string path;
        int pieceBytes = 0;
        int pieces = 0;
        int announcedTempo = 0;
        double firstPassAir = 0.0;

        Step step = Step::Stream;
        int nextPiece = 0;
        int announcedBlock = -1;    // the last 20-piece block an Announce went ahead of
        int round = 0;
        int windowSlots = FIRST_SLOTS;
        int windowSlotHalfSeconds = 0;
        uint64_t windowEndsMs = 0;
        uint64_t nextWindowAtMs = 0;
        uint64_t deadlineMs = 0;    // 0 until the end of stream
        int quiet = 0;
        bool stopAsked = false;
        GroupFileEnd endReason = GroupFileEnd::Quiet;
        int endsLeft = 0;

        std::map<std::string, Ask> asks;    // valid Requests for the next election
        uint64_t askSeq = 0;
        std::set<std::string> lostHints;    // heard with codewords lost and no Request
        std::map<std::string, int> hinted;  // hints each station has given
        std::string priority;               // given slot 0 next window
        int lastStations = 0;
        std::string lastNamed;
        std::map<std::string, std::pair<int, int>> named;  // times named without progress, missing then

        bool grantSent = false;
        bool announceFollows = false;
        std::deque<int> resend;
        std::vector<int> granted;
        std::string grantNamed;
        int grantOthers = 0;

        std::map<std::string, std::pair<int, uint64_t>> askers; // every station that asked: missing, when
        GroupFile::State state = GroupFile::State::Sending;
        std::string error;
    };

    struct Incoming
    {
        uint64_t id = 0;
        uint32_t fileId = 0;
        std::string sender;
        std::string name;
        bool announced = false;
        uint32_t size = 0;
        int pieceBytes = 0;
        int pieces = 0;
        Hash8 hash{};
        bool geometry = false;      // size, pieces and hash known
        int tempo = 0;
        std::map<int, std::vector<uint8_t>> spool;
        bool verified = false;
        int hashFailures = 0;
        bool restart = false;       // ask for all of it, after a hash failure

        GroupFile::State state = GroupFile::State::Heard;
        std::string path;           // chosen on Receive; the saved file once saved
        bool autoReceive = false;
        bool ended = false;         // the sender has stopped serving
        std::string error;

        uint64_t heardMs = 0;       // the last frame of this transfer
        uint64_t windowMs = 0;      // the last Window
        uint64_t stopAtMs = 0;      // the last Window's service time plus 2 min
        uint64_t windowLengthMs = 0;

        // The round's request.
        int round = -1;
        int windowTempo = -1;
        int slots = 0;
        uint64_t slotMs = 0;
        std::string priority;
        std::set<int> askedByOthers;
        bool asked = false;         // sent, withdrawn or silent this round
        bool pending = false;       // waiting for its slot
        int slot = -1;
        uint64_t sendAtMs = 0;
        uint64_t slotEndMs = 0;
        bool lateSent = false;      // the unsolicited Request since the last frame
        uint64_t lateAtMs = 0;      // when it goes, 0 when none is due

        std::string firstInLine;
        int coming = 0;
    };

    enum class KeyingKind
    {
        None,
        Announce,
        Pieces,
        Window,
        Grant,
        End,
        Request,
    };

    struct Taken
    {
        KeyingKind kind = KeyingKind::None;
        uint64_t id = 0;            // the transfer
        std::vector<std::vector<uint8_t>> frames;
        int gear = 0;
        bool retried = false;
        GroupFileEnd endReason = GroupFileEnd::Quiet; // an End's reason
    };

    ModeInfo modeFor(int gear) const;
    Outgoing* liveOutgoing();
    const Outgoing* liveOutgoing() const;
    Incoming* findIncoming(uint32_t fileId);
    Incoming* findIncomingById(uint64_t id);
    Incoming& incomingFor(uint32_t fileId, uint64_t nowMs);
    std::vector<int> missingOf(const Incoming& in) const;
    void learnGeometry(Incoming& in, uint32_t size, int pieceBytes, int pieces, const Hash8& hash);
    void storePiece(Incoming& in, int index, const std::vector<uint8_t>& bytes);
    void checkComplete(Incoming& in, uint64_t nowMs);
    bool save(Incoming& in);
    void endIncoming(Incoming& in, GroupFile::State state);
    void scheduleRequest(Incoming& in, uint64_t nowMs, bool fromSlotZero);
    bool buildRequest(Incoming& in, int round, std::vector<uint8_t>& frame);
    void limitSpool(uint64_t nowMs);

    void onRequest(const GroupRequest& request, uint64_t nowMs);
    void closeWindow(Outgoing& out, uint64_t nowMs);
    void elect(Outgoing& out);
    void startEnd(Outgoing& out, GroupFileEnd reason);
    std::vector<uint8_t> windowFrame(const Outgoing& out, uint64_t nowMs) const;
    std::vector<uint8_t> announceFrame(const Outgoing& out) const;
    std::vector<uint8_t> dataFrame(const Outgoing& out, int index) const;
    bool takeSenderKeying(Outgoing& out, uint64_t nowMs, Taken& taken);
    void finishOutgoing(Outgoing& out, GroupFileEnd reason, const std::string& error = std::string());
    void prune();
    void changed() { changes_++; }

    std::string myCall_;
    std::vector<ModeInfo> modes_;
    int gear_ = 3;
    std::mt19937 random_;
    std::string autoFolder_;
    bool inhibited_ = false;
    uint64_t holdUntilMs_ = 0;
    uint64_t nextKeyingMs_ = 0;     // the yield after our last file keying
    uint64_t lastTickMs_ = 0;

    std::list<Outgoing> outgoing_;
    std::list<Incoming> incoming_;
    Taken inFlight_;                // the keying with the transport
    Taken retry_;                   // one to send again
    uint64_t nextId_ = 1;
    uint64_t changes_ = 0;
};

} // namespace Data2G
} // namespace TextMessaging

#endif // TEXT_MESSAGING__DATA2G_BROADCAST_H
