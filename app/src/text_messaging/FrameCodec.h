//=========================================================================
// Name:            FrameCodec.h
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

#ifndef TEXT_MESSAGING__FRAME_CODEC_H
#define TEXT_MESSAGING__FRAME_CODEC_H

#include <cstdint>
#include <string>
#include <vector>

#include "TextMessagingTypes.h"

namespace TextMessaging
{

// One decoded (or about to be encoded) frame. Frames are carried by the codec2
// raw data API, which appends and verifies its own CRC over each modem frame,
// so nothing here repeats that check; a frame handed to decode() has already
// been vouched for by the modem.
struct Frame
{
    FrameType type = FrameType::Ping;
    uint32_t destinationCrc = 0;    // FrameCodec::callsignHash(); zero means "broadcast"
    std::string originCallsign;     // as decoded, may be truncated to 9 chars
    uint16_t airId = 0;             // matches a message to its acknowledgement; up to MAX_AIR_ID
    uint8_t fragmentIndex = 0;      // zero based
    uint8_t fragmentCount = 1;

    // Bursts still to come after this one in the same keying of the sender's
    // transmitter. A text frame carries the exact count (up to
    // MAX_TEXT_BURSTS_FOLLOWING); a signalling frame carries only whether it
    // is zero, so it always decodes as 0 or 1.
    uint8_t burstsFollowing = 0;

    // Whether the sender acknowledges and answers pings by itself. Said by
    // messages, broadcasts and pings; true for every other frame.
    bool senderAutoAck = true;

    // A text frame's characters, or a pong's or partial acknowledgement's
    // one byte. Pings and acknowledgements carry none.
    std::vector<uint8_t> payload;

    // A ping's or acknowledgement's FEATURE_* bits. Zero from Glissando 0.5
    // and older, which leave the byte empty.
    uint8_t features = 0;

    // A locator frame's grid square, four characters (FrameCodec::normalizeLocator).
    std::string locator;
};

constexpr int MAX_TEXT_BURSTS_FOLLOWING = (1 << BURSTS_FOLLOWING_BITS) - 1;

class FrameCodec
{
public:
    // Uppercases and strips characters the air alphabet cannot carry, so that
    // the hash both ends compute over a callsign agrees. Returns an empty
    // string if nothing usable is left.
    static std::string normalizeCallsign(const std::string& callsign);

    // CRC-24/OPENPGP over the normalized callsign.
    static uint32_t callsignCrc24(const std::string& callsign);

    // The DESTINATION_HASH_BITS that address a frame: the top of the CRC.
    static uint32_t callsignHash(const std::string& callsign);

    // Whether a callsign packs into STANDARD_CALLSIGN_BITS (a prefix of one
    // or two characters, a digit, then one to three letters, as FT8 packs
    // it) rather than EXTENDED_CALLSIGN_BITS.
    static bool isStandardCallsign(const std::string& callsign);

    // A Maidenhead locator as the operator typed it, tidied: a grid square
    // ("CN87") or a subsquare ("CN87ux"), the field letters upper case and
    // the subsquare's lower case. Empty if it is neither.
    static std::string normalizeLocator(const std::string& locator);

    // The LOCATOR_BITS value of a locator's grid square (its first four
    // characters), or -1 if it has none; and the grid square a value stands
    // for, or empty for a value packing never produces.
    static int packGridSquare(const std::string& locator);
    static std::string unpackGridSquare(int value);

    // Serializes a frame, zero padded out to frameBytes (SIGNALLING_FRAME_BYTES
    // or TEXT_FRAME_BYTES). Returns an empty vector if the frame does not fit,
    // if a field is out of range, if it carries a callsign that cannot be
    // packed, or if a text frame's text does not all fit behind its header.
    static std::vector<uint8_t> encode(const Frame& frame, int frameBytes);

    // A locator frame cut down to what a duet's spare voice holds: its type,
    // the sender's callsign and the grid square, with no destination and no
    // bursts-following bit (it is always the end of its keying), so it fits
    // nine bytes from any callsign the air alphabet packs. It has its own
    // type code, so builds that know only the full form pass it over;
    // decode() reads it as a Locator with destinationCrc and burstsFollowing
    // zero. Zero padded to frameBytes; empty if the callsign or the locator
    // cannot be packed.
    static std::vector<uint8_t> encodeShortLocator(const Frame& frame, int frameBytes);

    // Whether a TYPE_BITS type code is the short locator's, whose callsign
    // starts at TYPE_BITS rather than ORIGIN_BIT.
    static bool isShortLocatorCode(uint8_t code);

    // Parses a frame received from the modem. Returns false when the frame is
    // too short for its header, the type is not one of ours, or a field holds
    // a value no sender produces.
    static bool decode(const uint8_t* data, int length, Frame& frameOut);

    // How many characters of text, from character `from`, fit in one text
    // frame sent by originCallsign. The protocol cuts messages with it.
    static size_t textThatFits(const std::string& originCallsign, const std::string& text, size_t from);

    // Pings and acknowledgements ride DATAC13, which is too small for the
    // fragment fields, so they carry a shorter header and are always a single
    // fragment. Message text rides DATAC4 and carries the full header.
    static bool isSignallingFrameType(FrameType type);

    // Whether a frame type ends in a feature byte: pings and acknowledgements.
    static bool carriesFeatures(FrameType type);

    // The frame type a TYPE_BITS type code stands for, and whether it says
    // the sender has Auto acknowledge off. False for a code no frame uses.
    static bool typeFromCode(uint8_t code, FrameType& typeOut, bool& noAutoAckOut);

    // Where the header's fields sit, for reading a frame as it arrives: the
    // origin callsign starts at ORIGIN_BIT after its form bit, and everything
    // after it moves with its length.
    static constexpr int DESTINATION_BIT = TYPE_BITS;
    static constexpr int ORIGIN_BIT = DESTINATION_BIT + DESTINATION_HASH_BITS;
    static int originBits(bool standard) { return 1 + (standard ? STANDARD_CALLSIGN_BITS : EXTENDED_CALLSIGN_BITS); }
    static int headerBits(FrameType type, bool standardOrigin);
    static int signallingPayloadBits(FrameType type);

    // The callsign packed at bit (after the form bit), or empty if the value
    // is not one packing produces.
    static std::string unpackCallsign(const uint8_t* data, int bit, bool standard);

    // What a station can count on in the opening bytes of a frame addressed
    // to it: bits set in masksOut[i] are known to equal those of bytesOut[i].
    // The destination hash (ownCallsign's), and with a non-empty fromCallsign
    // the sender's callsign with its form bit. Fills EXPECTED_START_BYTES
    // bytes. A receiver hands these to its decoder as guesses, so a reply
    // from the station it is working decodes on less signal (FT8 calls it a
    // priori decoding).
    static constexpr int EXPECTED_START_BYTES = 10;
    static void expectedFrameStart(const std::string& ownCallsign, const std::string& fromCallsign,
                                   uint8_t* bytesOut, uint8_t* masksOut);
};

} // namespace TextMessaging

#endif // TEXT_MESSAGING__FRAME_CODEC_H
