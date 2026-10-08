//=========================================================================
// Name:            Data2GFileTransfer.h
// Purpose:         Files sent and received through a connected Data2G
//                  session: the offer, the answer, the pieces of the file,
//                  the receiver's word that it has saved it, cancelling and
//                  expiry, for each station chat has a session with. No
//                  sockets: the transport hands it the records that arrive
//                  and writes the ones it gives back, so all of it is
//                  tested on its own.
//
// Written for Glissando; no Data2G code is used (see docs/DATA2G.md).
//=========================================================================

#ifndef TEXT_MESSAGING__DATA2G_FILE_TRANSFER_H
#define TEXT_MESSAGING__DATA2G_FILE_TRANSFER_H

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Data2GLink.h"

namespace TextMessaging
{
namespace Data2G
{

// One file going one way, as the chat window shows it.
struct FileTransfer
{
    enum class State
    {
        // Ours, being sent.
        Waiting,        // for a session with the station, or for the file ahead of it
        Offered,        // the offer is in the session; no answer yet
        Sending,        // accepted: the pieces are going, then the far end saves it
        Delivered,      // the far end has saved it
        Declined,

        // Theirs, being received.
        Asking,         // offered to us: the operator has yet to answer
        Receiving,
        Saved,

        // Either way.
        Cancelled,          // by this operator
        CancelledThere,     // by the far end's operator
        Expired,            // the offer went unanswered for OFFER_EXPIRY_MS
        Failed,             // the session ended under it, or this side could not read or write it
        FailedThere,        // the far end could not write or read it
        NotSupported,       // the far end's Glissando can't take files
    };

    uint64_t id = 0;            // this run's own, unique, for the window to name it by
    bool outgoing = true;
    std::string peer;
    std::string name;           // the name offered; one received is made safe to save under
    uint64_t size = 0;
    uint64_t done = 0;          // sending: bytes the far end's modem has acknowledged; receiving: bytes written
    State state = State::Waiting;
    std::string path;           // the file sent, or where the one received is saved; UTF-8
    std::string error;          // why it failed, when this side knows
    bool autoAccepted = false;  // taken without asking, from a station on the auto-accept list

    // Still going: waiting, offered, being answered or under way.
    bool live() const;
};

// A name offered by the far end, made safe to save under: only its last
// path part, without control characters, path separators or the
// characters Windows refuses, without leading or trailing dots and
// spaces, and not a Windows device name. "received-file" when nothing is
// left.
std::string safeFileName(const std::string& offered);

// UTF-8 paths both ways, so names outside ASCII survive on Windows too.
std::filesystem::path pathFromUtf8(const std::string& utf8);
std::string utf8FromPath(const std::filesystem::path& path);

// The transfers of every station chat has had a session with. One file
// goes to a station at a time; the next waits for it. Not thread safe:
// the transport holds its lock around every call.
class FileTransferEngine
{
public:
    // An offer nobody answers is given up on, by both sides, after this.
    static constexpr uint64_t OFFER_EXPIRY_MS = 4 * 60 * 1000;

    // The Offer record's size field.
    static constexpr uint64_t MAX_FILE_BYTES = 0xFFFFFFFFull;

    FileTransferEngine() = default;
    ~FileTransferEngine();

    FileTransferEngine(const FileTransferEngine&) = delete;
    FileTransferEngine& operator=(const FileTransferEngine&) = delete;

    //---------------------------------------------------------------------
    // The operator's side
    //---------------------------------------------------------------------

    // Queues a file for a station: it is offered once a session with the
    // station is up and nothing else is going to it. 0, with the reason,
    // if the file can't be sent.
    uint64_t offer(const std::string& peer, const std::string& path, uint64_t nowMs, std::string& error);

    // Answers an offer: the file is written to path plus ".part" (or a
    // numbered part file, if that name is taken), and renamed to path once
    // all of it has come. Refused, the offer left open, if another
    // transfer is being saved to path.
    bool accept(uint64_t id, const std::string& path, std::string& error);
    bool decline(uint64_t id);

    // Stops a transfer either way; the far end is told. An offer to us
    // not yet answered is declined.
    bool cancel(uint64_t id);

    // Files from these stations are taken without asking, into folder,
    // under the name offered (numbered when one is there already). An
    // empty folder or list turns it off.
    void setAutoAccept(const std::string& folder, const std::vector<std::string>& calls);

    std::vector<FileTransfer> transfers() const;

    // Moves whenever anything transfers() shows changes.
    uint64_t changes() const { return changes_; }

    //---------------------------------------------------------------------
    // The transport's side, for the station a session is up with
    //---------------------------------------------------------------------

    // The records to write into the session now: answers, cancels and
    // Saved, then the offer of the next file once none is going.
    std::vector<uint8_t> takeRecords(const std::string& peer, uint64_t nowMs);

    // Appends the next piece of the file going to the station, if any;
    // streamOffset is where in the session's byte stream it will start.
    bool takePiece(const std::string& peer, uint64_t streamOffset, std::vector<uint8_t>& out);

    // The far end's modem has acknowledged the session's bytes up to here.
    void acknowledged(const std::string& peer, uint64_t streamAcked);

    void onRecord(const std::string& peer, const FileRecord& record, uint64_t nowMs);

    // The session with the station has ended: what was under way in it
    // failed, and a file still waiting its turn waits for the next
    // session. endedThere: the far end ended it (not this side, nor the
    // loss of data2g-host's ports), so an offer of ours still unanswered
    // was not understood; otherwise that offer failed too.
    void sessionEnded(const std::string& peer, uint64_t nowMs, bool endedThere);

    // No session could be had with the station: its files fail.
    void noSession(const std::string& peer, const std::string& why);

    // The operator let go of the station: everything with it is cancelled
    // here, with nothing sent, as the session ends at once.
    void release(const std::string& peer);

    // The transport stopped: everything still going failed.
    void stopAll(const std::string& why);

    // Expires offers left unanswered.
    void tick(uint64_t nowMs);

    // The station a file of ours waits for a session with, the one
    // waiting longest, and since when.
    bool wantsSession(std::string& peer, uint64_t& sinceMs) const;

    // Something is under way with the station, or waiting for it, so its
    // session stays open.
    bool busyWith(const std::string& peer) const;

private:
    struct Entry
    {
        FileTransfer transfer;
        uint8_t number = 0;         // the sender's number for it
        uint64_t sinceMs = 0;       // Waiting: since queued; Offered and Asking: since offered
        std::ifstream in;           // ours, once accepted
        std::ofstream out;          // theirs, once accepted
        std::filesystem::path part; // where out writes, while it does
        uint64_t written = 0;       // ours: bytes of the file written into the session
        bool awaitingSaved = false; // ours, cancelled with all of it written: a Saved still delivers it
        std::deque<std::pair<uint64_t, uint64_t>> pieceEnds; // stream offset a piece ends at, file bytes to there
    };

    Entry* find(uint64_t id);
    Entry* findOutgoing(const std::string& peer, uint8_t number);
    Entry* findIncoming(const std::string& peer, uint8_t number);
    void queueRecord(const std::string& peer, FileRecordType type, const std::vector<uint8_t>& body);
    void queueCancel(Entry& entry, CancelReason reason);
    void finish(Entry& entry, FileTransfer::State state, const std::string& error = std::string());
    std::filesystem::path freePart(const std::string& path) const;
    bool openPart(Entry& entry, const std::string& path, std::string& error);
    void receivePiece(Entry& entry, const uint8_t* bytes, size_t length);
    void completeReceive(Entry& entry);
    void autoAccept(Entry& entry);
    void prune();
    void changed() { changes_++; }

    std::list<Entry> entries_;
    std::map<std::string, std::vector<uint8_t>> pending_;   // records waiting for each station's session
    uint64_t nextId_ = 1;
    uint8_t nextNumber_ = 1;
    uint64_t changes_ = 0;
    std::string autoFolder_;
    std::set<std::string> autoCalls_;
};

} // namespace Data2G
} // namespace TextMessaging

#endif // TEXT_MESSAGING__DATA2G_FILE_TRANSFER_H
