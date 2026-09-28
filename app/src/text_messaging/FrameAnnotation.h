//=========================================================================
// Name:            FrameAnnotation.h
// Purpose:         Reads a chat frame out loud as it arrives, piece by
//                  piece, for the Glissando visi-scope to write on the
//                  waterfall.
//
// Glissando carries a chat frame nine bytes at a time (see
// modem/GlissandoLink.h), so a frame's header and text are heard one
// segment after another, a frame length apart. describeSegment() says what
// the newest segment added: the header fields that became whole with it
// (the frame type, who it is to and from, the message number, which part of
// how many) and the characters of text it carried.
//
// Nothing here depends on wxWidgets or the modem.
//=========================================================================

#ifndef TEXT_MESSAGING__FRAME_ANNOTATION_H
#define TEXT_MESSAGING__FRAME_ANNOTATION_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace TextMessaging
{

struct AnnotationToken
{
    enum class Role
    {
        Kind,       // what the frame is: MESSAGE, PING, ACK...
        Station,    // who from, who to
        Field,      // message number, part, length
        Text,       // characters of the message itself
        Unknown,    // bytes whose meaning was lost with an earlier segment
    };

    Role role = Role::Field;
    std::string text;
};

// A name for a destination CRC: the station's callsign, "you", or empty
// when the CRC is not one we know.
using CallsignForCrc = std::function<std::string(uint32_t crc)>;

// bytes holds the frame so far, from its first byte; the newest segment is
// bytes [segmentFrom, bytes.size()). Bytes before knownFrom were never heard
// and are ignored. text says which of the two burst sizes the frame is
// (signalling or text). nameFor may be empty.
std::vector<AnnotationToken> describeSegment(const std::vector<uint8_t>& bytes, int segmentFrom, int knownFrom,
                                             bool text, const CallsignForCrc& nameFor);

} // namespace TextMessaging

#endif // TEXT_MESSAGING__FRAME_ANNOTATION_H
