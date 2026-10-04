//=========================================================================
// Name:            FrameAnnotation.cpp
// Purpose:         Reads a chat frame out loud as it arrives, piece by
//                  piece, for the Glissando visi-scope.
//=========================================================================

#include "FrameAnnotation.h"

#include <algorithm>
#include <cstdio>

#include "FrameCodec.h"
#include "HamText.h"
#include "TextMessagingTypes.h"

namespace TextMessaging
{

namespace
{

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

std::string format(const char* pattern, unsigned a, unsigned b = 0)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), pattern, a, b);
    return buffer;
}

// The header is packed to the bit, so fields are read by bit position.
class Reader
{
public:
    Reader(const std::vector<uint8_t>& bytes, int segmentFrom, int knownFrom)
        : bytes_(bytes), segmentFromBit_(8 * segmentFrom), knownFromBit_(8 * knownFrom)
    {
    }

    bool known(int bit, int count) const
    {
        return bit >= knownFromBit_ && bit + count <= sizeBits();
    }

    // A field is told on the segment that brings its last bit.
    bool completesHere(int bit, int count) const
    {
        return known(bit, count) && bit + count - 1 >= segmentFromBit_;
    }

    uint64_t value(int bit, int count) const { return getBits(bytes_.data(), bit, count); }
    const uint8_t* data() const { return bytes_.data(); }
    int segmentFromBit() const { return segmentFromBit_; }
    int sizeBits() const { return 8 * (int)bytes_.size(); }

private:
    const std::vector<uint8_t>& bytes_;
    int segmentFromBit_;
    int knownFromBit_;
};

void add(std::vector<AnnotationToken>& tokens, AnnotationToken::Role role, const std::string& text)
{
    tokens.push_back(AnnotationToken{role, text});
}

// The characters the newest segment carried, from bit `from` on. Codes never
// cross a segment, so a segment's text reads on its own.
void addText(std::vector<AnnotationToken>& tokens, const Reader& r, int from)
{
    from = std::max(from, r.segmentFromBit());
    if (from >= r.sizeBits()) return;
    std::string text;
    for (char c : HamText::decode(r.data(), from, r.sizeBits()))
    {
        text.push_back((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F ? '.' : c);
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

    // Without the type and the origin's form there is no telling where the
    // header ends: say what was lost, and read any segment past the longest
    // header as text.
    const int longestHeader = FrameCodec::headerBits(FrameType::Message, false);
    if (!r.known(0, TYPE_BITS) || !r.known(FrameCodec::ORIGIN_BIT, 1))
    {
        int textFrom = (longestHeader + HamText::TEXT_BLOCK_BITS - 1) / HamText::TEXT_BLOCK_BITS *
                       HamText::TEXT_BLOCK_BITS;
        if (r.segmentFromBit() < textFrom) add(tokens, Role::Unknown, "...");
        else if (text) addText(tokens, r, textFrom);
        return tokens;
    }

    FrameType type;
    bool noAutoAck = false;
    if (!FrameCodec::typeFromCode((uint8_t)r.value(0, TYPE_BITS), type, noAutoAck))
    {
        if (r.completesHere(0, TYPE_BITS)) add(tokens, Role::Unknown, "NOT CHAT");
        return tokens;
    }
    const bool signalling = FrameCodec::isSignallingFrameType(type);
    const bool standard = r.value(FrameCodec::ORIGIN_BIT, 1) == 0;
    const int originBits = FrameCodec::originBits(standard);

    if (r.completesHere(0, TYPE_BITS))
    {
        add(tokens, Role::Kind, kindName(type));
        if (noAutoAck) add(tokens, Role::Field, "NO AUTO ACK");
    }
    if (r.completesHere(FrameCodec::DESTINATION_BIT, DESTINATION_HASH_BITS))
    {
        uint32_t hash = (uint32_t)r.value(FrameCodec::DESTINATION_BIT, DESTINATION_HASH_BITS);
        std::string name = hash != 0 && nameFor ? nameFor(hash) : std::string();
        add(tokens, Role::Station, hash == 0 ? std::string("TO ALL")
                                   : !name.empty() ? "TO " + name : format("TO #%05X", hash));
    }
    if (r.completesHere(FrameCodec::ORIGIN_BIT, originBits))
    {
        std::string origin = FrameCodec::unpackCallsign(r.data(), FrameCodec::ORIGIN_BIT + 1, standard);
        add(tokens, Role::Station, "DE " + (origin.empty() ? std::string("?") : origin));
    }

    int bit = FrameCodec::ORIGIN_BIT + originBits;
    if (signalling)
    {
        if (r.completesHere(bit, 1) && r.value(bit, 1) != 0) add(tokens, Role::Field, "MORE TO COME");
        bit += 1;
        if (r.completesHere(bit, AIR_ID_BITS)) add(tokens, Role::Field, format("No.%u", (unsigned)r.value(bit, AIR_ID_BITS)));
        bit += AIR_ID_BITS;

        // A pong's SNR or which parts arrived, in hex.
        int payloadBits = FrameCodec::signallingPayloadBits(type);
        if (payloadBits > 0 && r.completesHere(bit, payloadBits))
        {
            add(tokens, Role::Field, format("%02X", (unsigned)r.value(bit, payloadBits)));
        }
        return tokens;
    }

    int followingBit = bit;
    bit += BURSTS_FOLLOWING_BITS;
    if (r.completesHere(bit, AIR_ID_BITS)) add(tokens, Role::Field, format("No.%u", (unsigned)r.value(bit, AIR_ID_BITS)));
    bit += AIR_ID_BITS;
    if (r.completesHere(followingBit, bit + 2 * FRAGMENT_FIELD_BITS - followingBit))
    {
        unsigned index = (unsigned)r.value(bit, FRAGMENT_FIELD_BITS);
        unsigned count = (unsigned)r.value(bit + FRAGMENT_FIELD_BITS, FRAGMENT_FIELD_BITS) + 1;
        unsigned following = (unsigned)r.value(followingBit, BURSTS_FOLLOWING_BITS);
        add(tokens, Role::Field, format("PART %u/%u", index + 1, count));
        if (following > 0) add(tokens, Role::Field, format("%u MORE TO COME", following));
    }
    bit += 2 * FRAGMENT_FIELD_BITS;

    addText(tokens, r, bit);
    return tokens;
}

} // namespace TextMessaging
