//=========================================================================
// Name:            FrameAnnotation.cpp
// Purpose:         Reads a chat frame out loud as it arrives, piece by
//                  piece, for the Glissando visi-scope.
//=========================================================================

#include "FrameAnnotation.h"

#include <algorithm>
#include <cstdio>

#include "FrameCodec.h"
#include "TextMessagingTypes.h"

namespace TextMessaging
{

namespace
{

// The header layout, as FrameCodec.cpp packs it (see TextMessagingTypes.h).
// FrameAnnotationTest checks these against frames FrameCodec encodes.
constexpr int OFFSET_TYPE = 0;
constexpr int OFFSET_DEST_CRC = 1;
constexpr int OFFSET_ORIGIN_CALLSIGN = 4;
constexpr int OFFSET_AIR_ID = 10;
constexpr int OFFSET_FRAGMENT_INDEX = 12;
constexpr int OFFSET_FRAGMENT_COUNT = 13;
constexpr int OFFSET_SIGNALLING_PAYLOAD_LENGTH = 12;
constexpr int OFFSET_TEXT_PAYLOAD_LENGTH = 14;
constexpr uint8_t TYPE_MORE_FOLLOWS = 0x80;
constexpr uint8_t TYPE_VALUE_MASK = 0x7F;
constexpr uint8_t FRAGMENT_INDEX_MASK = 0x0F;
constexpr int BURSTS_FOLLOWING_SHIFT = 4;

const char* kindName(FrameType type)
{
    switch (type)
    {
        case FrameType::Ping: return "PING";
        case FrameType::PingAck: return "PONG";
        case FrameType::Message: return "MESSAGE";
        case FrameType::MessageAck: return "ACK";
        case FrameType::Broadcast: return "BROADCAST";
        case FrameType::MessagePartialAck: return "PARTIAL ACK";
    }
    return "?";
}

bool carriesText(FrameType type)
{
    return type == FrameType::Message || type == FrameType::Broadcast;
}

std::string format(const char* pattern, unsigned a, unsigned b = 0)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), pattern, a, b);
    return buffer;
}

class Reader
{
public:
    Reader(const std::vector<uint8_t>& bytes, int segmentFrom, int knownFrom)
        : bytes_(bytes), segmentFrom_(segmentFrom), knownFrom_(knownFrom)
    {
    }

    bool known(int offset, int length) const
    {
        return offset >= knownFrom_ && offset + length <= (int)bytes_.size();
    }

    // A field is told on the segment that brings its last byte.
    bool completesHere(int offset, int length) const
    {
        int lastByte = offset + length - 1;
        return known(offset, length) && lastByte >= segmentFrom_;
    }

    uint32_t value(int offset, int length) const
    {
        uint32_t v = 0;
        for (int i = 0; i < length; i++) v = (v << 8) | bytes_[(size_t)(offset + i)];
        return v;
    }

    const uint8_t* at(int offset) const { return bytes_.data() + offset; }
    int segmentFrom() const { return segmentFrom_; }
    int size() const { return (int)bytes_.size(); }

private:
    const std::vector<uint8_t>& bytes_;
    int segmentFrom_;
    int knownFrom_;
};

void add(std::vector<AnnotationToken>& tokens, AnnotationToken::Role role, const std::string& text)
{
    tokens.push_back(AnnotationToken{role, text});
}

// The characters of the segment from `from`, up to `to`, as one token.
// Zero bytes are the padding after the text and are left out.
void addText(std::vector<AnnotationToken>& tokens, const Reader& r, int from, int to)
{
    std::string text;
    for (int i = std::max(from, r.segmentFrom()); i < std::min(to, r.size()); i++)
    {
        uint8_t c = *r.at(i);
        if (c == 0) continue;
        text.push_back(c < 0x20 || c == 0x7F ? '.' : (char)c);
    }
    if (!text.empty()) add(tokens, AnnotationToken::Role::Text, text);
}

} // namespace

std::vector<AnnotationToken> describeSegment(const std::vector<uint8_t>& bytes, int segmentFrom, int knownFrom,
                                             bool text, const CallsignForCrc& nameFor)
{
    using Role = AnnotationToken::Role;
    std::vector<AnnotationToken> tokens;
    Reader r(bytes, segmentFrom, knownFrom);
    if (segmentFrom >= (int)bytes.size()) return tokens;

    // Without the type byte there is no telling the header's layout: say
    // what was lost, and read anything past the longest header as text.
    if (!r.known(OFFSET_TYPE, 1))
    {
        int header = text ? TEXT_HEADER_BYTES : SIGNALLING_HEADER_BYTES;
        if (segmentFrom < header) add(tokens, Role::Unknown, "...");
        if (text) addText(tokens, r, header, r.size());
        return tokens;
    }

    uint8_t typeByte = *r.at(OFFSET_TYPE);
    if (!FrameCodec::isKnownFrameType(typeByte & TYPE_VALUE_MASK))
    {
        if (r.completesHere(OFFSET_TYPE, 1)) add(tokens, Role::Unknown, "NOT CHAT");
        return tokens;
    }
    FrameType type = (FrameType)(typeByte & TYPE_VALUE_MASK);
    bool signalling = FrameCodec::isSignallingFrameType(type);
    int header = FrameCodec::headerBytes(type);

    if (r.completesHere(OFFSET_TYPE, 1))
    {
        add(tokens, Role::Kind, kindName(type));
    }
    if (r.completesHere(OFFSET_DEST_CRC, 3))
    {
        uint32_t crc = r.value(OFFSET_DEST_CRC, 3);
        std::string name = crc != 0 && nameFor ? nameFor(crc) : std::string();
        add(tokens, Role::Station, crc == 0 ? std::string("TO ALL")
                                   : !name.empty() ? "TO " + name : format("TO #%06X", crc));
    }
    if (r.completesHere(OFFSET_ORIGIN_CALLSIGN, PACKED_CALLSIGN_BYTES))
    {
        add(tokens, Role::Station, "DE " + FrameCodec::unpackCallsign(r.at(OFFSET_ORIGIN_CALLSIGN)));
    }
    if (r.completesHere(OFFSET_AIR_ID, 2))
    {
        add(tokens, Role::Field, format("No.%u", r.value(OFFSET_AIR_ID, 2)));
    }

    int lengthOffset = signalling ? OFFSET_SIGNALLING_PAYLOAD_LENGTH : OFFSET_TEXT_PAYLOAD_LENGTH;
    if (!signalling && r.completesHere(OFFSET_FRAGMENT_INDEX, 2))
    {
        uint8_t indexByte = *r.at(OFFSET_FRAGMENT_INDEX);
        unsigned index = indexByte & FRAGMENT_INDEX_MASK;
        unsigned following = indexByte >> BURSTS_FOLLOWING_SHIFT;
        add(tokens, Role::Field, format("PART %u/%u", index + 1, *r.at(OFFSET_FRAGMENT_COUNT)));
        if (following > 0) add(tokens, Role::Field, format("%u MORE TO COME", following));
    }
    if (signalling && (typeByte & TYPE_MORE_FOLLOWS) && r.completesHere(OFFSET_TYPE, 1))
    {
        add(tokens, Role::Field, "MORE TO COME");
    }

    int payloadEnd = r.size();
    if (r.known(lengthOffset, 1))
    {
        int length = *r.at(lengthOffset);
        payloadEnd = header + length;
        if (r.completesHere(lengthOffset, 1))
        {
            add(tokens, Role::Field, carriesText(type) ? format("%u CH", (unsigned)length)
                                                       : format("%u B", (unsigned)length));
        }
    }

    if (carriesText(type))
    {
        addText(tokens, r, header, payloadEnd);
    }
    else
    {
        // Signalling payloads (a ping's SNR, which parts arrived) in hex.
        std::string hex;
        for (int i = std::max(header, segmentFrom); i < std::min(payloadEnd, r.size()); i++)
        {
            hex += format(hex.empty() ? "%02X" : " %02X", *r.at(i));
        }
        if (!hex.empty()) add(tokens, Role::Field, hex);
    }
    return tokens;
}

} // namespace TextMessaging
