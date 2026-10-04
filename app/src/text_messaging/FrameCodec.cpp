//=========================================================================
// Name:            FrameCodec.cpp
// Purpose:         Packs and parses text messaging frames for the modem.
//
// Authors:         FreeDV text messaging contributors
// License:
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//=========================================================================

#include "FrameCodec.h"

#include <cctype>
#include <cstring>

#include "HamText.h"

namespace TextMessaging
{

namespace
{

// Air alphabet for callsigns that are not standard. Index zero is the pad
// character, so a short callsign packs to the same value whichever end does
// the packing.
const char* const BASE40_ALPHABET = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-/.";
constexpr int BASE40_SIZE = 40;

// A standard callsign as FT8 packs it: six positions, the call area digit
// third (a one character prefix is shifted right by a space), each position
// from its own alphabet.
const char* const STANDARD_ALPHABETS[6] = {
    " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0123456789",
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ",
};

// CRC-24/OPENPGP, the same parameters FreeDATA uses for its callsign CRCs.
constexpr uint32_t CRC24_INIT = 0xB704CEu;
constexpr uint32_t CRC24_POLY = 0x864CFBu;

int base40Index(char c)
{
    for (int i = 0; i < BASE40_SIZE; i++)
    {
        if (BASE40_ALPHABET[i] == c) return i;
    }
    return -1;
}

// Type codes. Glissando 0.3 and older began every frame with a type byte
// whose top nibble was 1, 2, 9 or A, so none of those is used here, and
// none of these, read as such a byte, is one of their types.
constexpr uint8_t CODE_PING = 0x3;
constexpr uint8_t CODE_PING_NO_AUTO_ACK = 0x4;
constexpr uint8_t CODE_PING_ACK = 0x5;
constexpr uint8_t CODE_MESSAGE_ACK = 0x6;
constexpr uint8_t CODE_PARTIAL_ACK = 0x7;
constexpr uint8_t CODE_MESSAGE = 0x8;
constexpr uint8_t CODE_MESSAGE_NO_AUTO_ACK = 0xB;
constexpr uint8_t CODE_BROADCAST = 0xC;
constexpr uint8_t CODE_BROADCAST_NO_AUTO_ACK = 0xD;

uint8_t typeCode(FrameType type, bool autoAck)
{
    switch (type)
    {
        case FrameType::Ping: return autoAck ? CODE_PING : CODE_PING_NO_AUTO_ACK;
        case FrameType::PingAck: return CODE_PING_ACK;
        case FrameType::MessageAck: return CODE_MESSAGE_ACK;
        case FrameType::MessagePartialAck: return CODE_PARTIAL_ACK;
        case FrameType::Message: return autoAck ? CODE_MESSAGE : CODE_MESSAGE_NO_AUTO_ACK;
        case FrameType::Broadcast: return autoAck ? CODE_BROADCAST : CODE_BROADCAST_NO_AUTO_ACK;
    }
    return 0;
}

// The six positions of a standard callsign, or empty if it is not one.
std::string standardPositions(const std::string& normalized)
{
    std::string t;
    if (normalized.size() >= 3 && std::isdigit((unsigned char)normalized[2])) t = normalized;
    else if (normalized.size() >= 2 && std::isdigit((unsigned char)normalized[1])) t = " " + normalized;
    else return std::string();
    if (t.size() > 6) return std::string();
    t.resize(6, ' ');

    for (int i = 0; i < 6; i++)
    {
        if (std::strchr(STANDARD_ALPHABETS[i], t[(size_t)i]) == nullptr) return std::string();
    }
    // At least one suffix letter, and no letter after a space.
    if (t[3] == ' ' || (t[4] == ' ' && t[5] != ' ')) return std::string();
    return t;
}

uint64_t packStandard(const std::string& positions)
{
    uint64_t value = 0;
    for (int i = 0; i < 6; i++)
    {
        const char* alphabet = STANDARD_ALPHABETS[i];
        value = value * std::strlen(alphabet) + (uint64_t)(std::strchr(alphabet, positions[(size_t)i]) - alphabet);
    }
    return value;
}

uint64_t packExtended(const std::string& normalized)
{
    // Left aligned and padded with the zero symbol, so unpacking gives the
    // callsign back with trailing pad characters that are easy to strip.
    uint64_t packed = 0;
    for (int i = 0; i < MAX_PACKED_CALLSIGN_CHARS; i++)
    {
        int index = i < (int)normalized.size() ? base40Index(normalized[(size_t)i]) : 0;
        if (index < 0) index = 0;
        packed = packed * BASE40_SIZE + (uint64_t)index;
    }
    return packed;
}

std::string trimmed(const std::string& s)
{
    size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    return s.substr(first, s.find_last_not_of(' ') - first + 1);
}

} // namespace

std::string FrameCodec::normalizeCallsign(const std::string& callsign)
{
    std::string result;
    result.reserve(callsign.size());

    for (char c : callsign)
    {
        char upper = (char)std::toupper((unsigned char)c);
        if (upper == ' ') continue;
        if (base40Index(upper) > 0) result += upper;
    }

    return result;
}

uint32_t FrameCodec::callsignCrc24(const std::string& callsign)
{
    std::string normalized = normalizeCallsign(callsign);
    uint32_t crc = CRC24_INIT;

    for (char c : normalized)
    {
        crc ^= ((uint32_t)(unsigned char)c) << 16;
        for (int bit = 0; bit < 8; bit++)
        {
            crc <<= 1;
            if (crc & 0x1000000u) crc ^= CRC24_POLY;
        }
    }

    return crc & 0xFFFFFFu;
}

uint32_t FrameCodec::callsignHash(const std::string& callsign)
{
    return callsignCrc24(callsign) >> (24 - DESTINATION_HASH_BITS);
}

bool FrameCodec::isStandardCallsign(const std::string& callsign)
{
    return !standardPositions(normalizeCallsign(callsign)).empty();
}

std::string FrameCodec::unpackCallsign(const uint8_t* data, int bit, bool standard)
{
    if (standard)
    {
        uint64_t value = getBits(data, bit, STANDARD_CALLSIGN_BITS);
        char positions[7] = {};
        for (int i = 5; i >= 0; i--)
        {
            size_t size = std::strlen(STANDARD_ALPHABETS[i]);
            positions[i] = STANDARD_ALPHABETS[i][value % size];
            value /= size;
        }
        // Values past the alphabets' range, or positions packing never
        // produces, are corruption that slipped past the modem CRC.
        std::string callsign = trimmed(positions);
        if (value != 0 || standardPositions(callsign) != std::string(positions)) return "";
        return callsign;
    }

    uint64_t packed = getBits(data, bit, EXTENDED_CALLSIGN_BITS);
    uint64_t maxPacked = 1;
    for (int i = 0; i < MAX_PACKED_CALLSIGN_CHARS; i++) maxPacked *= BASE40_SIZE;
    if (packed >= maxPacked) return "";

    char chars[MAX_PACKED_CALLSIGN_CHARS + 1];
    chars[MAX_PACKED_CALLSIGN_CHARS] = '\0';
    for (int i = MAX_PACKED_CALLSIGN_CHARS - 1; i >= 0; i--)
    {
        chars[i] = BASE40_ALPHABET[packed % BASE40_SIZE];
        packed /= BASE40_SIZE;
    }

    std::string result(chars);
    size_t end = result.find_last_not_of(' ');
    if (end == std::string::npos) return "";
    result.resize(end + 1);

    // Pad characters in the middle are not a callsign either.
    if (result.find(' ') != std::string::npos) return "";

    return result;
}

bool FrameCodec::typeFromCode(uint8_t code, FrameType& typeOut, bool& noAutoAckOut)
{
    noAutoAckOut = false;
    switch (code)
    {
        case CODE_PING_NO_AUTO_ACK: noAutoAckOut = true; [[fallthrough]];
        case CODE_PING: typeOut = FrameType::Ping; return true;
        case CODE_PING_ACK: typeOut = FrameType::PingAck; return true;
        case CODE_MESSAGE_ACK: typeOut = FrameType::MessageAck; return true;
        case CODE_PARTIAL_ACK: typeOut = FrameType::MessagePartialAck; return true;
        case CODE_MESSAGE_NO_AUTO_ACK: noAutoAckOut = true; [[fallthrough]];
        case CODE_MESSAGE: typeOut = FrameType::Message; return true;
        case CODE_BROADCAST_NO_AUTO_ACK: noAutoAckOut = true; [[fallthrough]];
        case CODE_BROADCAST: typeOut = FrameType::Broadcast; return true;
        default: return false;
    }
}

bool FrameCodec::isSignallingFrameType(FrameType type)
{
    switch (type)
    {
        case FrameType::Ping:
        case FrameType::PingAck:
        case FrameType::MessageAck:
        case FrameType::MessagePartialAck:
            return true;
        case FrameType::Message:
        case FrameType::Broadcast:
            return false;
    }

    return false;
}

int FrameCodec::signallingPayloadBits(FrameType type)
{
    return type == FrameType::PingAck || type == FrameType::MessagePartialAck ? SIGNALLING_PAYLOAD_BITS : 0;
}

int FrameCodec::headerBits(FrameType type, bool standardOrigin)
{
    int bits = ORIGIN_BIT + originBits(standardOrigin) + AIR_ID_BITS;
    if (isSignallingFrameType(type)) return bits + 1;
    return bits + BURSTS_FOLLOWING_BITS + 2 * FRAGMENT_FIELD_BITS;
}

size_t FrameCodec::textThatFits(const std::string& originCallsign, const std::string& text, size_t from)
{
    int start = headerBits(FrameType::Message, isStandardCallsign(originCallsign));
    return HamText::encode(text, from, nullptr, start, 8 * TEXT_FRAME_BYTES);
}

std::vector<uint8_t> FrameCodec::encode(const Frame& frame, int frameBytes)
{
    const bool signalling = isSignallingFrameType(frame.type);
    const std::string origin = normalizeCallsign(frame.originCallsign);
    const std::string positions = standardPositions(origin);
    const bool standard = !positions.empty();
    const int header = headerBits(frame.type, standard);
    const int payloadBits = signalling ? signallingPayloadBits(frame.type) : 0;

    if (origin.empty()) return {};
    if (frameBytes <= 0 || 8 * frameBytes < header + payloadBits) return {};
    if (frame.destinationCrc >> DESTINATION_HASH_BITS) return {};
    if (frame.airId > MAX_AIR_ID) return {};

    // A signalling frame has nowhere to put the fragment fields, so it may
    // only ever describe a single fragment.
    if (signalling)
    {
        if (frame.fragmentCount != 1 || frame.fragmentIndex != 0) return {};
        if ((int)frame.payload.size() * 8 > payloadBits) return {};
    }
    else
    {
        if (frame.fragmentCount == 0 || frame.fragmentCount > MAX_FRAGMENTS_PER_MESSAGE) return {};
        if (frame.fragmentIndex >= frame.fragmentCount) return {};
        if (frame.burstsFollowing > MAX_TEXT_BURSTS_FOLLOWING) return {};
    }

    std::vector<uint8_t> out((size_t)frameBytes, 0);
    uint8_t* data = out.data();
    int bit = 0;
    auto put = [&](uint64_t value, int count) {
        putBits(data, bit, value, count);
        bit += count;
    };

    put(typeCode(frame.type, frame.senderAutoAck || !(frame.type == FrameType::Ping ||
                                                      frame.type == FrameType::Message ||
                                                      frame.type == FrameType::Broadcast)),
        TYPE_BITS);
    put(frame.destinationCrc, DESTINATION_HASH_BITS);
    put(standard ? 0 : 1, 1);
    if (standard) put(packStandard(positions), STANDARD_CALLSIGN_BITS);
    else put(packExtended(origin), EXTENDED_CALLSIGN_BITS);

    if (signalling)
    {
        put(frame.burstsFollowing > 0 ? 1 : 0, 1);
        put(frame.airId, AIR_ID_BITS);
        if (payloadBits > 0) put(frame.payload.empty() ? 0 : frame.payload[0], payloadBits);
        return out;
    }

    put(frame.burstsFollowing, BURSTS_FOLLOWING_BITS);
    put(frame.airId, AIR_ID_BITS);
    put(frame.fragmentIndex, FRAGMENT_FIELD_BITS);
    put(frame.fragmentCount - 1u, FRAGMENT_FIELD_BITS);

    std::string text(frame.payload.begin(), frame.payload.end());
    if (HamText::encode(text, 0, data, bit, 8 * frameBytes) != text.size()) return {};
    return out;
}

bool FrameCodec::decode(const uint8_t* data, int length, Frame& frameOut)
{
    if (data == nullptr || length < 1) return false;
    const int available = 8 * length;
    if (available < ORIGIN_BIT + 1) return false;

    FrameType type;
    bool noAutoAck = false;
    if (!typeFromCode((uint8_t)getBits(data, 0, TYPE_BITS), type, noAutoAck)) return false;

    const bool signalling = isSignallingFrameType(type);
    const bool standard = getBits(data, ORIGIN_BIT, 1) == 0;
    const int header = headerBits(type, standard);
    const int payloadBits = signalling ? signallingPayloadBits(type) : 0;
    if (available < header + payloadBits) return false;

    std::string originCallsign = unpackCallsign(data, ORIGIN_BIT + 1, standard);
    if (originCallsign.empty()) return false;

    int bit = ORIGIN_BIT + originBits(standard);
    auto get = [&](int count) {
        uint64_t value = getBits(data, bit, count);
        bit += count;
        return value;
    };

    Frame frame;
    frame.type = type;
    frame.destinationCrc = (uint32_t)getBits(data, DESTINATION_BIT, DESTINATION_HASH_BITS);
    frame.originCallsign = originCallsign;
    frame.senderAutoAck = !noAutoAck;

    if (signalling)
    {
        frame.burstsFollowing = (uint8_t)get(1);
        frame.airId = (uint16_t)get(AIR_ID_BITS);
        if (payloadBits > 0) frame.payload.push_back((uint8_t)get(payloadBits));
    }
    else
    {
        frame.burstsFollowing = (uint8_t)get(BURSTS_FOLLOWING_BITS);
        frame.airId = (uint16_t)get(AIR_ID_BITS);
        frame.fragmentIndex = (uint8_t)get(FRAGMENT_FIELD_BITS);
        frame.fragmentCount = (uint8_t)(get(FRAGMENT_FIELD_BITS) + 1);
        if (frame.fragmentIndex >= frame.fragmentCount) return false;
        std::string text = HamText::decode(data, bit, available);
        frame.payload.assign(text.begin(), text.end());
    }

    frameOut = frame;
    return true;
}

void FrameCodec::expectedFrameStart(const std::string& ownCallsign, const std::string& fromCallsign,
                                    uint8_t* bytesOut, uint8_t* masksOut)
{
    static_assert(8 * EXPECTED_START_BYTES >= ORIGIN_BIT + 1 + EXTENDED_CALLSIGN_BITS,
                  "the expected start must cover the longest origin callsign");
    std::memset(bytesOut, 0, EXPECTED_START_BYTES);
    std::memset(masksOut, 0, EXPECTED_START_BYTES);

    // The type is anybody's guess.
    putBits(bytesOut, DESTINATION_BIT, callsignHash(ownCallsign), DESTINATION_HASH_BITS);
    putBits(masksOut, DESTINATION_BIT, (1u << DESTINATION_HASH_BITS) - 1, DESTINATION_HASH_BITS);

    std::string from = normalizeCallsign(fromCallsign);
    if (from.empty()) return;
    std::string positions = standardPositions(from);
    bool standard = !positions.empty();
    int bits = originBits(standard);
    putBits(bytesOut, ORIGIN_BIT, standard ? 0 : 1, 1);
    if (standard) putBits(bytesOut, ORIGIN_BIT + 1, packStandard(positions), STANDARD_CALLSIGN_BITS);
    else putBits(bytesOut, ORIGIN_BIT + 1, packExtended(from), EXTENDED_CALLSIGN_BITS);
    putBits(masksOut, ORIGIN_BIT, ~0ull >> (64 - bits), bits);
}

} // namespace TextMessaging
