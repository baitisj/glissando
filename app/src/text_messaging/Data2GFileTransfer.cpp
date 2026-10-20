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
#include <iterator>
#include <system_error>

#include "Sha256.h"

namespace TextMessaging
{
namespace Data2G
{

namespace
{

// Finished transfers kept for the window to show, past which the oldest go.
constexpr size_t FINISHED_KEPT = 64;

const char* const FALLBACK_NAME = "received-file";

const char* const NOT_ALLOWED = "Sending is not allowed here.";

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

// The first bytes of the SHA-256 of a file's first size bytes; empty if
// it can't be read.
std::vector<uint8_t> fingerprintOf(const std::filesystem::path& path, uint64_t size)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256 hash;
    std::vector<uint8_t> buffer(65536);
    uint64_t left = size;
    while (left > 0)
    {
        size_t want = (size_t)std::min<uint64_t>(buffer.size(), left);
        in.read((char*)buffer.data(), (std::streamsize)want);
        if ((size_t)in.gcount() != want) return {};
        hash.update(buffer.data(), want);
        left -= want;
    }
    Sha256::Digest digest = hash.finish();
    return std::vector<uint8_t>(digest.begin(), digest.begin() + FILE_FINGERPRINT_BYTES);
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

// A name in the folder that neither a file nor a part file has taken:
// the name offered, or "name (2).ext" and so on; empty if none is free.
std::filesystem::path freeSavePath(const std::filesystem::path& folder, const std::string& name)
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

FileTransferEngine::~FileTransferEngine()
{
    stopAll("closed");
    while (!kept_.empty()) dropKept(kept_.begin());
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
// saved is kept for the same file sent again, or deleted.
void FileTransferEngine::finish(Entry& entry, FileTransfer::State state, const std::string& error)
{
    FileTransfer& t = entry.transfer;
    t.state = state;
    if (!error.empty()) t.error = error;
    if (entry.in.is_open()) entry.in.close();
    if (entry.out.is_open())
    {
        entry.out.close();
        if (state != FileTransfer::State::Saved) keep(entry);
    }
    entry.part.clear();
    entry.pieces.clear();
    entry.arriving = 0;
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
    if (inhibited_)
    {
        error = NOT_ALLOWED;
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
    std::vector<uint8_t> fingerprint = fingerprintOf(file, size);
    if (fingerprint.empty())
    {
        error = "\"" + path + "\" could not be read.";
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
    entry.fingerprint = fingerprint;
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

// A part file kept for the same file from the same station, if any.
std::list<FileTransferEngine::Kept>::iterator FileTransferEngine::findKept(const Entry& entry)
{
    const FileTransfer& t = entry.transfer;
    if (entry.fingerprint.empty()) return kept_.end();
    return std::find_if(kept_.begin(), kept_.end(), [&](const Kept& k) {
        return k.peer == t.peer && k.size == t.size && k.fingerprint == entry.fingerprint;
    });
}

// Keeps what has come of a transfer of theirs that ended unfinished, if
// there is anything and it can be trusted; otherwise its part file goes.
void FileTransferEngine::keep(Entry& entry)
{
    const FileTransfer& t = entry.transfer;
    std::error_code ec;
    if (entry.spoiled || entry.fingerprint.empty() || t.done == 0 || t.done >= t.size)
    {
        std::filesystem::remove(entry.part, ec);
        return;
    }
    auto older = findKept(entry);
    if (older != kept_.end()) dropKept(older);
    Kept& k = kept_.emplace_back();
    k.peer = t.peer;
    k.size = t.size;
    k.fingerprint = entry.fingerprint;
    k.path = t.path;
    k.part = entry.part;
    k.bytes = t.done;
    k.keptAtMs = nowMs_;
    limitKept(nowMs_);
}

void FileTransferEngine::dropKept(std::list<Kept>::iterator it)
{
    std::error_code ec;
    std::filesystem::remove(it->part, ec);
    kept_.erase(it);
}

void FileTransferEngine::limitKept(uint64_t nowMs)
{
    for (auto it = kept_.begin(); it != kept_.end();)
    {
        // Kept where no time was to hand: its day starts now.
        if (it->keptAtMs == 0) it->keptAtMs = nowMs;
        bool old = nowMs > it->keptAtMs && nowMs - it->keptAtMs >= KEPT_MS;
        auto next = std::next(it);
        if (old) dropKept(it);
        it = next;
    }
    while (kept_.size() > KEPT_MAX) dropKept(kept_.begin());
}

// Carries on in the part file kept for the offer, with the rest asked for.
// False, with nothing changed, if it is gone or not as it was left.
bool FileTransferEngine::resumePart(Entry& entry, const std::string& path)
{
    FileTransfer& t = entry.transfer;
    auto kept = findKept(entry);
    if (kept == kept_.end() || kept->path != path) return false;
    std::error_code ec;
    if (std::filesystem::file_size(kept->part, ec) != kept->bytes || ec)
    {
        dropKept(kept);
        return false;
    }
    entry.out.open(kept->part, std::ios::binary | std::ios::app);
    if (!entry.out)
    {
        dropKept(kept);
        return false;
    }
    entry.part = kept->part;
    t.path = path;
    t.done = kept->bytes;
    t.resumedFrom = kept->bytes;
    t.kept = 0;
    t.keptPath.clear();
    t.state = FileTransfer::State::Receiving;
    kept_.erase(kept);

    std::vector<uint8_t> body = {entry.number, (uint8_t)(t.done >> 24), (uint8_t)(t.done >> 16),
                                 (uint8_t)(t.done >> 8), (uint8_t)t.done};
    queueRecord(t.peer, FileRecordType::AcceptFrom, body);
    changed();

    // All of it was here already.
    if (t.done == t.size) completeReceive(entry);
    return true;
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
    t.kept = 0;
    t.keptPath.clear();
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
    if (inhibited_)
    {
        error = NOT_ALLOWED;
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
    if (resumePart(*entry, path)) return true;
    // Saved somewhere else: what was kept of it is of no more use.
    auto kept = findKept(*entry);
    if (kept != kept_.end()) dropKept(kept);
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

std::vector<FileTransfer> FileTransferEngine::transfers(uint64_t nowMs) const
{
    std::vector<FileTransfer> out;
    out.reserve(entries_.size());
    for (const Entry& entry : entries_)
    {
        FileTransfer& t = out.emplace_back(entry.transfer);
        t.handed = t.outgoing ? std::max(entry.written, t.done) : std::min(t.size, t.done + entry.arriving);
        bool open = t.state == FileTransfer::State::Offered || t.state == FileTransfer::State::Asking;
        if (open && nowMs != 0)
        {
            uint64_t age = nowMs > entry.sinceMs ? nowMs - entry.sinceMs : 0;
            t.expiresInMs = age < OFFER_EXPIRY_MS ? OFFER_EXPIRY_MS - age : 0;
        }
    }
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
    if (going || inhibited_) return out;

    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.peer != peer || t.state != FileTransfer::State::Waiting) continue;

        // Its fingerprint first, so the far end knows the file when the
        // offer comes.
        std::vector<uint8_t> print(1 + entry.fingerprint.size());
        print[0] = entry.number;
        std::copy(entry.fingerprint.begin(), entry.fingerprint.end(), print.begin() + 1);
        std::vector<uint8_t> printRecord = fileRecordEncode(FileRecordType::Fingerprint, print);
        out.insert(out.end(), printRecord.begin(), printRecord.end());

        std::vector<uint8_t> body;
        body.push_back(entry.number);
        for (int shift = 24; shift >= 0; shift -= 8) body.push_back((uint8_t)(t.size >> shift));
        body.insert(body.end(), t.name.begin(), t.name.end());
        std::vector<uint8_t> record = fileRecordEncode(FileRecordType::Offer, body);
        out.insert(out.end(), record.begin(), record.end());

        t.state = FileTransfer::State::Offered;
        t.resumedFrom = 0;
        t.done = 0;
        entry.sinceMs = nowMs;
        entry.written = 0;
        changed();
        break;
    }
    return out;
}

bool FileTransferEngine::takePiece(const std::string& peer, uint64_t streamOffset, std::vector<uint8_t>& out)
{
    if (inhibited_) return false;
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
        entry.pieces.push_back({streamOffset + record.size() - length, entry.written, length});
        entry.written += length;
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
        while (!entry.pieces.empty() && entry.pieces.front().streamStart + entry.pieces.front().length <= streamAcked)
        {
            done = entry.pieces.front().fileStart + entry.pieces.front().length;
            entry.pieces.pop_front();
        }
        // Into the piece now going: to the step, so a fast modem doesn't
        // redraw the window for every frame.
        if (!entry.pieces.empty() && streamAcked > entry.pieces.front().streamStart)
        {
            const Entry::Piece& piece = entry.pieces.front();
            uint64_t into = (streamAcked - piece.streamStart) / PROGRESS_STEP * PROGRESS_STEP;
            done = std::max(done, piece.fileStart + into);
        }
        if (done != t.done)
        {
            t.done = done;
            changed();
        }
    }
}

void FileTransferEngine::arriving(const std::string& peer, uint8_t number, size_t bytes)
{
    Entry* entry = findIncoming(peer, number);
    if (entry == nullptr || entry->transfer.state != FileTransfer::State::Receiving) return;
    uint64_t stepped = bytes / PROGRESS_STEP * PROGRESS_STEP;
    if (stepped == entry->arriving) return;
    entry->arriving = stepped;
    changed();
}

void FileTransferEngine::autoAccept(Entry& entry)
{
    if (inhibited_ || autoFolder_.empty() || autoCalls_.count(entry.transfer.peer) == 0) return;
    if (!entry.transfer.keptPath.empty() && resumePart(entry, entry.transfer.keptPath))
    {
        entry.transfer.autoAccepted = true;
        return;
    }

    std::error_code ec;
    std::filesystem::path folder = pathFromUtf8(autoFolder_);
    std::filesystem::create_directories(folder, ec);
    std::filesystem::path chosen = freeSavePath(folder, entry.transfer.name);
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
    nowMs_ = nowMs;

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
            auto print = fingerprints_.find({peer, number});
            if (print != fingerprints_.end())
            {
                entry.fingerprint = print->second;
                fingerprints_.erase(print);
            }
            auto kept = findKept(entry);
            if (kept != kept_.end())
            {
                t.kept = kept->bytes;
                t.keptPath = kept->path;
            }
            changed();
            if (inhibited_)
            {
                // It could not be answered: the far end hears so at once.
                queueCancel(entry, CancelReason::ReceiverFailed);
                finish(entry, State::Failed, NOT_ALLOWED);
            }
            else
            {
                autoAccept(entry);
            }
            prune();
            break;
        }
        case FileRecordType::Fingerprint:
        {
            if (record.body.size() < 1 + FILE_FINGERPRINT_BYTES) return;
            // Only the latest few, in case Offers never follow.
            if (fingerprints_.size() >= 16) fingerprints_.clear();
            fingerprints_[{peer, number}] =
                std::vector<uint8_t>(record.body.begin() + 1, record.body.begin() + 1 + FILE_FINGERPRINT_BYTES);
            break;
        }
        case FileRecordType::Accept:
        case FileRecordType::AcceptFrom:
        {
            Entry* entry = findOutgoing(peer, number);
            if (entry == nullptr || entry->transfer.state != State::Offered) return;
            FileTransfer& t = entry->transfer;
            uint64_t from = 0;
            if ((FileRecordType)record.type == FileRecordType::AcceptFrom)
            {
                if (record.body.size() < 5) return;
                for (int i = 1; i <= 4; i++) from = (from << 8) | record.body[(size_t)i];
            }
            if (from > t.size)
            {
                queueCancel(*entry, CancelReason::SenderFailed);
                finish(*entry, State::Failed, t.peer + " asked to carry on past the end of the file.");
                return;
            }
            entry->in.open(pathFromUtf8(t.path), std::ios::binary);
            if (entry->in && from > 0) entry->in.seekg((std::streamoff)from);
            if (!entry->in)
            {
                queueCancel(*entry, CancelReason::SenderFailed);
                finish(*entry, State::Failed, "\"" + t.path + "\" could not be opened.");
                return;
            }
            entry->written = from;
            t.done = from;
            t.resumedFrom = from;
            t.state = State::Sending;
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
        entry.spoiled = true;
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed, t.peer + " sent more than it offered.");
        return;
    }

    entry.out.write((const char*)bytes, (std::streamsize)length);
    if (!entry.out)
    {
        entry.spoiled = true;
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed, "\"" + utf8FromPath(entry.part) + "\" could not be written.");
        return;
    }
    t.done += length;
    entry.arriving = 0;
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
    if (written && !entry.fingerprint.empty() && fingerprintOf(entry.part, t.size) != entry.fingerprint)
    {
        // Not the file offered: pieces from a changed file, say, after
        // carrying on from a kept part.
        entry.spoiled = true;
        std::error_code ec;
        std::filesystem::remove(entry.part, ec);
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed, "\"" + t.name + "\" did not match what " + t.peer + " sent.");
        return;
    }

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
            std::filesystem::path other = freeSavePath(final.parent_path(), t.name);
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
    nowMs_ = nowMs;
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

void FileTransferEngine::release(const std::string& peer, bool inSession)
{
    using State = FileTransfer::State;
    if (!inSession) pending_.erase(peer);
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (t.peer == peer) entry.awaitingSaved = false;
        if (t.peer != peer || !t.live()) continue;

        // One offered in the session is cancelled there too, so the far
        // end doesn't take the session's end for a Glissando that can't
        // take files.
        if (inSession && t.state != State::Waiting)
        {
            queueCancel(entry, t.outgoing ? CancelReason::SenderStopped : CancelReason::ReceiverStopped);
        }
        finish(entry, State::Cancelled);
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
    nowMs_ = nowMs;
    limitKept(nowMs);
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

void FileTransferEngine::setInhibited(bool inhibited)
{
    if (inhibited == inhibited_) return;
    inhibited_ = inhibited;
    if (!inhibited) return;

    using State = FileTransfer::State;
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.live()) continue;
        if (t.state != State::Waiting)
        {
            // As for a cancel: one written in full may be saved there already.
            entry.awaitingSaved = t.state == State::Sending && entry.written >= t.size;
            queueCancel(entry, t.outgoing ? CancelReason::SenderFailed : CancelReason::ReceiverFailed);
        }
        finish(entry, State::Failed, NOT_ALLOWED);
    }
}

bool FileTransferEngine::hasRecords(const std::string& peer) const
{
    auto pending = pending_.find(peer);
    return pending != pending_.end() && !pending->second.empty();
}

bool FileTransferEngine::wantsSession(std::string& peer, uint64_t& sinceMs) const
{
    if (inhibited_) return false;
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
    if (hasRecords(peer)) return true;
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const Entry& e) { return e.transfer.peer == peer && e.transfer.live(); });
}

} // namespace Data2G
} // namespace TextMessaging
