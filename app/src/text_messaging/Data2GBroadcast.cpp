//=========================================================================
// Name:            Data2GBroadcast.cpp
// Purpose:         Files to and from everybody on the GLISS group: the
//                  frames, and the sender's and listeners' sides of a
//                  transfer.
//
// Written for Glissando; no Data2G code is used (see docs/DATA2G.md).
//=========================================================================

#include "Data2GBroadcast.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#include "Data2GFileTransfer.h"
#include "Sha256.h"

namespace TextMessaging
{
namespace Data2G
{

namespace
{

constexpr uint64_t NEVER = std::numeric_limits<uint64_t>::max();

// Finished transfers kept for the window to show, past which the oldest go.
constexpr size_t FINISHED_KEPT = 64;

// The most all spools together hold.
constexpr size_t SPOOL_BYTES = 4 * 1024 * 1024;

// The largest a Data frame's payload can be: the frame limit less its header.
constexpr int DATA_HEADER_BYTES = 6;
constexpr int MAX_PIECE_BYTES = (int)GROUP_FRAME_MAX_BYTES - DATA_HEADER_BYTES;

// Data2G's length before each frame in a burst.
constexpr int LENGTH_BYTES = 2;

// What a keying may take at OFDM, and the most pieces in one.
constexpr double BURST_LIMIT_SECONDS = 12.0;
constexpr int MAX_PIECES_PER_KEYING = 8;

// Sizes for estimates: an Announce with a usual call and name, a Window.
constexpr int ANNOUNCE_ESTIMATE_BYTES = 40;
constexpr int WINDOW_BYTES = 25;

void putU16(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back((uint8_t)(value >> 8));
    out.push_back((uint8_t)value);
}

void putU24(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back((uint8_t)(value >> 16));
    putU16(out, value);
}

uint32_t getU16(const uint8_t* p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

uint32_t getU24(const uint8_t* p)
{
    return ((uint32_t)p[0] << 16) | getU16(p + 1);
}

// A call as it goes in a frame: a length byte, then up to 10 characters.
void putCall(std::vector<uint8_t>& out, const std::string& call)
{
    std::string c = commandCallsign(call);
    out.push_back((uint8_t)c.size());
    out.insert(out.end(), c.begin(), c.end());
}

bool getCall(const uint8_t* bytes, size_t length, size_t& at, std::string& out, bool allowEmpty)
{
    if (at >= length) return false;
    size_t n = bytes[at++];
    if (n > 10 || (!allowEmpty && n == 0) || at + n > length) return false;
    out.assign((const char*)bytes + at, n);
    at += n;
    return commandCallsign(out) == out;
}

// At most bytes long, cut where a UTF-8 character starts.
std::string truncateUtf8(const std::string& text, size_t bytes)
{
    if (text.size() <= bytes) return text;
    size_t at = bytes;
    while (at > 0 && ((unsigned char)text[at] & 0xC0) == 0x80) at--;
    return text.substr(0, at);
}

bool isType(const uint8_t* bytes, size_t length, GroupFileFrame type)
{
    return bytes != nullptr && length > 0 && bytes[0] == (uint8_t)type;
}

// Consecutive pieces, as ranges carry them: no more than 256 a range.
size_t rangesIn(const std::vector<int>& pieces, size_t count)
{
    size_t ranges = 0;
    int start = -1;
    int previous = -2;
    for (size_t i = 0; i < count; i++)
    {
        int p = pieces[i];
        if (p != previous + 1 || p - start >= 256)
        {
            ranges++;
            start = p;
        }
        previous = p;
    }
    return ranges;
}

size_t bitmapBytes(const std::vector<int>& pieces, size_t count)
{
    if (count == 0) return 0;
    return 2 + (size_t)(pieces[count - 1] - pieces[0]) / 8 + 1;
}

// A Request's or Grant's header, a piece set filling the room left.
std::vector<uint8_t> withPieceSet(std::vector<uint8_t> header, const std::vector<int>& pieces, size_t maxBytes,
                                  size_t& listedOut)
{
    std::vector<uint8_t> set;
    size_t room = maxBytes > header.size() ? maxBytes - header.size() : 0;
    listedOut = encodePieceSet(pieces, room, set);
    header.insert(header.end(), set.begin(), set.end());
    return header;
}

std::vector<int> sortedUnique(std::vector<int> pieces)
{
    std::sort(pieces.begin(), pieces.end());
    pieces.erase(std::unique(pieces.begin(), pieces.end()), pieces.end());
    return pieces;
}

} // namespace

//-------------------------------------------------------------------------
// Frames
//-------------------------------------------------------------------------

bool isGroupFileFrame(const uint8_t* bytes, size_t length)
{
    return bytes != nullptr && length > 0 && (bytes[0] >> 4) == 0;
}

size_t encodePieceSet(const std::vector<int>& ascending, size_t room, std::vector<uint8_t>& out)
{
    out.clear();
    if (room < 1) return 0;

    // The most of the lowest pieces either form fits, and which form.
    size_t best = 0;
    bool bitmap = false;
    for (size_t k = 1; k <= ascending.size(); k++)
    {
        size_t asBitmap = 1 + bitmapBytes(ascending, k);
        size_t asRanges = 1 + 3 * rangesIn(ascending, k);
        if (std::min(asBitmap, asRanges) > room) break;
        best = k;
        bitmap = asBitmap < asRanges;
    }

    if (best == 0)
    {
        out.push_back(1); // no ranges
        return 0;
    }

    if (bitmap)
    {
        out.push_back(0);
        int base = ascending[0];
        putU16(out, (uint32_t)base);
        size_t bytes = bitmapBytes(ascending, best) - 2;
        size_t at = out.size();
        out.resize(at + bytes, 0);
        for (size_t i = 0; i < best; i++)
        {
            int bit = ascending[i] - base;
            out[at + (size_t)bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
        }
    }
    else
    {
        out.push_back(1);
        size_t i = 0;
        while (i < best)
        {
            int start = ascending[i];
            size_t j = i + 1;
            while (j < best && ascending[j] == ascending[j - 1] + 1 && ascending[j] - start < 256) j++;
            putU16(out, (uint32_t)start);
            out.push_back((uint8_t)(j - i - 1));
            i = j;
        }
    }
    return best;
}

bool decodePieceSet(const uint8_t* bytes, size_t length, std::vector<int>& out)
{
    out.clear();
    if (length < 1) return false;
    if (bytes[0] == 0)
    {
        if (length < 3) return false;
        int base = (int)getU16(bytes + 1);
        for (size_t i = 3; i < length; i++)
        {
            for (int bit = 0; bit < 8; bit++)
            {
                if ((bytes[i] & (0x80 >> bit)) == 0) continue;
                int piece = base + (int)(i - 3) * 8 + bit;
                if (piece > 0xFFFF) return false;
                out.push_back(piece);
            }
        }
        return true;
    }
    if (bytes[0] == 1)
    {
        if ((length - 1) % 3 != 0) return false;
        for (size_t i = 1; i < length; i += 3)
        {
            int start = (int)getU16(bytes + i);
            int count = bytes[i + 2] + 1;
            if (start + count - 1 > 0xFFFF) return false;
            for (int p = 0; p < count; p++) out.push_back(start + p);
        }
        out = sortedUnique(out);
        return true;
    }
    return false;
}

std::vector<uint8_t> encodeGroupAnnounce(const GroupAnnounce& frame)
{
    std::vector<uint8_t> out;
    out.push_back((uint8_t)GroupFileFrame::Announce);
    putU24(out, frame.fileId);
    out.push_back(frame.version);
    putU24(out, frame.size);
    out.push_back((uint8_t)frame.pieceBytes);
    putU16(out, (uint32_t)frame.pieces);
    out.insert(out.end(), frame.hash.begin(), frame.hash.end());
    out.push_back((uint8_t)frame.tempo);
    putU16(out, (uint32_t)std::min(frame.airSeconds, 0xFFFF));
    putCall(out, frame.call);
    std::string name = truncateUtf8(frame.name, GROUP_FILE_NAME_BYTES);
    out.push_back((uint8_t)name.size());
    out.insert(out.end(), name.begin(), name.end());
    return out;
}

bool decodeGroupAnnounce(const uint8_t* bytes, size_t length, GroupAnnounce& out)
{
    if (!isType(bytes, length, GroupFileFrame::Announce) || length < 23) return false;
    GroupAnnounce a;
    a.fileId = getU24(bytes + 1);
    a.version = bytes[4];
    a.size = getU24(bytes + 5);
    a.pieceBytes = bytes[8];
    a.pieces = (int)getU16(bytes + 9);
    std::copy(bytes + 11, bytes + 19, a.hash.begin());
    a.tempo = bytes[19];
    a.airSeconds = (int)getU16(bytes + 20);
    size_t at = 22;
    if (!getCall(bytes, length, at, a.call, false)) return false;
    if (at >= length) return false;
    size_t nameLength = bytes[at++];
    if (nameLength > GROUP_FILE_NAME_BYTES || at + nameLength > length) return false;
    a.name.assign((const char*)bytes + at, nameLength);
    if (a.version < 1 || a.pieceBytes < 1 || a.pieceBytes > MAX_PIECE_BYTES || a.tempo > 4) return false;
    if (a.size == 0 || a.pieces != (int)((a.size + (uint32_t)a.pieceBytes - 1) / (uint32_t)a.pieceBytes)) return false;
    // No sender sends more, so no listener keeps more.
    if (a.size > GROUP_FILE_MAX_BYTES) return false;
    out = a;
    return true;
}

std::vector<uint8_t> encodeGroupData(const GroupData& frame)
{
    std::vector<uint8_t> out;
    out.reserve(DATA_HEADER_BYTES + frame.bytes.size());
    out.push_back((uint8_t)GroupFileFrame::Data);
    putU24(out, frame.fileId);
    putU16(out, (uint32_t)frame.index);
    out.insert(out.end(), frame.bytes.begin(), frame.bytes.end());
    return out;
}

bool decodeGroupData(const uint8_t* bytes, size_t length, GroupData& out)
{
    if (!isType(bytes, length, GroupFileFrame::Data) || length <= (size_t)DATA_HEADER_BYTES) return false;
    out.fileId = getU24(bytes + 1);
    out.index = (int)getU16(bytes + 4);
    out.bytes.assign(bytes + DATA_HEADER_BYTES, bytes + length);
    return true;
}

std::vector<uint8_t> encodeGroupWindow(const GroupWindow& frame)
{
    std::vector<uint8_t> out;
    out.push_back((uint8_t)GroupFileFrame::Window);
    putU24(out, frame.fileId);
    out.push_back((uint8_t)frame.round);
    putU24(out, frame.size);
    out.push_back((uint8_t)frame.pieceBytes);
    putU16(out, (uint32_t)frame.pieces);
    out.insert(out.end(), frame.hash.begin(), frame.hash.end());
    out.push_back((uint8_t)frame.slots);
    out.push_back((uint8_t)std::min(frame.slotHalfSeconds, 255));
    putU16(out, (uint32_t)std::min(frame.serviceLeftTens, 0xFFFF));
    putCall(out, frame.priorityCall);
    // An addition to the layout: the tempo to ask in, since ours follows
    // the console's. A listener that doesn't know it uses the Announce's.
    if (frame.tempo >= 0) out.push_back((uint8_t)frame.tempo);
    return out;
}

bool decodeGroupWindow(const uint8_t* bytes, size_t length, GroupWindow& out)
{
    if (!isType(bytes, length, GroupFileFrame::Window) || length < 24) return false;
    GroupWindow w;
    w.fileId = getU24(bytes + 1);
    w.round = bytes[4];
    w.size = getU24(bytes + 5);
    w.pieceBytes = bytes[8];
    w.pieces = (int)getU16(bytes + 9);
    std::copy(bytes + 11, bytes + 19, w.hash.begin());
    w.slots = bytes[19];
    w.slotHalfSeconds = bytes[20];
    w.serviceLeftTens = (int)getU16(bytes + 21);
    size_t at = 23;
    if (!getCall(bytes, length, at, w.priorityCall, true)) return false;
    w.tempo = at < length && bytes[at] <= 4 ? bytes[at] : -1;
    if (w.slots < 1 || w.slotHalfSeconds < 1 || w.pieceBytes < 1 || w.pieceBytes > MAX_PIECE_BYTES) return false;
    if (w.size == 0 || w.pieces != (int)((w.size + (uint32_t)w.pieceBytes - 1) / (uint32_t)w.pieceBytes)) return false;
    if (w.size > GROUP_FILE_MAX_BYTES) return false;
    out = w;
    return true;
}

std::vector<uint8_t> encodeGroupRequest(const GroupRequest& frame, size_t maxBytes, size_t& listedOut)
{
    std::vector<int> pieces = sortedUnique(frame.pieces);
    std::vector<uint8_t> header;
    header.push_back((uint8_t)GroupFileFrame::Request);
    putU24(header, frame.fileId);
    header.push_back((uint8_t)frame.round);
    header.push_back(frame.flags);
    putU16(header, (uint32_t)std::min(frame.missingTotal, 0xFFFF));
    putCall(header, frame.call);
    std::vector<uint8_t> out = withPieceSet(header, pieces, maxBytes, listedOut);
    if (listedOut < pieces.size()) out[5] |= REQUEST_TRUNCATED;
    return out;
}

bool decodeGroupRequest(const uint8_t* bytes, size_t length, GroupRequest& out)
{
    if (!isType(bytes, length, GroupFileFrame::Request) || length < 11) return false;
    GroupRequest r;
    r.fileId = getU24(bytes + 1);
    r.round = bytes[4];
    r.flags = bytes[5];
    r.missingTotal = (int)getU16(bytes + 6);
    size_t at = 8;
    if (!getCall(bytes, length, at, r.call, false)) return false;
    if (!decodePieceSet(bytes + at, length - at, r.pieces)) return false;
    out = r;
    return true;
}

std::vector<uint8_t> encodeGroupGrant(const GroupGrant& frame, size_t maxBytes, size_t& listedOut)
{
    std::vector<int> pieces = sortedUnique(frame.pieces);
    std::vector<uint8_t> header;
    header.push_back((uint8_t)GroupFileFrame::Grant);
    putU24(header, frame.fileId);
    header.push_back((uint8_t)frame.round);
    header.push_back(frame.flags);
    putCall(header, frame.call);
    size_t countAt = header.size();
    header.push_back(0);
    if (pieces.size() > 255) pieces.resize(255);
    std::vector<uint8_t> out = withPieceSet(header, pieces, maxBytes, listedOut);
    out[countAt] = (uint8_t)listedOut;
    return out;
}

bool decodeGroupGrant(const uint8_t* bytes, size_t length, GroupGrant& out)
{
    if (!isType(bytes, length, GroupFileFrame::Grant) || length < 10) return false;
    GroupGrant g;
    g.fileId = getU24(bytes + 1);
    g.round = bytes[4];
    g.flags = bytes[5];
    size_t at = 6;
    if (!getCall(bytes, length, at, g.call, false)) return false;
    if (at >= length) return false;
    size_t count = bytes[at++];
    if (!decodePieceSet(bytes + at, length - at, g.pieces) || g.pieces.size() != count) return false;
    out = g;
    return true;
}

std::vector<uint8_t> encodeGroupEnd(const GroupEnd& frame)
{
    std::vector<uint8_t> out;
    out.push_back((uint8_t)GroupFileFrame::End);
    putU24(out, frame.fileId);
    out.push_back((uint8_t)frame.reason);
    out.push_back((uint8_t)std::min(frame.rounds, 255));
    return out;
}

bool decodeGroupEnd(const uint8_t* bytes, size_t length, GroupEnd& out)
{
    if (!isType(bytes, length, GroupFileFrame::End) || length < 6) return false;
    out.fileId = getU24(bytes + 1);
    out.reason = (GroupFileEnd)bytes[4];
    out.rounds = bytes[5];
    return true;
}

uint32_t groupFileId(const std::string& call, const std::array<uint8_t, 32>& fileHash, uint64_t size)
{
    Sha256 hash;
    std::string c = commandCallsign(call);
    hash.update((const uint8_t*)c.data(), c.size());
    hash.update(fileHash.data(), fileHash.size());
    uint8_t sizeBytes[4] = {(uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size};
    hash.update(sizeBytes, 4);
    Sha256::Digest id = hash.finish();
    return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

//-------------------------------------------------------------------------
// Geometry and airtime
//-------------------------------------------------------------------------

bool isCpmMode(const ModeInfo& mode)
{
    return mode.name.compare(0, 3, "fsk") == 0;
}

int groupPieceBytes(const ModeInfo& mode)
{
    int pb = mode.bytesPerCodeword;
    if (pb <= 0) return 100;
    int frame;
    if (pb - LENGTH_BYTES >= (int)GROUP_FRAME_MAX_BYTES)
    {
        frame = (int)GROUP_FRAME_MAX_BYTES;
    }
    else
    {
        // Every codeword but the burst's control one may carry data.
        int most = std::max(1, mode.maxCodewords - 1);
        int k = isCpmMode(mode) ? 3 : ((int)GROUP_FRAME_MAX_BYTES + LENGTH_BYTES) / pb;
        k = std::min(std::max(k, 1), most);
        while (k > 1 && k * pb - LENGTH_BYTES > (int)GROUP_FRAME_MAX_BYTES) k--;
        frame = std::min(k * pb - LENGTH_BYTES, (int)GROUP_FRAME_MAX_BYTES);
    }
    return std::min(std::max(frame - DATA_HEADER_BYTES, 16), MAX_PIECE_BYTES);
}

int groupPiecesPerKeying(const ModeInfo& mode, int frameBytes)
{
    if (isCpmMode(mode) || !mode.valid()) return 1;
    int n = 1;
    while (n < MAX_PIECES_PER_KEYING)
    {
        int bytes = (n + 1) * (LENGTH_BYTES + frameBytes);
        int codewords = 1 + (bytes + mode.bytesPerCodeword - 1) / mode.bytesPerCodeword;
        if (codewords > mode.maxCodewords) break;
        if (mode.burstSeconds(std::vector<int>((size_t)n + 1, frameBytes)) > BURST_LIMIT_SECONDS) break;
        n++;
    }
    return n;
}

namespace
{

int requestBytes(const ModeInfo& mode)
{
    return std::min(std::max(mode.bytesPerCodeword - LENGTH_BYTES, 1), (int)GROUP_FRAME_MAX_BYTES);
}

double requestSeconds(const ModeInfo& mode)
{
    return mode.valid() ? mode.burstSeconds({requestBytes(mode)}) : 16.0;
}

// The keyings a first pass makes: Announces, pieces, the end of stream.
struct FirstPass
{
    double air = 0.0;
    int keyings = 0;
    double pieceKeyingSeconds = 0.0;  // the longest
};

FirstPass firstPass(uint64_t size, int pieceBytes, const ModeInfo& mode)
{
    FirstPass pass;
    if (!mode.valid() || pieceBytes <= 0) return pass;
    int pieces = (int)((size + (uint64_t)pieceBytes - 1) / (uint64_t)pieceBytes);
    int perKeying = groupPiecesPerKeying(mode, pieceBytes + DATA_HEADER_BYTES);
    int announcedBlock = -1;
    for (int start = 0; start < pieces; start += perKeying)
    {
        if (start / GroupFileEngine::ANNOUNCE_EVERY > announcedBlock)
        {
            announcedBlock = start / GroupFileEngine::ANNOUNCE_EVERY;
            pass.air += mode.burstSeconds({ANNOUNCE_ESTIMATE_BYTES});
            pass.keyings++;
        }
        std::vector<int> frames;
        for (int i = start; i < std::min(pieces, start + perKeying); i++)
        {
            uint64_t bytes = i == pieces - 1 ? size - (uint64_t)i * (uint64_t)pieceBytes : (uint64_t)pieceBytes;
            frames.push_back((int)bytes + DATA_HEADER_BYTES);
        }
        double seconds = mode.burstSeconds(frames);
        pass.pieceKeyingSeconds = std::max(pass.pieceKeyingSeconds, seconds);
        pass.air += seconds;
        pass.keyings++;
    }
    pass.air += mode.burstSeconds({WINDOW_BYTES});
    pass.keyings++;
    return pass;
}

uint64_t serviceMs(double firstPassAir)
{
    uint64_t air = (uint64_t)(firstPassAir * 1000.0);
    return std::min(std::max(air, GroupFileEngine::MIN_SERVICE_MS), GroupFileEngine::MAX_SERVICE_MS);
}

} // namespace

int groupSlotHalfSeconds(const ModeInfo& mode)
{
    double seconds = requestSeconds(mode) + 4.0;
    return std::min(255, (int)std::ceil(seconds * 2.0 - 1e-9));
}

GroupFileEstimate estimateGroupFile(uint64_t size, const ModeInfo& mode)
{
    GroupFileEstimate estimate;
    estimate.pieceBytes = groupPieceBytes(mode);
    estimate.pieces = (int)((size + (uint64_t)estimate.pieceBytes - 1) / (uint64_t)estimate.pieceBytes);
    FirstPass pass = firstPass(size, estimate.pieceBytes, mode);
    estimate.airSeconds = pass.air;
    estimate.wallSeconds = pass.air + std::max(0, pass.keyings - 1) * (GroupFileEngine::KEYING_GAP_MS / 1000.0);
    estimate.serviceSeconds = serviceMs(pass.air) / 1000.0;
    return estimate;
}

bool GroupFile::live() const
{
    switch (state)
    {
        case State::Sending:
        case State::Repairing:
        case State::Heard:
        case State::Receiving:
            return true;
        default:
            return false;
    }
}

//-------------------------------------------------------------------------
// The engine
//-------------------------------------------------------------------------

GroupFileEngine::GroupFileEngine()
    : random_(std::random_device{}())
{
    // empty
}

void GroupFileEngine::setMyCallsign(const std::string& callsign)
{
    myCall_ = commandCallsign(callsign);
}

void GroupFileEngine::setModes(const std::vector<ModeInfo>& offered)
{
    modes_ = offered;
}

void GroupFileEngine::setGear(int gear)
{
    gear_ = std::min(std::max(gear, 1), 5);
}

void GroupFileEngine::setRandomSeed(uint32_t seed)
{
    random_.seed(seed);
}

void GroupFileEngine::setAutoReceive(const std::string& folder)
{
    autoFolder_ = folder;
    if (!folder.empty()) return;

    // Turned off: what was being received without asking waits for the
    // operator's Receive again, and asks for nothing until then.
    for (Incoming& in : incoming_)
    {
        if (!in.autoReceive || in.state != GroupFile::State::Receiving) continue;
        in.autoReceive = false;
        in.state = GroupFile::State::Heard;
        in.pending = false;
        in.lateAtMs = 0;
        changed();
    }
}

ModeInfo GroupFileEngine::modeFor(int gear) const
{
    return modeForGear(gear, modes_);
}

GroupFileEngine::Outgoing* GroupFileEngine::liveOutgoing()
{
    for (Outgoing& out : outgoing_)
    {
        if (out.step != Step::Over) return &out;
    }
    return nullptr;
}

const GroupFileEngine::Outgoing* GroupFileEngine::liveOutgoing() const
{
    for (const Outgoing& out : outgoing_)
    {
        if (out.step != Step::Over) return &out;
    }
    return nullptr;
}

GroupFileEngine::Incoming* GroupFileEngine::findIncoming(uint32_t fileId)
{
    for (Incoming& in : incoming_)
    {
        if (in.fileId == fileId) return &in;
    }
    return nullptr;
}

GroupFileEngine::Incoming* GroupFileEngine::findIncomingById(uint64_t id)
{
    for (Incoming& in : incoming_)
    {
        if (in.id == id) return &in;
    }
    return nullptr;
}

void GroupFileEngine::setInhibited(bool inhibited)
{
    if (inhibited == inhibited_) return;
    inhibited_ = inhibited;
    if (!inhibited) return;

    // Nothing more can go: a file of ours ends here without an End, and
    // nobody is asked for.
    if (Outgoing* out = liveOutgoing()) finishOutgoing(*out, GroupFileEnd::Failed, "Sending is not allowed here.");
    for (Incoming& in : incoming_)
    {
        in.pending = false;
        in.lateAtMs = 0;
    }
    if (retry_.kind == KeyingKind::Request) retry_ = Taken();
}

void GroupFileEngine::holdUntil(uint64_t nowMs, uint64_t holdMs)
{
    holdUntilMs_ = std::max(holdUntilMs_, nowMs + holdMs);
}

uint64_t GroupFileEngine::send(const std::string& path, uint64_t nowMs, std::string& error)
{
    if (myCall_.empty())
    {
        error = "Set your callsign before sending a file.";
        return 0;
    }
    if (inhibited_)
    {
        error = "Sending is not allowed here.";
        return 0;
    }
    if (liveOutgoing() != nullptr)
    {
        error = "A file is already going to the group; one goes at a time.";
        return 0;
    }

    std::filesystem::path file = pathFromUtf8(path);
    std::error_code ec;
    uint64_t size = std::filesystem::is_regular_file(file, ec) ? std::filesystem::file_size(file, ec) : 0;
    if (ec || !std::filesystem::is_regular_file(file, ec))
    {
        error = "\"" + path + "\" is not a file that can be sent.";
        return 0;
    }
    if (size == 0)
    {
        error = "\"" + path + "\" is empty.";
        return 0;
    }
    if (size > GROUP_FILE_MAX_BYTES)
    {
        error = "A file for the group can be 64 KiB at most.";
        return 0;
    }

    ModeInfo mode = modeFor(gear_);
    GroupFileEstimate estimate = estimateGroupFile(size, mode);
    if (estimate.airSeconds > REFUSE_AIR_SECONDS)
    {
        error = "At this tempo the file would take over an hour on the air.";
        return 0;
    }

    std::vector<uint8_t> content(size);
    std::ifstream in(file, std::ios::binary);
    in.read((char*)content.data(), (std::streamsize)size);
    if (!in || (uint64_t)in.gcount() != size)
    {
        error = "\"" + path + "\" could not be read.";
        return 0;
    }

    Sha256::Digest digest = Sha256::of(content);
    Outgoing& out = outgoing_.emplace_back();
    out.id = nextId_++;
    out.content = std::move(content);
    out.size = size;
    std::copy(digest.begin(), digest.begin() + 8, out.hash.begin());
    out.fileId = groupFileId(myCall_, digest, size);
    out.path = path;
    out.name = truncateUtf8(utf8FromPath(file.filename()), GROUP_FILE_NAME_BYTES);
    if (out.name.empty()) out.name = "file";
    out.pieceBytes = estimate.pieceBytes;
    out.pieces = estimate.pieces;
    out.announcedTempo = gear_ - 1;
    out.firstPassAir = estimate.airSeconds;
    out.windowSlotHalfSeconds = groupSlotHalfSeconds(mode);
    (void)nowMs;
    changed();
    uint64_t id = out.id;
    prune();
    return id;
}

void GroupFileEngine::finishOutgoing(Outgoing& out, GroupFileEnd reason, const std::string& error)
{
    out.step = Step::Over;
    out.state = GroupFile::State::Ended;
    out.endReason = reason;
    if (!error.empty()) out.error = error;
    out.content.clear();
    out.content.shrink_to_fit();
    out.resend.clear();
    if (retry_.id == out.id) retry_ = Taken();
    changed();
}

void GroupFileEngine::startEnd(Outgoing& out, GroupFileEnd reason)
{
    out.step = Step::End;
    out.endReason = reason;
    out.endsLeft = 2;
    out.resend.clear();
    out.windowEndsMs = 0;
    changed();
}

bool GroupFileEngine::stopServing(uint64_t id)
{
    for (Outgoing& out : outgoing_)
    {
        if (out.id != id || out.step == Step::Over || out.step == Step::End) continue;
        out.stopAsked = true;
        bool nothingSent = out.nextPiece == 0 && out.announcedBlock < 0 && inFlight_.id != id;
        if (nothingSent) finishOutgoing(out, GroupFileEnd::Stopped);
        else if (out.step == Step::WindowOpen || out.step == Step::NextWindow) startEnd(out, GroupFileEnd::Stopped);
        else changed();
        return true;
    }
    return false;
}

bool GroupFileEngine::cancel(uint64_t id)
{
    for (Outgoing& out : outgoing_)
    {
        if (out.id != id || out.step == Step::Over) continue;
        if (out.step == Step::End && out.endReason == GroupFileEnd::Cancelled) return true;
        // Whatever of it waits is withdrawn; the End goes next.
        if (retry_.id == id) retry_ = Taken();
        bool nothingSent = out.nextPiece == 0 && out.announcedBlock < 0 && inFlight_.id != id;
        if (nothingSent) finishOutgoing(out, GroupFileEnd::Cancelled);
        else startEnd(out, GroupFileEnd::Cancelled);
        return true;
    }
    return ignore(id);
}

bool GroupFileEngine::receive(uint64_t id, const std::string& path, uint64_t nowMs, std::string& error)
{
    Incoming* in = findIncomingById(id);
    using State = GroupFile::State;
    if (in == nullptr || (in->state != State::Heard && in->state != State::Incomplete && in->state != State::Receiving))
    {
        error = "That file is no longer coming.";
        return false;
    }
    if (path.empty())
    {
        error = "No name to save it under.";
        return false;
    }
    in->path = path;
    in->autoReceive = false;
    in->error.clear();
    changed();
    // Over for now: it is received there when it is sent again.
    if (in->state == State::Incomplete) return true;
    in->state = State::Receiving;

    if (in->verified)
    {
        if (!save(*in))
        {
            error = in->error;
            return false;
        }
        return true;
    }

    // Ask in this window if a slot is still to come.
    if (in->round >= 0 && !in->ended) scheduleRequest(*in, nowMs, false);
    return true;
}

bool GroupFileEngine::ignore(uint64_t id)
{
    Incoming* in = findIncomingById(id);
    if (in == nullptr) return false;
    using State = GroupFile::State;
    if (in->state != State::Heard && in->state != State::Receiving && in->state != State::Incomplete) return false;
    endIncoming(*in, State::Ignored);
    return true;
}

std::vector<GroupFile> GroupFileEngine::files(uint64_t nowMs) const
{
    auto seconds = [nowMs](uint64_t atMs) -> int {
        if (atMs == 0 || atMs == NEVER || atMs <= nowMs) return 0;
        return (int)((atMs - nowMs + 999) / 1000);
    };

    std::vector<GroupFile> out;
    for (const Outgoing& o : outgoing_)
    {
        GroupFile f;
        f.id = o.id;
        f.outgoing = true;
        f.fileId = o.fileId;
        f.sender = myCall_;
        f.name = o.name;
        f.size = o.size;
        f.pieces = o.pieces;
        f.have = std::min(o.nextPiece, o.pieces);
        f.tempo = o.announcedTempo;
        f.state = o.state;
        f.round = o.round;
        f.path = o.path;
        f.error = o.error;
        f.endReason = o.endReason;
        f.resending = (int)o.granted.size();
        f.firstInLine = o.grantNamed;
        f.othersAsking = o.grantOthers;
        f.serviceSecondsLeft = o.deadlineMs == 0 ? -1 : seconds(o.deadlineMs);
        switch (o.step)
        {
            case Step::Stream:
            case Step::Done: f.phase = GroupFile::Phase::Streaming; break;
            case Step::WindowOpen:
                f.phase = GroupFile::Phase::WindowOpen;
                f.secondsLeftInPhase = seconds(o.windowEndsMs);
                break;
            case Step::Grant: f.phase = GroupFile::Phase::Resending; break;
            case Step::NextWindow:
                f.phase = GroupFile::Phase::Waiting;
                f.secondsLeftInPhase = seconds(o.nextWindowAtMs);
                break;
            case Step::End: f.phase = GroupFile::Phase::Ending; break;
            case Step::Over: f.phase = GroupFile::Phase::None; break;
        }
        for (const auto& [call, asked] : o.askers)
        {
            f.askers.push_back({call, asked.first, nowMs > asked.second ? (nowMs - asked.second) / 1000 : 0});
        }
        out.push_back(f);
    }

    for (const Incoming& in : incoming_)
    {
        GroupFile f;
        f.id = in.id;
        f.outgoing = false;
        f.fileId = in.fileId;
        f.sender = in.sender;
        f.name = in.name;
        f.size = in.geometry ? in.size : 0;
        f.pieces = in.geometry ? in.pieces : 0;
        f.have = in.verified ? in.pieces : (int)in.spool.size();
        f.tempo = in.tempo;
        f.state = in.state;
        f.round = std::max(in.round, 0);
        f.verified = in.verified;
        f.comingForUs = in.coming;
        f.firstInLine = in.firstInLine;
        f.autoReceived = in.autoReceive;
        f.path = in.path;
        f.error = in.error;
        f.serviceSecondsLeft = in.stopAtMs == 0 ? -1 : seconds(in.stopAtMs - AFTER_SERVICE_MS);
        if (in.pending)
        {
            f.slot = in.slot;
            f.slotInSeconds = seconds(in.sendAtMs);
        }
        out.push_back(f);
    }
    return out;
}

GroupFileEstimate GroupFileEngine::estimate(uint64_t size, int gear) const
{
    return estimateGroupFile(size, modeFor(gear));
}

//-------------------------------------------------------------------------
// Frames heard
//-------------------------------------------------------------------------

GroupFileEngine::Incoming& GroupFileEngine::incomingFor(uint32_t fileId, uint64_t nowMs)
{
    if (Incoming* in = findIncoming(fileId)) return *in;
    Incoming& in = incoming_.emplace_back();
    in.id = nextId_++;
    in.fileId = fileId;
    in.sender = "?";
    in.heardMs = nowMs;
    changed();
    limitSpool(nowMs);
    prune();
    return *findIncoming(fileId);
}

void GroupFileEngine::learnGeometry(Incoming& in, uint32_t size, int pieceBytes, int pieces, const Hash8& hash)
{
    if (in.geometry && in.size == size && in.pieceBytes == pieceBytes && in.hash == hash) return;
    if (in.geometry)
    {
        // Sent again at another tempo, so cut into pieces of another size:
        // what is known of the file is cut again the new way. Another file
        // under the same id starts over.
        bool sameFile = in.size == size && in.hash == hash;
        std::map<int, std::vector<uint8_t>> spool;
        if (sameFile)
        {
            std::vector<uint8_t> content(size);
            std::vector<bool> known(size, false);
            for (const auto& [index, bytes] : in.spool)
            {
                size_t from = (size_t)index * (size_t)in.pieceBytes;
                std::copy(bytes.begin(), bytes.end(), content.begin() + (long)from);
                std::fill(known.begin() + (long)from, known.begin() + (long)(from + bytes.size()), true);
            }
            for (int i = 0; i < pieces; i++)
            {
                size_t from = (size_t)i * (size_t)pieceBytes;
                size_t to = std::min((size_t)size, from + (size_t)pieceBytes);
                if (std::all_of(known.begin() + (long)from, known.begin() + (long)to, [](bool k) { return k; }))
                {
                    spool[i].assign(content.begin() + (long)from, content.begin() + (long)to);
                }
            }
        }
        else
        {
            in.verified = false;
            in.hashFailures = 0;
            in.restart = false;
        }
        in.spool = std::move(spool);
        in.size = size;
        in.pieceBytes = pieceBytes;
        in.pieces = pieces;
        in.hash = hash;
        in.askedByOthers.clear();
        in.coming = 0;
        changed();
        return;
    }
    in.geometry = true;
    in.size = size;
    in.pieceBytes = pieceBytes;
    in.pieces = pieces;
    in.hash = hash;

    // Pieces kept before it was known that don't belong.
    for (auto it = in.spool.begin(); it != in.spool.end();)
    {
        size_t expected = it->first == pieces - 1 ? size - (size_t)(pieces - 1) * (size_t)pieceBytes : (size_t)pieceBytes;
        if (it->first >= pieces || it->second.size() != expected) it = in.spool.erase(it);
        else ++it;
    }
    changed();
}

void GroupFileEngine::storePiece(Incoming& in, int index, const std::vector<uint8_t>& bytes)
{
    if (in.verified || in.spool.count(index) != 0) return;
    if (in.geometry)
    {
        if (index >= in.pieces) return;
        size_t expected =
            index == in.pieces - 1 ? in.size - (size_t)(in.pieces - 1) * (size_t)in.pieceBytes : (size_t)in.pieceBytes;
        if (bytes.size() != expected) return;
    }
    else
    {
        // Before the geometry is known, no more than a whole file's worth:
        // no piece larger than any Announce could name, and no more of them
        // than a file of the largest size at that size.
        if (bytes.size() > (size_t)MAX_PIECE_BYTES) return;
        if ((in.spool.size() + 1) * (size_t)MAX_PIECE_BYTES > GROUP_FILE_MAX_BYTES + (size_t)MAX_PIECE_BYTES) return;
    }
    in.spool[index] = bytes;
    changed();
}

std::vector<int> GroupFileEngine::missingOf(const Incoming& in) const
{
    std::vector<int> missing;
    if (!in.geometry || in.verified) return missing;
    for (int i = 0; i < in.pieces; i++)
    {
        if (in.spool.count(i) == 0) missing.push_back(i);
    }
    return missing;
}

void GroupFileEngine::checkComplete(Incoming& in, uint64_t nowMs)
{
    (void)nowMs;
    if (!in.geometry) return;
    if (!in.verified)
    {
        if ((int)in.spool.size() < in.pieces) return;

        Sha256 hash;
        uint64_t length = 0;
        for (const auto& [index, bytes] : in.spool)
        {
            hash.update(bytes);
            length += bytes.size();
        }
        Sha256::Digest digest = hash.finish();
        if (length != in.size || !std::equal(in.hash.begin(), in.hash.end(), digest.begin()))
        {
            // A piece that passed the modem's checks wrongly can't be told
            // apart: all of it again, once.
            in.spool.clear();
            in.hashFailures++;
            if (in.hashFailures >= 2)
            {
                endIncoming(in, GroupFile::State::FailedVerification);
                in.error = "The file came twice with the wrong checksum.";
            }
            else
            {
                in.restart = true;
                in.asked = false;
            }
            changed();
            return;
        }
        in.verified = true;
        in.pending = false;
        in.lateAtMs = 0;
        changed();
    }

    // Saved now if the operator has said where, or automatically once its
    // name is known.
    if (in.state == GroupFile::State::Receiving && (!in.autoReceive || in.announced)) save(in);
}

bool GroupFileEngine::save(Incoming& in)
{
    std::vector<uint8_t> content;
    for (const auto& [index, bytes] : in.spool) content.insert(content.end(), bytes.begin(), bytes.end());

    std::error_code ec;
    std::filesystem::path final;
    if (in.autoReceive)
    {
        if (autoFolder_.empty())
        {
            // No longer received without asking: the operator says where.
            in.autoReceive = false;
            in.state = GroupFile::State::Heard;
            in.pending = false;
            in.lateAtMs = 0;
            changed();
            return false;
        }
        std::filesystem::path folder = pathFromUtf8(autoFolder_);
        std::filesystem::create_directories(folder, ec);
        final = freeSavePath(folder, in.name.empty() ? std::string("received-file") : in.name);
    }
    else
    {
        final = pathFromUtf8(in.path);
    }

    auto fail = [&](const std::string& why) {
        in.error = why;
        endIncoming(in, GroupFile::State::Failed);
        return false;
    };
    if (final.empty()) return fail("No free name for \"" + in.name + "\" in \"" + autoFolder_ + "\".");

    // Written beside it first, so a file is never there half written.
    std::filesystem::path part;
    for (int n = 1; n < 1000 && part.empty(); n++)
    {
        std::filesystem::path candidate = final;
        if (n > 1) candidate += "." + std::to_string(n);
        candidate += ".part";
        if (!std::filesystem::exists(candidate, ec)) part = candidate;
    }
    if (part.empty()) return fail("No part file could be made beside \"" + utf8FromPath(final) + "\".");

    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write((const char*)content.data(), (std::streamsize)content.size());
        out.close();
        if (!out)
        {
            std::filesystem::remove(part, ec);
            return fail("\"" + utf8FromPath(part) + "\" could not be written.");
        }
    }
    // Only a name the operator chose in the save dialog, which asked,
    // replaces a file.
    if (!in.autoReceive && std::filesystem::exists(final, ec)) std::filesystem::remove(final, ec);
    std::filesystem::rename(part, final, ec);
    if (ec)
    {
        std::filesystem::remove(part, ec);
        return fail("\"" + utf8FromPath(final) + "\" could not be saved.");
    }

    in.path = utf8FromPath(final);
    endIncoming(in, GroupFile::State::Saved);
    return true;
}

void GroupFileEngine::endIncoming(Incoming& in, GroupFile::State state)
{
    using State = GroupFile::State;
    in.state = state;
    in.pending = false;
    in.lateAtMs = 0;
    if (state != State::Incomplete && state != State::Heard)
    {
        in.spool.clear();
        if (state != State::Saved) in.verified = false;
    }
    changed();
}

void GroupFileEngine::limitSpool(uint64_t nowMs)
{
    auto stillComing = [](const Incoming& in) {
        return (in.state == GroupFile::State::Heard || in.state == GroupFile::State::Receiving) && !in.ended;
    };
    (void)nowMs;
    for (;;)
    {
        size_t transfers = 0;
        size_t bytes = 0;
        Incoming* oldest = nullptr;
        for (Incoming& in : incoming_)
        {
            if (in.spool.empty()) continue;
            transfers++;
            for (const auto& [index, piece] : in.spool) bytes += piece.size();
            // The least lately heard goes first, one already over before one still coming.
            bool over = !stillComing(in);
            bool oldestOver = oldest != nullptr && !stillComing(*oldest);
            if (oldest == nullptr || (over && !oldestOver) || (over == oldestOver && in.heardMs < oldest->heardMs))
            {
                oldest = &in;
            }
        }
        if (oldest == nullptr || (transfers <= SPOOL_TRANSFERS && bytes <= SPOOL_BYTES)) return;
        oldest->spool.clear();
        if (oldest->state == GroupFile::State::Heard || oldest->state == GroupFile::State::Receiving)
        {
            endIncoming(*oldest, GroupFile::State::Incomplete);
        }
        oldest->verified = false;
        changed();
    }
}

void GroupFileEngine::onFrame(const uint8_t* bytes, size_t length, uint64_t nowMs)
{
    if (!isGroupFileFrame(bytes, length)) return;
    using State = GroupFile::State;

    // Ours, heard back: nothing for a listener.
    auto ours = [this](uint32_t fileId) {
        return std::any_of(outgoing_.begin(), outgoing_.end(), [&](const Outgoing& o) { return o.fileId == fileId; });
    };
    // Cancelled, or not saved or verified, and the sender's pass over: the
    // same file sent again is taken from the start.
    auto overForGood = [](const Incoming& in) {
        return in.ended &&
               (in.state == State::CancelledThere || in.state == State::Failed || in.state == State::FailedVerification);
    };
    // A transfer heard again after it ended (sent anew) is under way again,
    // with nothing of the last pass's windows.
    auto revive = [this, overForGood](Incoming& in) {
        if (in.state != State::Incomplete && !in.ended) return;
        if (overForGood(in))
        {
            in.spool.clear();
            in.verified = false;
            in.hashFailures = 0;
            in.restart = false;
            in.path.clear();
            in.error.clear();
            in.state = State::Incomplete;
        }
        in.ended = false;
        in.stopAtMs = 0;
        in.windowLengthMs = 0;
        in.round = -1;
        in.asked = false;
        in.pending = false;
        in.lateSent = false;
        in.lateAtMs = 0;
        in.askedByOthers.clear();
        in.firstInLine.clear();
        in.coming = 0;
        if (in.state == State::Incomplete)
        {
            in.autoReceive = in.path.empty() && !autoFolder_.empty();
            in.state = !in.path.empty() || in.autoReceive ? State::Receiving : State::Heard;
        }
        changed();
    };
    // Files received without asking start as soon as one is heard of.
    auto autoReceive = [this](Incoming& in) {
        if (in.state != State::Heard || autoFolder_.empty()) return;
        in.state = State::Receiving;
        in.autoReceive = true;
        changed();
    };
    auto taking = [overForGood](const Incoming& in) {
        return in.state == State::Heard || in.state == State::Receiving || in.state == State::Incomplete ||
               overForGood(in);
    };

    switch ((GroupFileFrame)bytes[0])
    {
        case GroupFileFrame::Announce:
        {
            GroupAnnounce a;
            if (!decodeGroupAnnounce(bytes, length, a) || ours(a.fileId) || a.call == myCall_) return;
            Incoming& in = incomingFor(a.fileId, nowMs);
            in.heardMs = nowMs;
            in.lateSent = false;
            if (!taking(in)) return;
            revive(in);
            in.sender = a.call;
            in.tempo = a.tempo;
            if (!in.announced)
            {
                in.announced = true;
                in.name = safeFileName(a.name);
                changed();
            }
            learnGeometry(in, a.size, a.pieceBytes, a.pieces, a.hash);
            autoReceive(in);
            checkComplete(in, nowMs);
            break;
        }
        case GroupFileFrame::Data:
        {
            GroupData d;
            if (!decodeGroupData(bytes, length, d) || ours(d.fileId)) return;
            Incoming& in = incomingFor(d.fileId, nowMs);
            in.heardMs = nowMs;
            in.lateSent = false;
            if (!taking(in)) return;
            revive(in);
            storePiece(in, d.index, d.bytes);
            checkComplete(in, nowMs);
            break;
        }
        case GroupFileFrame::Window:
        {
            GroupWindow w;
            if (!decodeGroupWindow(bytes, length, w) || ours(w.fileId)) return;
            Incoming& in = incomingFor(w.fileId, nowMs);
            in.heardMs = nowMs;
            in.lateSent = false;
            in.lateAtMs = 0;
            if (!taking(in)) return;
            revive(in);
            learnGeometry(in, w.size, w.pieceBytes, w.pieces, w.hash);
            autoReceive(in);
            in.windowMs = nowMs;
            in.slots = w.slots;
            in.slotMs = (uint64_t)w.slotHalfSeconds * 500;
            in.windowLengthMs = in.slots * in.slotMs + WINDOW_GUARD_MS;
            in.stopAtMs = nowMs + (uint64_t)w.serviceLeftTens * 10000 + AFTER_SERVICE_MS;
            in.priority = w.priorityCall;
            in.windowTempo = w.tempo;
            if (w.round != in.round)
            {
                in.round = w.round;
                in.askedByOthers.clear();
                in.asked = false;
                in.pending = false;
                in.firstInLine.clear();
                in.coming = 0;
            }
            changed();
            checkComplete(in, nowMs);
            // Pieces missing, or all of it but the name, to be saved
            // without asking: asked for.
            if (in.state == State::Receiving && (!in.verified || !in.announced)) scheduleRequest(in, nowMs, true);
            break;
        }
        case GroupFileFrame::Request:
        {
            GroupRequest r;
            if (!decodeGroupRequest(bytes, length, r)) return;
            if (ours(r.fileId))
            {
                onRequest(r, nowMs);
                return;
            }
            Incoming* in = findIncoming(r.fileId);
            if (in == nullptr || r.call == myCall_) return;
            in->heardMs = nowMs;
            in->lateSent = false;
            // Pieces somebody else asked for this round are left out of ours.
            if (r.round == in->round) in->askedByOthers.insert(r.pieces.begin(), r.pieces.end());
            break;
        }
        case GroupFileFrame::Grant:
        {
            GroupGrant g;
            if (!decodeGroupGrant(bytes, length, g) || ours(g.fileId)) return;
            Incoming* in = findIncoming(g.fileId);
            if (in == nullptr) return;
            in->heardMs = nowMs;
            in->lateSent = false;
            if (in->pending)
            {
                // The window is over: too late for this round.
                in->pending = false;
                in->asked = true;
            }
            in->firstInLine = g.call;
            in->coming = 0;
            for (int piece : g.pieces)
            {
                if (in->geometry && piece < in->pieces && in->spool.count(piece) == 0 && !in->verified) in->coming++;
            }
            changed();
            break;
        }
        case GroupFileFrame::End:
        {
            GroupEnd e;
            if (!decodeGroupEnd(bytes, length, e) || ours(e.fileId)) return;
            Incoming* in = findIncoming(e.fileId);
            if (in == nullptr) return;
            if (in->state == State::Failed || in->state == State::FailedVerification || in->state == State::CancelledThere)
            {
                in->ended = true; // so the next time it is sent is taken afresh
                return;
            }
            if (!taking(*in)) return;
            in->heardMs = nowMs;
            in->ended = true;
            if (e.reason == GroupFileEnd::Cancelled)
            {
                endIncoming(*in, State::CancelledThere);
                return;
            }
            if (!in->verified) endIncoming(*in, State::Incomplete);
            else if (in->state == State::Receiving)
            {
                // All of it, and no name coming now: saved under one of its own.
                save(*in);
            }
            else
            {
                in->pending = false;
                changed();
            }
            break;
        }
    }
}

void GroupFileEngine::onRequest(const GroupRequest& request, uint64_t nowMs)
{
    Outgoing* out = nullptr;
    for (Outgoing& o : outgoing_)
    {
        if (o.fileId == request.fileId && o.step != Step::Over && o.step != Step::End) out = &o;
    }
    // After the End, and before the stream is over, nothing is taken.
    if (out == nullptr || out->step == Step::Stream || request.call.empty() || request.call == myCall_) return;
    if (request.round != out->round && request.round != ROUND_LATE) return;

    std::vector<int> pieces;
    for (int piece : request.pieces)
    {
        if (piece < out->pieces) pieces.push_back(piece);
    }

    Ask& ask = out->asks[request.call];
    bool fresh = ask.call.empty();
    ask.call = request.call;
    ask.pieces = pieces;
    ask.missingTotal = request.missingTotal;
    ask.flags = request.flags;
    if (fresh) ask.seq = out->askSeq++;
    out->lostHints.erase(request.call);
    out->askers[request.call] = {request.missingTotal, nowMs};

    // One asking between windows, after a quiet one: a window at once.
    if (request.round == ROUND_LATE && out->step == Step::NextWindow)
    {
        out->nextWindowAtMs = nowMs;
        out->quiet = 0;
    }
    changed();
}

void GroupFileEngine::onHeardLost(const std::string& call, int lost, uint64_t nowMs)
{
    (void)nowMs;
    std::string c = commandCallsign(call);
    if (lost <= 0 || c.empty() || c == myCall_) return;
    Outgoing* out = liveOutgoing();
    if (out == nullptr || out->step != Step::WindowOpen || out->asks.count(c) != 0) return;
    // Twice at most for a station, so somebody else's chat heard badly
    // can't hold windows open.
    if (out->hinted[c] >= 2) return;
    out->hinted[c]++;
    out->lostHints.insert(c);
}

//-------------------------------------------------------------------------
// Requests
//-------------------------------------------------------------------------

void GroupFileEngine::scheduleRequest(Incoming& in, uint64_t nowMs, bool fromSlotZero)
{
    if (in.asked || in.pending || in.slots < 1 || in.slotMs == 0 || inhibited_) return;

    int first = 0;
    if (!fromSlotZero)
    {
        // Only a slot that has yet to start.
        uint64_t into = nowMs > in.windowMs ? nowMs - in.windowMs : 0;
        first = (int)((into + in.slotMs - 1) / in.slotMs);
    }
    int slot;
    if (in.priority == myCall_ && !myCall_.empty())
    {
        slot = 0; // named for slot 0
    }
    else
    {
        int low = in.priority.empty() || in.slots < 2 ? 0 : 1;
        low = std::max(low, first);
        if (low >= in.slots) return;
        slot = low + (int)(random_() % (uint32_t)(in.slots - low));
    }
    if (slot < first) return;

    in.slot = slot;
    in.sendAtMs = in.windowMs + (uint64_t)slot * in.slotMs + random_() % (JITTER_MS + 1);
    in.slotEndMs = in.windowMs + (uint64_t)(slot + 1) * in.slotMs;
    in.pending = true;
    changed();
}

bool GroupFileEngine::buildRequest(Incoming& in, int round, std::vector<uint8_t>& frame)
{
    std::vector<int> missing = missingOf(in);
    std::vector<int> listed;
    for (int piece : missing)
    {
        if (in.askedByOthers.count(piece) == 0) listed.push_back(piece);
    }

    GroupRequest request;
    request.fileId = in.fileId;
    request.round = round;
    request.call = myCall_;
    request.missingTotal = (int)missing.size();
    request.pieces = listed;
    if (!in.announced) request.flags |= REQUEST_NEEDS_ANNOUNCE;
    if (in.restart) request.flags |= REQUEST_RESTART;
    // Nothing left that nobody else has asked for: silent.
    if (listed.empty() && (request.flags & REQUEST_NEEDS_ANNOUNCE) == 0) return false;

    int tempo = in.windowTempo >= 0 ? in.windowTempo : in.tempo;
    ModeInfo mode = modeFor(tempo + 1);
    size_t header = 10 + myCall_.size();
    size_t maxBytes = std::max((size_t)requestBytes(mode), header + 4);
    size_t count = 0;
    frame = encodeGroupRequest(request, maxBytes, count);
    in.restart = false;
    return true;
}

//-------------------------------------------------------------------------
// The sender
//-------------------------------------------------------------------------

std::vector<uint8_t> GroupFileEngine::announceFrame(const Outgoing& out) const
{
    GroupAnnounce a;
    a.fileId = out.fileId;
    a.size = (uint32_t)out.content.size();
    a.pieceBytes = out.pieceBytes;
    a.pieces = out.pieces;
    a.hash = out.hash;
    a.tempo = gear_ - 1;
    a.airSeconds = (int)std::lround(out.firstPassAir);
    a.call = myCall_;
    a.name = out.name;
    return encodeGroupAnnounce(a);
}

std::vector<uint8_t> GroupFileEngine::dataFrame(const Outgoing& out, int index) const
{
    GroupData d;
    d.fileId = out.fileId;
    d.index = index;
    size_t from = (size_t)index * (size_t)out.pieceBytes;
    size_t to = std::min(out.content.size(), from + (size_t)out.pieceBytes);
    d.bytes.assign(out.content.begin() + (long)from, out.content.begin() + (long)to);
    return encodeGroupData(d);
}

std::vector<uint8_t> GroupFileEngine::windowFrame(const Outgoing& out, uint64_t nowMs) const
{
    GroupWindow w;
    w.fileId = out.fileId;
    w.round = out.round;
    w.size = (uint32_t)out.content.size();
    w.pieceBytes = out.pieceBytes;
    w.pieces = out.pieces;
    w.hash = out.hash;
    w.slots = out.windowSlots;
    w.slotHalfSeconds = out.windowSlotHalfSeconds;
    uint64_t left = out.deadlineMs > nowMs ? out.deadlineMs - nowMs : 0;
    w.serviceLeftTens = (int)std::min<uint64_t>(left / 10000, 0xFFFF);
    w.priorityCall = out.priority;
    w.tempo = gear_ - 1;
    return encodeGroupWindow(w);
}

// The next window's slots: more for more stations, and two more for each
// that seems to have asked and not been heard.
void GroupFileEngine::closeWindow(Outgoing& out, uint64_t nowMs)
{
    if (out.stopAsked)
    {
        startEnd(out, GroupFileEnd::Stopped);
        return;
    }
    if (out.deadlineMs != 0 && nowMs >= out.deadlineMs)
    {
        startEnd(out, GroupFileEnd::Deadline);
        return;
    }
    if (!out.asks.empty())
    {
        out.quiet = 0;
        elect(out);
        out.step = Step::Grant;
        out.grantSent = false;
        changed();
        return;
    }
    if (!out.lostHints.empty())
    {
        // Somebody may have asked and not been heard: again at once, with
        // room for them.
        out.quiet = 0;
        out.step = Step::NextWindow;
        out.nextWindowAtMs = nowMs;
        changed();
        return;
    }

    out.quiet++;
    if (out.quiet >= QUIET_WINDOWS_TO_END)
    {
        startEnd(out, GroupFileEnd::Quiet);
        return;
    }
    uint64_t lastWindow = (uint64_t)out.windowSlots * (uint64_t)out.windowSlotHalfSeconds * 500 + WINDOW_GUARD_MS;
    out.step = Step::NextWindow;
    out.nextWindowAtMs = out.quiet == 1 ? nowMs : nowMs + lastWindow;
    changed();
}

void GroupFileEngine::elect(Outgoing& out)
{
    std::vector<Ask> order;
    for (const auto& [call, ask] : out.asks) order.push_back(ask);
    std::sort(order.begin(), order.end(), [](const Ask& a, const Ask& b) { return a.seq < b.seq; });

    // Turns go round: last round's station goes to the back when others
    // wait, and so, while others wait, does one named again and again
    // that gets no further.
    if (order.size() > 1)
    {
        auto stuck = [&](const Ask& a) {
            auto it = out.named.find(a.call);
            return a.call == out.lastNamed ||
                   (it != out.named.end() && it->second.first >= TIMES_NAMED_WITHOUT_PROGRESS &&
                    a.missingTotal >= it->second.second);
        };
        std::stable_partition(order.begin(), order.end(), [&](const Ask& a) { return !stuck(a); });
    }
    const Ask& first = order.front();
    auto& record = out.named[first.call];
    if (record.first > 0 && first.missingTotal >= record.second) record.first++;
    else record.first = 1;
    record.second = first.missingTotal;

    // Its pieces first, then everybody else's, the most asked for first.
    std::vector<int> set = sortedUnique(first.pieces);
    std::set<int> chosen(set.begin(), set.end());
    std::map<int, int> askers;
    for (size_t i = 1; i < order.size(); i++)
    {
        for (int piece : sortedUnique(order[i].pieces))
        {
            if (chosen.count(piece) == 0) askers[piece]++;
        }
    }
    std::vector<std::pair<int, int>> others(askers.begin(), askers.end());
    std::stable_sort(others.begin(), others.end(),
                     [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.second > b.second; });
    for (const auto& [piece, count] : others) set.push_back(piece);

    // No more than RESEND_CAP_SECONDS of airtime in a round.
    ModeInfo mode = modeFor(gear_);
    int frameBytes = out.pieceBytes + DATA_HEADER_BYTES;
    int perKeying = groupPiecesPerKeying(mode, frameBytes);
    double keying = mode.valid() ? mode.burstSeconds(std::vector<int>((size_t)perKeying, frameBytes)) : 40.0;
    double perPiece = (keying + KEYING_GAP_MS / 1000.0) / perKeying;
    size_t cap = (size_t)std::max(1.0, std::floor(RESEND_CAP_SECONDS / perPiece));
    cap = std::min<size_t>(cap, 255);
    if (set.size() > cap) set.resize(cap);

    // And only what the Grant can list.
    GroupGrant grant;
    grant.pieces = set;
    grant.call = first.call;
    size_t listed = 0;
    encodeGroupGrant(grant, GROUP_FRAME_MAX_BYTES, listed);
    if (listed < set.size())
    {
        std::vector<int> ascending = sortedUnique(set);
        ascending.resize(listed);
        std::set<int> keep(ascending.begin(), ascending.end());
        set.erase(std::remove_if(set.begin(), set.end(), [&](int p) { return keep.count(p) == 0; }), set.end());
    }

    out.resend.assign(set.begin(), set.end());
    out.granted = set;
    out.grantNamed = first.call;
    out.grantOthers = 0;
    out.announceFollows = false;
    for (size_t i = 0; i < order.size(); i++)
    {
        if ((order[i].flags & REQUEST_NEEDS_ANNOUNCE) != 0) out.announceFollows = true;
        if (i == 0) continue;
        bool served = std::any_of(order[i].pieces.begin(), order[i].pieces.end(),
                                  [&](int p) { return std::find(set.begin(), set.end(), p) != set.end(); });
        if (served) out.grantOthers++;
    }
    out.lastNamed = first.call;
    out.lastStations = (int)order.size();
    out.asks.clear();
    // The hints stay for the next Window: its extra slots, and slot 0.
}

bool GroupFileEngine::takeSenderKeying(Outgoing& out, uint64_t nowMs, Taken& taken)
{
    taken.id = out.id;
    ModeInfo mode = modeFor(gear_);
    int frameBytes = out.pieceBytes + DATA_HEADER_BYTES;

    auto pieces = [&](std::deque<int>* queue) {
        int perKeying = groupPiecesPerKeying(mode, frameBytes);
        taken.kind = KeyingKind::Pieces;
        for (int i = 0; i < perKeying; i++)
        {
            int index;
            if (queue != nullptr)
            {
                if (queue->empty()) break;
                index = queue->front();
                queue->pop_front();
            }
            else
            {
                if (out.nextPiece >= out.pieces) break;
                index = out.nextPiece++;
            }
            taken.frames.push_back(dataFrame(out, index));
        }
        changed();
    };

    auto window = [&]() {
        out.windowSlotHalfSeconds = groupSlotHalfSeconds(mode);
        taken.kind = KeyingKind::Window;
        taken.frames.push_back(windowFrame(out, nowMs));
        out.priority.clear();
        out.step = Step::WindowOpen;
        out.windowEndsMs = NEVER; // opens once it has gone out
        changed();
    };

    switch (out.step)
    {
        case Step::End:
        {
            GroupEnd end;
            end.fileId = out.fileId;
            end.reason = out.endReason;
            end.rounds = out.round;
            taken.kind = KeyingKind::End;
            taken.endReason = out.endReason;
            taken.frames.push_back(encodeGroupEnd(end));
            return true;
        }
        case Step::Stream:
        {
            if (out.nextPiece < out.pieces)
            {
                int block = out.nextPiece / ANNOUNCE_EVERY;
                if (block > out.announcedBlock)
                {
                    out.announcedBlock = block;
                    taken.kind = KeyingKind::Announce;
                    taken.frames.push_back(announceFrame(out));
                    return true;
                }
                pieces(nullptr);
                if (out.nextPiece >= out.pieces) out.step = Step::Done;
                return true;
            }
            out.step = Step::Done;
            [[fallthrough]];
        }
        case Step::Done:
        {
            if (out.stopAsked)
            {
                startEnd(out, GroupFileEnd::Stopped);
                return takeSenderKeying(out, nowMs, taken);
            }
            // The end of the stream opens the first window, and the service
            // time runs from here.
            out.state = GroupFile::State::Repairing;
            out.round = 0;
            out.windowSlots = FIRST_SLOTS;
            out.deadlineMs = nowMs + serviceMs(out.firstPassAir);
            window();
            return true;
        }
        case Step::Grant:
        {
            if (out.stopAsked)
            {
                startEnd(out, GroupFileEnd::Stopped);
                return takeSenderKeying(out, nowMs, taken);
            }
            if (!out.grantSent)
            {
                GroupGrant grant;
                grant.fileId = out.fileId;
                grant.round = out.round;
                grant.flags = out.announceFollows ? GRANT_ANNOUNCE_FOLLOWS : 0;
                grant.call = out.grantNamed;
                grant.pieces = out.granted;
                size_t listed = 0;
                taken.kind = KeyingKind::Grant;
                taken.frames.push_back(encodeGroupGrant(grant, GROUP_FRAME_MAX_BYTES, listed));
                out.grantSent = true;
                changed();
                return true;
            }
            if (out.announceFollows)
            {
                out.announceFollows = false;
                taken.kind = KeyingKind::Announce;
                taken.frames.push_back(announceFrame(out));
                return true;
            }
            if (!out.resend.empty())
            {
                pieces(&out.resend);
                return true;
            }
            // Every piece of the round has gone: the next window.
            out.step = Step::NextWindow;
            out.nextWindowAtMs = nowMs;
            [[fallthrough]];
        }
        case Step::NextWindow:
        {
            if (nowMs < out.nextWindowAtMs) return false;
            if (out.stopAsked)
            {
                startEnd(out, GroupFileEnd::Stopped);
                return takeSenderKeying(out, nowMs, taken);
            }
            if ((out.deadlineMs != 0 && nowMs >= out.deadlineMs) || out.round >= 254)
            {
                startEnd(out, GroupFileEnd::Deadline);
                return takeSenderKeying(out, nowMs, taken);
            }
            out.round++;
            int fromStations = (int)std::ceil(1.5 * out.lastStations) + 1;
            out.windowSlots = std::min(std::max(fromStations, MIN_SLOTS), MAX_SLOTS_FROM_STATIONS);
            out.windowSlots = std::min(out.windowSlots + 2 * (int)out.lostHints.size(), MAX_SLOTS);
            // A station that seems to have asked unheard gets slot 0.
            out.priority = out.lostHints.empty() ? std::string() : *out.lostHints.begin();
            out.lostHints.clear();
            out.granted.clear();
            out.grantNamed.clear();
            out.grantOthers = 0;
            window();
            return true;
        }
        case Step::WindowOpen:
        case Step::Over:
            return false;
    }
    return false;
}

//-------------------------------------------------------------------------
// Keyings
//-------------------------------------------------------------------------

int GroupFileEngine::dueGear(uint64_t nowMs) const
{
    if (inhibited_ || inFlight_.kind != KeyingKind::None || nowMs < nextKeyingMs_ || nowMs < holdUntilMs_) return 0;
    if (myCall_.empty()) return 0;
    if (retry_.kind != KeyingKind::None) return retry_.gear;

    const Outgoing* out = liveOutgoing();
    // A cancel goes ahead of everything.
    if (out != nullptr && out->step == Step::End && out->endReason == GroupFileEnd::Cancelled) return gear_;

    // A request whose time has come.
    for (const Incoming& in : incoming_)
    {
        bool due = (in.pending && nowMs >= in.sendAtMs) || (in.lateAtMs != 0 && nowMs >= in.lateAtMs);
        if (due && in.state == GroupFile::State::Receiving)
        {
            int tempo = in.windowTempo >= 0 ? in.windowTempo : in.tempo;
            return tempo + 1;
        }
    }

    if (out == nullptr) return 0;
    switch (out->step)
    {
        case Step::Stream:
        case Step::Done:
        case Step::Grant:
        case Step::End:
            return gear_;
        case Step::NextWindow:
            return nowMs >= out->nextWindowAtMs ? gear_ : 0;
        default:
            return 0;
    }
}

bool GroupFileEngine::takeKeying(uint64_t nowMs, std::vector<std::vector<uint8_t>>& framesOut)
{
    framesOut.clear();
    int gear = dueGear(nowMs);
    if (gear == 0) return false;

    if (retry_.kind != KeyingKind::None)
    {
        inFlight_ = retry_;
        retry_ = Taken();
        framesOut = inFlight_.frames;
        return true;
    }

    Taken taken;
    taken.gear = gear;
    Outgoing* out = liveOutgoing();
    if (out != nullptr && out->step == Step::End && out->endReason == GroupFileEnd::Cancelled)
    {
        takeSenderKeying(*out, nowMs, taken);
    }
    else
    {
        for (Incoming& in : incoming_)
        {
            if (in.state != GroupFile::State::Receiving) continue;
            bool inSlot = in.pending && nowMs >= in.sendAtMs;
            bool late = in.lateAtMs != 0 && nowMs >= in.lateAtMs;
            if (!inSlot && !late) continue;
            int round = inSlot ? in.round : ROUND_LATE;
            if (inSlot)
            {
                in.pending = false;
                in.asked = true;
            }
            else
            {
                in.lateAtMs = 0;
                in.lateSent = true;
            }
            changed();
            std::vector<uint8_t> frame;
            if (!buildRequest(in, round, frame)) return false; // nothing to ask: silent
            taken.kind = KeyingKind::Request;
            taken.id = in.id;
            taken.frames.push_back(frame);
            break;
        }
        if (taken.kind == KeyingKind::None && out != nullptr) takeSenderKeying(*out, nowMs, taken);
    }

    if (taken.kind == KeyingKind::None || taken.frames.empty()) return false;
    inFlight_ = taken;
    framesOut = taken.frames;
    return true;
}

void GroupFileEngine::keyingSent(uint64_t nowMs)
{
    Taken taken = inFlight_;
    inFlight_ = Taken();
    if (taken.kind == KeyingKind::None) return;
    nextKeyingMs_ = nowMs + YIELD_MS;

    for (Outgoing& out : outgoing_)
    {
        if (out.id != taken.id || out.step == Step::Over) continue;
        if (taken.kind == KeyingKind::Window && out.step == Step::WindowOpen && out.windowEndsMs == NEVER)
        {
            // The window opens as the Window leaves us: listeners hear it at
            // the same moment, once the burst is decoded.
            out.windowEndsMs = nowMs + (uint64_t)out.windowSlots * (uint64_t)out.windowSlotHalfSeconds * 500 +
                               WINDOW_GUARD_MS;
            changed();
        }
        else if (taken.kind == KeyingKind::End && out.step == Step::End && taken.endReason == out.endReason)
        {
            // An End for an earlier reason (a cancel came while it was on
            // the air) does not count towards the two of the new reason.
            if (--out.endsLeft <= 0) finishOutgoing(out, out.endReason);
        }
    }
}

void GroupFileEngine::keyingLost(uint64_t nowMs)
{
    (void)nowMs;
    Taken taken = inFlight_;
    inFlight_ = Taken();
    if (taken.kind == KeyingKind::None || taken.kind == KeyingKind::Request) return;

    if (!taken.retried)
    {
        taken.retried = true;
        retry_ = taken;
        return;
    }
    for (Outgoing& out : outgoing_)
    {
        if (out.id != taken.id || out.step == Step::Over) continue;
        if (taken.kind == KeyingKind::End)
        {
            // A cancel since then still has its own Ends to send.
            if (out.step != Step::End || taken.endReason == out.endReason) finishOutgoing(out, out.endReason);
        }
        else finishOutgoing(out, GroupFileEnd::Failed, "data2g-host did not send it.");
    }
}

bool GroupFileEngine::wantsGroup() const
{
    if (retry_.kind != KeyingKind::None || inFlight_.kind != KeyingKind::None) return true;
    const Outgoing* out = liveOutgoing();
    if (out != nullptr && out->step != Step::WindowOpen) return true;
    return std::any_of(incoming_.begin(), incoming_.end(),
                       [](const Incoming& in) { return in.pending || in.lateAtMs != 0; });
}

void GroupFileEngine::stopAll(const std::string& why)
{
    if (Outgoing* out = liveOutgoing()) finishOutgoing(*out, GroupFileEnd::Failed, why);
    inFlight_ = Taken();
    retry_ = Taken();
    // Nothing more will be heard: what was coming ends as it would on the
    // sender going quiet, its pieces kept a while for a later pass.
    using State = GroupFile::State;
    for (Incoming& in : incoming_)
    {
        in.pending = false;
        in.lateAtMs = 0;
        if ((in.state != State::Heard && in.state != State::Receiving) || in.ended) continue;
        in.ended = true;
        if (!in.verified) endIncoming(in, State::Incomplete);
        else if (in.state == State::Receiving) save(in);
        changed();
    }
}

void GroupFileEngine::tick(uint64_t nowMs, bool busy, bool held)
{
    uint64_t elapsed = lastTickMs_ != 0 && nowMs > lastTickMs_ ? nowMs - lastTickMs_ : 0;
    lastTickMs_ = nowMs;

    // Our yield runs on while the channel is busy, and a little after.
    if (busy && nowMs < nextKeyingMs_) nextKeyingMs_ = std::max(nextKeyingMs_, nowMs + AFTER_BUSY_MS);

    for (Outgoing& out : outgoing_)
    {
        if (out.step == Step::Over) continue;
        // A session holding the group stops the sender's clocks.
        if (held && elapsed > 0)
        {
            if (out.deadlineMs != 0) out.deadlineMs += elapsed;
            if (out.step == Step::WindowOpen && out.windowEndsMs != NEVER) out.windowEndsMs += elapsed;
            if (out.step == Step::NextWindow) out.nextWindowAtMs += elapsed;
        }
        if (out.step == Step::WindowOpen && out.windowEndsMs != NEVER && nowMs >= out.windowEndsMs)
        {
            closeWindow(out, nowMs);
        }
        else if (out.step == Step::NextWindow && out.deadlineMs != 0 && nowMs >= out.deadlineMs)
        {
            startEnd(out, GroupFileEnd::Deadline);
        }
    }

    using State = GroupFile::State;
    for (Incoming& in : incoming_)
    {
        if (in.state == State::Incomplete && !in.spool.empty() && nowMs - in.heardMs >= SPOOL_KEEP_MS)
        {
            in.spool.clear();
            changed();
        }
        // The sender gone quiet for good, or its service time over.
        uint64_t window = in.windowLengthMs != 0 ? in.windowLengthMs : 30000;
        uint64_t idle = std::max<uint64_t>(10 * 60 * 1000, 2 * (window + (uint64_t)(RESEND_CAP_SECONDS * 1000)));
        bool stalled = nowMs - in.heardMs >= idle || (in.stopAtMs != 0 && nowMs >= in.stopAtMs);

        // Not saved or verified, and that pass is over: sent again, it is
        // taken afresh.
        if ((in.state == State::Failed || in.state == State::FailedVerification) && stalled) in.ended = true;
        if (in.state != State::Heard && in.state != State::Receiving) continue;

        // A slot gone by unused: withdrawn, to ask again next round.
        if (in.pending && nowMs >= in.slotEndMs)
        {
            in.pending = false;
            in.asked = true;
            changed();
        }

        // Heard nothing of it for a while: maybe the Window was lost.
        // Asked once, unbidden, at a random moment.
        if (in.state == State::Receiving && in.geometry && !in.verified && !in.ended && !in.lateSent &&
            in.lateAtMs == 0 && !in.pending && !inhibited_)
        {
            ModeInfo mode = modeFor(in.tempo + 1);
            double keying = mode.valid() ? mode.burstSeconds({in.pieceBytes + DATA_HEADER_BYTES}) : 40.0;
            uint64_t quiet = std::max<uint64_t>(LOST_WINDOW_MIN_MS, (uint64_t)(3000.0 * (keying + 5.0)));
            if (nowMs - in.heardMs >= quiet) in.lateAtMs = nowMs + random_() % (LATE_REQUEST_SPREAD_MS + 1);
        }

        if (stalled && !in.ended)
        {
            in.ended = true;
            if (!in.verified) endIncoming(in, State::Incomplete);
            else if (in.state == State::Receiving)
            {
                // All of it, and no name coming now: saved under one of its own.
                save(in);
            }
            else
            {
                in.pending = false;
                in.lateAtMs = 0;
                changed();
            }
        }
    }
}

void GroupFileEngine::prune()
{
    auto trim = [](auto& list, auto isLive) {
        size_t finished = (size_t)std::count_if(list.begin(), list.end(), [&](const auto& e) { return !isLive(e); });
        for (auto it = list.begin(); it != list.end() && finished > FINISHED_KEPT;)
        {
            if (isLive(*it))
            {
                ++it;
                continue;
            }
            it = list.erase(it);
            finished--;
        }
    };
    trim(outgoing_, [](const Outgoing& o) { return o.step != Step::Over; });
    trim(incoming_, [](const Incoming& in) {
        return in.state == GroupFile::State::Heard || in.state == GroupFile::State::Receiving ||
               (in.state == GroupFile::State::Incomplete && !in.spool.empty());
    });
}

} // namespace Data2G
} // namespace TextMessaging
