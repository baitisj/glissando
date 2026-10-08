//=========================================================================
// Name:            Data2GFileTransfer.cpp
// Purpose:         Offer, answer, pieces, Saved, cancel and expiry for
//                  files through a connected Data2G session.
//
// Written for Glissando; no Data2G code is used (see docs/DATA2G.md).
//=========================================================================

#include "Data2GFileTransfer.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace TextMessaging
{
namespace Data2G
{

namespace
{

// Finished transfers kept for the window to show, past which the oldest go.
constexpr size_t FINISHED_KEPT = 64;

const char* const FALLBACK_NAME = "received-file";

// The length of the UTF-8 sequence starting at text[at], or 0 when it is
// not a whole, valid one.
size_t utf8Length(const std::string& text, size_t at)
{
    unsigned char c = (unsigned char)text[at];
    size_t length = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (length == 0 || (length == 2 && c < 0xC2) || at + length > text.size()) return 0;
    for (size_t i = 1; i < length; i++)
    {
        if (((unsigned char)text[at + i] & 0xC0) != 0x80) return 0;
    }
    return length;
}

// At most bytes long, cut where a character ends.
std::string truncateUtf8(const std::string& text, size_t bytes)
{
    if (text.size() <= bytes) return text;
    size_t at = 0;
    while (at < text.size())
    {
        size_t length = utf8Length(text, at);
        if (length == 0) length = 1;
        if (at + length > bytes) break;
        at += length;
    }
    return text.substr(0, at);
}

bool reservedOnWindows(const std::string& stem)
{
    std::string upper;
    for (char c : stem) upper.push_back((char)std::toupper((unsigned char)c));
    while (!upper.empty() && upper.back() == ' ') upper.pop_back();
    static const char* const devices[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    for (const char* device : devices)
    {
        if (upper == device) return true;
    }
    bool port = upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0;
    if (!port) return false;
    if (upper.size() == 4) return upper[3] >= '0' && upper[3] <= '9';
    // Superscript one, two and three name the same ports.
    return upper.size() == 5 && (unsigned char)upper[3] == 0xC2 &&
           ((unsigned char)upper[4] == 0xB9 || (unsigned char)upper[4] == 0xB2 || (unsigned char)upper[4] == 0xB3);
}

std::vector<uint8_t> numbered(uint8_t number)
{
    return std::vector<uint8_t>(1, number);
}

// A name in the folder that neither a file nor a part file has taken:
// the name offered, or "name (2).ext" and so on; empty if none is free.
std::filesystem::path freePath(const std::filesystem::path& folder, const std::string& name)
{
    std::filesystem::path wanted = folder / pathFromUtf8(name);
    auto taken = [](const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::path part = path;
        part += ".part";
        return std::filesystem::exists(path, ec) || std::filesystem::exists(part, ec);
    };
    if (!taken(wanted)) return wanted;

    std::filesystem::path stem = wanted.stem();
    std::filesystem::path extension = wanted.extension();
    for (int n = 2; n < 1000; n++)
    {
        std::filesystem::path candidate = folder / stem;
        candidate += " (" + std::to_string(n) + ")";
        candidate += extension;
        if (!taken(candidate)) return candidate;
    }
    return std::filesystem::path();
}

} // namespace

bool FileTransfer::live() const
{
    switch (state)
    {
        case State::Waiting:
        case State::Offered:
        case State::Sending:
        case State::Asking:
        case State::Receiving:
            return true;
        default:
            return false;
    }
}

std::string safeFileName(const std::string& offered)
{
    // Only what follows the last separator of either kind.
    size_t slash = offered.find_last_of("/\\");
    std::string last = slash == std::string::npos ? offered : offered.substr(slash + 1);

    std::string clean;
    for (size_t at = 0; at < last.size();)
    {
        size_t length = utf8Length(last, at);
        if (length == 0)
        {
            at++; // not UTF-8: dropped
            continue;
        }
        unsigned char c = (unsigned char)last[at];
        bool control = c < 0x20 || c == 0x7F ||
                       (length == 2 && c == 0xC2 && (unsigned char)last[at + 1] < 0xA0); // C1 controls
        bool refused = length == 1 && std::string("<>:\"|?*").find((char)c) != std::string::npos;
        // Direction marks, embeddings, overrides and isolates, which would
        // let a name show another extension than it has.
        if (length == 3 && c == 0xE2)
        {
            unsigned char c1 = (unsigned char)last[at + 1], c2 = (unsigned char)last[at + 2];
            bool marks = c1 == 0x80 && (c2 == 0x8E || c2 == 0x8F || (c2 >= 0xAA && c2 <= 0xAE)); // U+200E-F, U+202A-E
            bool isolates = c1 == 0x81 && c2 >= 0xA6 && c2 <= 0xA9;                             // U+2066-9
            refused = refused || marks || isolates;
        }
        if (!control && !refused) clean.append(last, at, length);
        at += length;
    }

    // No hidden files, and nothing Windows would trim or refuse.
    size_t first = clean.find_first_not_of(". ");
    clean = first == std::string::npos ? std::string() : clean.substr(first);
    while (!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) clean.pop_back();

    clean = truncateUtf8(clean, FILE_NAME_BYTES);
    while (!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) clean.pop_back();
    if (clean.empty()) return FALLBACK_NAME;

    size_t dot = clean.find('.');
    if (reservedOnWindows(clean.substr(0, dot)))
    {
        return std::string(FALLBACK_NAME) + (dot == std::string::npos ? std::string() : clean.substr(dot));
    }
    return clean;
}

std::filesystem::path pathFromUtf8(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8FromPath(const std::filesystem::path& path)
{
    std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

FileTransferEngine::~FileTransferEngine()
{
    stopAll("closed");
}

FileTransferEngine::Entry* FileTransferEngine::find(uint64_t id)
{
    for (Entry& entry : entries_)
    {
        if (entry.transfer.id == id) return &entry;
    }
    return nullptr;
}

FileTransferEngine::Entry* FileTransferEngine::findOutgoing(const std::string& peer, uint8_t number)
{
    for (Entry& entry : entries_)
    {
        const FileTransfer& t = entry.transfer;
        bool offered = t.live() && t.state != FileTransfer::State::Waiting;
        if (t.outgoing && t.peer == peer && entry.number == number && offered) return &entry;
    }
    return nullptr;
}

FileTransferEngine::Entry* FileTransferEngine::findIncoming(const std::string& peer, uint8_t number)
{
    for (Entry& entry : entries_)
    {
        const FileTransfer& t = entry.transfer;
        if (!t.outgoing && t.peer == peer && entry.number == number && t.live()) return &entry;
    }
    return nullptr;
}

void FileTransferEngine::queueRecord(const std::string& peer, FileRecordType type, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> record = fileRecordEncode(type, body);
    std::vector<uint8_t>& queue = pending_[peer];
    queue.insert(queue.end(), record.begin(), record.end());
}

void FileTransferEngine::queueCancel(Entry& entry, CancelReason reason)
{
    queueRecord(entry.transfer.peer, FileRecordType::Cancel, {entry.number, (uint8_t)reason});
}

// The transfer is over: files are closed, and a part file of one not
// saved is deleted.
void FileTransferEngine::finish(Entry& entry, FileTransfer::State state, const std::string& error)
{
    FileTransfer& t = entry.transfer;
    t.state = state;
    if (!error.empty()) t.error = error;
    if (entry.in.is_open()) entry.in.close();
    if (entry.out.is_open())
    {
        entry.out.close();
        std::error_code ec;
        std::filesystem::remove(entry.part, ec);
    }
    entry.part.clear();
    entry.pieceEnds.clear();
    changed();
}

// Called only where a transfer is added, never while the list is being
// walked.
void FileTransferEngine::prune()
{
    size_t finished = (size_t)std::count_if(entries_.begin(), entries_.end(),
                                            [](const Entry& e) { return !e.transfer.live(); });
    for (auto it = entries_.begin(); it != entries_.end() && finished > FINISHED_KEPT;)
    {
        if (it->transfer.live())
        {
            ++it;
            continue;
        }
        it = entries_.erase(it);
        finished--;
    }
}

uint64_t FileTransferEngine::offer(const std::string& peer, const std::string& path, uint64_t nowMs,
                                   std::string& error)
{
    if (peer.empty())
    {
        error = "No station to send the file to.";
        return 0;
    }

    std::filesystem::path file = pathFromUtf8(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
    {
        error = "\"" + path + "\" is not a file that can be sent.";
        return 0;
    }
    uint64_t size = std::filesystem::file_size(file, ec);
    if (ec)
    {
        error = "\"" + path + "\" could not be read: " + ec.message();
        return 0;
    }
    if (size > MAX_FILE_BYTES)
    {
        error = "\"" + path + "\" is larger than the 4 GB a transfer can carry.";
        return 0;
    }
    std::ifstream probe(file, std::ios::binary);
    if (!probe)
    {
        error = "\"" + path + "\" could not be opened.";
        return 0;
    }

    Entry& entry = entries_.emplace_back();
    FileTransfer& t = entry.transfer;
    t.id = nextId_++;
    t.outgoing = true;
    t.peer = peer;
    t.name = truncateUtf8(utf8FromPath(file.filename()), FILE_NAME_BYTES);
    if (t.name.empty()) t.name = "file";
    t.size = size;
    t.path = path;
    t.state = FileTransfer::State::Waiting;
    entry.sinceMs = nowMs;
    entry.number = nextNumber_;
    nextNumber_ = nextNumber_ == 255 ? 1 : (uint8_t)(nextNumber_ + 1);
    changed();
    uint64_t id = t.id;
    prune();
    return id;
}

// "<path>.part", or "<path>.2.part" and so on when a file of that name is
// there already or another transfer is writing it; empty if none is free.
std::filesystem::path FileTransferEngine::freePart(const std::string& path) const
{
    for (int n = 1; n < 1000; n++)
    {
        std::filesystem::path part = pathFromUtf8(path);
        if (n > 1) part += "." + std::to_string(n);
        part += ".part";
        std::error_code ec;
        bool taken = std::filesystem::exists(part, ec) ||
                     std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.part == part; });
        if (!taken) return part;
    }
    return std::filesystem::path();
}

bool FileTransferEngine::openPart(Entry& entry, const std::string& path, std::string& error)
{
    FileTransfer& t = entry.transfer;
    std::filesystem::path part = freePart(path);
    if (part.empty())
    {
        error = "No part file could be made beside \"" + path + "\".";
        return false;
    }
    entry.out.open(part, std::ios::binary | std::ios::trunc);
    if (!entry.out)
    {
        error = "\"" + utf8FromPath(part) + "\" could not be written.";
        return false;
    }
    entry.part = part;
    t.path = path;
    t.done = 0;
    t.state = FileTransfer::State::Receiving;
    queueRecord(t.peer, FileRecordType::Accept, numbered(entry.number));
    changed();

    // Nothing is coming for an empty file: it is saved now.
    if (t.size == 0) completeReceive(entry);
    return true;
}

bool FileTransferEngine::accept(uint64_t id, const std::string& path, std::string& error)
{
    Entry* entry = find(id);
    if (entry == nullptr || entry->transfer.outgoing || entry->transfer.state != FileTransfer::State::Asking)
    {
        error = entry != nullptr && entry->transfer.state == FileTransfer::State::Expired
                    ? "The offer has expired."
                    : "The offer is no longer open.";
        return false;
    }
    for (const Entry& other : entries_)
    {
        const FileTransfer& o = other.transfer;
        if (&other != entry && !o.outgoing && o.state == FileTransfer::State::Receiving && o.path == path)
        {
            // Left open, for another name.
            error = "Another file is being saved as \"" + path + "\".";
            return false;
        }
    }
    if (openPart(*entry, path, error)) return true;

    // The far end hears that this side can't take it.
    entry->transfer.path = path;
    queueCancel(*entry, CancelReason::ReceiverFailed);
    finish(*entry, FileTransfer::State::Failed, error);
    return false;
}

bool FileTransferEngine::decline(uint64_t id)
{
    Entry* entry = find(id);
    if (entry == nullptr || entry->transfer.outgoing || entry->transfer.state != FileTransfer::State::Asking)
    {
        return false;
    }
    queueRecord(entry->transfer.peer, FileRecordType::Decline, numbered(entry->number));
    finish(*entry, FileTransfer::State::Declined);
    return true;
}

bool FileTransferEngine::cancel(uint64_t id)
{
    Entry* entry = find(id);
    if (entry == nullptr || !entry->transfer.live()) return false;

    using State = FileTransfer::State;
    switch (entry->transfer.state)
    {
        case State::Waiting:
            // Never offered: nothing to tell the far end.
            finish(*entry, State::Cancelled);
            break;
        case State::Offered:
        case State::Sending:
            // With all of it written, the far end may have saved it
            // already, and then ignores the Cancel: its Saved still counts.
            entry->awaitingSaved =
                entry->transfer.state == State::Sending && entry->written >= entry->transfer.size;
            queueCancel(*entry, CancelReason::SenderStopped);
            finish(*entry, State::Cancelled);
            break;
        case State::Asking:
            return decline(id);
        case State::Receiving:
            queueCancel(*entry, CancelReason::ReceiverStopped);
            finish(*entry, State::Cancelled);
            break;
        default:
            return false;
    }
    return true;
}

void FileTransferEngine::setAutoAccept(const std::string& folder, const std::vector<std::string>& calls)
{
    autoFolder_ = folder;
    autoCalls_.clear();
    for (const std::string& call : calls)
    {
        std::string normal = commandCallsign(call);
        if (!normal.empty()) autoCalls_.insert(normal);
    }
}

std::vector<FileTransfer> FileTransferEngine::transfers() const
{
    std::vector<FileTransfer> out;
    out.reserve(entries_.size());
    for (const Entry& entry : entries_) out.push_back(entry.transfer);
    return out;
}

std::vector<uint8_t> FileTransferEngine::takeRecords(const std::string& peer, uint64_t nowMs)
{
    std::vector<uint8_t> out;
    auto pending = pending_.find(peer);
    if (pending != pending_.end())
    {
        out.swap(pending->second);
        pending_.erase(pending);
    }

    // The next file for the station, once the last has finished.
    bool going = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
        return e.transfer.outgoing && e.transfer.peer == peer &&
               (e.transfer.state == FileTransfer::State::Offered || e.transfer.state == FileTransfer::State::Sending);
    });
    if (going) return out;

    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.peer != peer || t.state != FileTransfer::State::Waiting) continue;

        std::vector<uint8_t> body;
        body.push_back(entry.number);
        for (int shift = 24; shift >= 0; shift -= 8) body.push_back((uint8_t)(t.size >> shift));
        body.insert(body.end(), t.name.begin(), t.name.end());
        std::vector<uint8_t> record = fileRecordEncode(FileRecordType::Offer, body);
        out.insert(out.end(), record.begin(), record.end());

        t.state = FileTransfer::State::Offered;
        entry.sinceMs = nowMs;
        entry.written = 0;
        changed();
        break;
    }
    return out;
}

bool FileTransferEngine::takePiece(const std::string& peer, uint64_t streamOffset, std::vector<uint8_t>& out)
{
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.peer != peer || t.state != FileTransfer::State::Sending) continue;
        if (entry.written >= t.size) return false;

        size_t length = (size_t)std::min<uint64_t>(FILE_PIECE_BYTES, t.size - entry.written);
        std::vector<uint8_t> body(1 + length);
        body[0] = entry.number;
        entry.in.read((char*)body.data() + 1, (std::streamsize)length);
        if (!entry.in || (size_t)entry.in.gcount() != length)
        {
            // Changed or gone since it was offered.
            std::vector<uint8_t> cancel =
                fileRecordEncode(FileRecordType::Cancel, {entry.number, (uint8_t)CancelReason::SenderFailed});
            out.insert(out.end(), cancel.begin(), cancel.end());
            finish(entry, FileTransfer::State::Failed, "\"" + t.path + "\" could not be read.");
            return true;
        }

        std::vector<uint8_t> record = fileRecordEncode(FileRecordType::Data, body);
        out.insert(out.end(), record.begin(), record.end());
        entry.written += length;
        entry.pieceEnds.emplace_back(streamOffset + record.size(), entry.written);
        return true;
    }
    return false;
}

void FileTransferEngine::acknowledged(const std::string& peer, uint64_t streamAcked)
{
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.peer != peer || t.state != FileTransfer::State::Sending) continue;
        uint64_t done = t.done;
        while (!entry.pieceEnds.empty() && entry.pieceEnds.front().first <= streamAcked)
        {
            done = entry.pieceEnds.front().second;
            entry.pieceEnds.pop_front();
        }
        if (done != t.done)
        {
            t.done = done;
            changed();
        }
    }
}

void FileTransferEngine::autoAccept(Entry& entry)
{
    if (autoFolder_.empty() || autoCalls_.count(entry.transfer.peer) == 0) return;

    std::error_code ec;
    std::filesystem::path folder = pathFromUtf8(autoFolder_);
    std::filesystem::create_directories(folder, ec);
    std::filesystem::path chosen = freePath(folder, entry.transfer.name);
    std::string path = utf8FromPath(chosen);
    std::string error = "No free name for \"" + entry.transfer.name + "\" in \"" + autoFolder_ + "\".";
    if (!chosen.empty() && openPart(entry, path, error))
    {
        entry.transfer.autoAccepted = true;
    }
    else
    {
        // Not written: the operator is asked instead.
        entry.transfer.error = error;
        entry.transfer.state = FileTransfer::State::Asking;
        if (entry.out.is_open()) entry.out.close();
    }
}

void FileTransferEngine::onRecord(const std::string& peer, const FileRecord& record, uint64_t nowMs)
{
    if (record.body.empty()) return;
    uint8_t number = record.body[0];
    using State = FileTransfer::State;

    switch ((FileRecordType)record.type)
    {
        case FileRecordType::Offer:
        {
            if (record.body.size() < 5 || findIncoming(peer, number) != nullptr) return;
            Entry& entry = entries_.emplace_back();
            FileTransfer& t = entry.transfer;
            t.id = nextId_++;
            t.outgoing = false;
            t.peer = peer;
            t.size = 0;
            for (int i = 1; i <= 4; i++) t.size = (t.size << 8) | record.body[(size_t)i];
            t.name = safeFileName(std::string(record.body.begin() + 5, record.body.end()));
            t.state = State::Asking;
            entry.number = number;
            entry.sinceMs = nowMs;
            changed();
            autoAccept(entry);
            prune();
            break;
        }
        case FileRecordType::Accept:
        {
            Entry* entry = findOutgoing(peer, number);
            if (entry == nullptr || entry->transfer.state != State::Offered) return;
            entry->in.open(pathFromUtf8(entry->transfer.path), std::ios::binary);
            if (!entry->in)
            {
                queueCancel(*entry, CancelReason::SenderFailed);
                finish(*entry, State::Failed, "\"" + entry->transfer.path + "\" could not be opened.");
                return;
            }
            entry->transfer.state = State::Sending;
            changed();
            break;
        }
        case FileRecordType::Decline:
        {
            Entry* entry = findOutgoing(peer, number);
            if (entry != nullptr && entry->transfer.state == State::Offered) finish(*entry, State::Declined);
            break;
        }
        case FileRecordType::Data:
        {
            // Anything for a transfer not under way, such as pieces still
            // coming after it was cancelled, is dropped.
            Entry* entry = findIncoming(peer, number);
            if (entry == nullptr || entry->transfer.state != State::Receiving) return;
            receivePiece(*entry, record.body.data() + 1, record.body.size() - 1);
            break;
        }
        case FileRecordType::Saved:
        {
            Entry* entry = findOutgoing(peer, number);
            if (entry == nullptr)
            {
                // Saved before our Cancel of it arrived: it was delivered.
                for (Entry& e : entries_)
                {
                    const FileTransfer& t = e.transfer;
                    if (e.awaitingSaved && t.outgoing && t.peer == peer && e.number == number) entry = &e;
                }
                if (entry == nullptr) return;
                entry->awaitingSaved = false;
            }
            else if (entry->transfer.state != State::Sending)
            {
                return;
            }
            entry->transfer.done = entry->transfer.size;
            finish(*entry, State::Delivered);
            break;
        }
        case FileRecordType::Cancel:
        {
            // The reason says whose transfer it ends; one without a reason
            // ends whichever has that number, ours first.
            bool hasReason = record.body.size() > 1;
            uint8_t reason = hasReason ? record.body[1] : 0;
            Entry* ours = (!hasReason || (reason & CANCEL_FROM_RECEIVER) != 0) ? findOutgoing(peer, number) : nullptr;
            Entry* theirs = ours == nullptr && (!hasReason || (reason & CANCEL_FROM_RECEIVER) == 0)
                                ? findIncoming(peer, number)
                                : nullptr;
            Entry* entry = ours != nullptr ? ours : theirs;
            if (entry == nullptr) return;

            State state = State::CancelledThere;
            if (reason == (uint8_t)CancelReason::SenderExpired || reason == (uint8_t)CancelReason::ReceiverExpired)
            {
                state = State::Expired;
            }
            else if (reason == (uint8_t)CancelReason::SenderFailed || reason == (uint8_t)CancelReason::ReceiverFailed)
            {
                state = State::FailedThere;
            }
            finish(*entry, state);
            break;
        }
        default:
            // A record of a later version: not for us.
            break;
    }
}

void FileTransferEngine::receivePiece(Entry& entry, const uint8_t* bytes, size_t length)
{
    FileTransfer& t = entry.transfer;
    if (t.done + length > t.size)
    {
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed, t.peer + " sent more than it offered.");
        return;
    }

    entry.out.write((const char*)bytes, (std::streamsize)length);
    if (!entry.out)
    {
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed, "\"" + utf8FromPath(entry.part) + "\" could not be written.");
        return;
    }
    t.done += length;
    changed();
    if (t.done == t.size) completeReceive(entry);
}

// All of it has come: the part file becomes the file, and the sender is
// told it is saved.
void FileTransferEngine::completeReceive(Entry& entry)
{
    FileTransfer& t = entry.transfer;
    entry.out.close();
    bool written = !entry.out.fail();

    std::filesystem::path final = pathFromUtf8(t.path);
    std::filesystem::path part = entry.part;
    std::error_code ec;
    if (written)
    {
        // Another transfer's part file is never replaced.
        auto clash = [&](const std::filesystem::path& path) {
            return std::any_of(entries_.begin(), entries_.end(),
                               [&](const Entry& e) { return &e != &entry && !e.part.empty() && e.part == path; });
        };
        if (t.autoAccepted && (std::filesystem::exists(final, ec) || clash(final)))
        {
            // Taken while it came: auto-accept never replaces a file, so
            // it gets the next free name.
            std::filesystem::path other = freePath(final.parent_path(), t.name);
            if (!other.empty() && !clash(other))
            {
                final = other;
                t.path = utf8FromPath(final);
            }
            else
            {
                written = false;
            }
        }
        else if (clash(final))
        {
            written = false;
        }
        else if (std::filesystem::exists(final, ec))
        {
            // The operator agreed, in the save dialog, to replace it.
            std::filesystem::remove(final, ec);
        }
        if (written) std::filesystem::rename(part, final, ec);
    }
    if (!written || ec)
    {
        std::filesystem::remove(part, ec);
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed, "\"" + t.path + "\" could not be saved.");
        return;
    }

    queueRecord(t.peer, FileRecordType::Saved, numbered(entry.number));
    finish(entry, FileTransfer::State::Saved);
}

void FileTransferEngine::sessionEnded(const std::string& peer, uint64_t nowMs, bool endedThere)
{
    using State = FileTransfer::State;
    pending_.erase(peer);
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (t.peer == peer) entry.awaitingSaved = false;
        if (t.peer != peer || !t.live()) continue;
        switch (t.state)
        {
            case State::Waiting:
                entry.sinceMs = nowMs; // for the next session
                break;
            case State::Offered:
                // A Glissando without file transfer closes the session on
                // the offer. One that ended on this side says nothing of
                // the far end.
                if (endedThere) finish(entry, State::NotSupported);
                else finish(entry, State::Failed, "The session with " + peer + " ended.");
                break;
            default:
                finish(entry, State::Failed, "The session with " + peer + " ended.");
                break;
        }
    }
}

void FileTransferEngine::noSession(const std::string& peer, const std::string& why)
{
    for (Entry& entry : entries_)
    {
        if (entry.transfer.peer == peer && entry.transfer.state == FileTransfer::State::Waiting)
        {
            finish(entry, FileTransfer::State::Failed, why);
        }
    }
}

void FileTransferEngine::release(const std::string& peer)
{
    pending_.erase(peer);
    for (Entry& entry : entries_)
    {
        if (entry.transfer.peer == peer) entry.awaitingSaved = false;
        if (entry.transfer.peer == peer && entry.transfer.live()) finish(entry, FileTransfer::State::Cancelled);
    }
}

void FileTransferEngine::stopAll(const std::string& why)
{
    pending_.clear();
    for (Entry& entry : entries_)
    {
        entry.awaitingSaved = false;
        if (entry.transfer.live()) finish(entry, FileTransfer::State::Failed, why);
    }
}

void FileTransferEngine::tick(uint64_t nowMs)
{
    using State = FileTransfer::State;
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if ((t.state != State::Offered && t.state != State::Asking) || nowMs - entry.sinceMs < OFFER_EXPIRY_MS)
        {
            continue;
        }
        queueCancel(entry, t.outgoing ? CancelReason::SenderExpired : CancelReason::ReceiverExpired);
        finish(entry, State::Expired);
    }
}

bool FileTransferEngine::wantsSession(std::string& peer, uint64_t& sinceMs) const
{
    bool found = false;
    for (const Entry& entry : entries_)
    {
        const FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.state != FileTransfer::State::Waiting) continue;
        if (!found || entry.sinceMs < sinceMs)
        {
            peer = t.peer;
            sinceMs = entry.sinceMs;
            found = true;
        }
    }
    return found;
}

bool FileTransferEngine::busyWith(const std::string& peer) const
{
    auto pending = pending_.find(peer);
    if (pending != pending_.end() && !pending->second.empty()) return true;
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const Entry& e) { return e.transfer.peer == peer && e.transfer.live(); });
}

} // namespace Data2G
} // namespace TextMessaging
