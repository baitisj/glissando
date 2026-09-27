//=========================================================================
// Name:            Data2GLink.h
// Purpose:         The byte formats chat uses to talk to an external
//                  data2g-host: KISS framing, AX.25 UI frames, and the
//                  VARA-style command port's notifications. No sockets
//                  here, so all of it can be tested on its own.
//
// Written for Glissando from the public KISS, AX.25 2.2 and VARA protocol
// descriptions; no Data2G code is used (see docs/DATA2G.md).
//=========================================================================

#ifndef TEXT_MESSAGING__DATA2G_LINK_H
#define TEXT_MESSAGING__DATA2G_LINK_H

#include <cstdint>
#include <string>
#include <vector>

#include "TextMessagingTypes.h"

namespace TextMessaging
{
namespace Data2G
{

constexpr int DEFAULT_KISS_PORT = 8100;
constexpr int DEFAULT_COMMAND_PORT = 8300;

// Every chat frame goes to the same AX.25 destination: our frames address
// stations by a callsign CRC inside the frame, and Data2G sends UI frames in
// its robust broadcast mode whoever they are for.
constexpr const char* AX25_DESTINATION = "GLISS";

// The info field of our UI frames starts with this, so frames from other
// programs sharing the host (APRS, Winlink) are told apart and dropped.
constexpr uint8_t INFO_TAG[] = {'G', 'L', 'S', 1};
constexpr int INFO_TAG_BYTES = (int)sizeof(INFO_TAG);

// KISS (TCP, as KA9Q/K3MC): FEND frame FEND, with FEND and FESC in the data
// escaped. Only data frames on port 0 are sent.
std::vector<uint8_t> kissEncode(const std::vector<uint8_t>& data);

// Splits a KISS byte stream into data frames, however it arrives in pieces.
class KissDecoder
{
public:
    // Appends every complete data frame in bytes (on any port) to framesOut.
    // Frames of other KISS commands are dropped.
    void feed(const uint8_t* bytes, int length, std::vector<std::vector<uint8_t>>& framesOut);
    void reset();

private:
    std::vector<uint8_t> current_;
    bool escaped_ = false;
    bool inFrame_ = false;
};

// An AX.25 address as far as AX.25 can carry it: up to six letters and
// digits and an SSID of 0 to 15, from a callsign such as "VK2ABC-7".
// Portable and other suffixes ("/P") are dropped; the frame inside carries
// the full callsign. Nothing usable left gives "NOCALL".
struct Ax25Address
{
    std::string call;
    int ssid = 0;
};

Ax25Address ax25Address(const std::string& callsign);

// A UI command frame (control 0x03, PID 0xF0: no layer 3) from source to
// destination carrying info.
std::vector<uint8_t> ax25UiFrame(const std::string& source, const std::string& destination,
                                 const std::vector<uint8_t>& info);

// Parses a UI frame without digipeaters or with up to eight of them. Returns
// false for anything else: a connected-mode frame, a PID other than 0xF0, a
// malformed address field.
struct Ax25Ui
{
    Ax25Address source;
    Ax25Address destination;
    std::vector<uint8_t> info;
};

bool parseAx25Ui(const std::vector<uint8_t>& frame, Ax25Ui& out);

// One chat frame (as FrameCodec::encode made it) as the KISS payload that
// carries it, and back. chatFrameFromPayload() returns false for a payload
// that is not one of ours.
std::vector<uint8_t> payloadForChatFrame(const std::string& myCallsign,
                                         const std::vector<uint8_t>& chatFrame);
bool chatFrameFromPayload(const std::vector<uint8_t>& payload, std::vector<uint8_t>& chatFrameOut);

// What a line from the command port means to us. Everything the host says
// that chat has no use for (CONNECTED, BUFFER, IAMALIVE, OK, ...) is Other.
struct CommandEvent
{
    enum class Type { Other, Ptt, Busy, Mode } type = Type::Other;
    bool on = false;         // Ptt, Busy
    std::string mode;        // Mode: the submode name
};

CommandEvent parseCommandLine(const std::string& line);

// Splits the command port's CR (or LF) terminated lines.
class LineSplitter
{
public:
    void feed(const char* bytes, int length, std::vector<std::string>& linesOut);
    void reset() { current_.clear(); }

private:
    std::string current_;
};

// Protocol timers for chat over Data2G. A keying of ours goes out as one
// Data2G burst, which the far end decodes only once all of it has arrived.
// Sized for the longest burst Data2G plans for broadcasts (12 s) and a
// decode of a few seconds; the command port's PTT and BUSY reports keep the
// protocol from waiting on these in the usual case.
AirTiming airTiming();

// How long a keying is assumed to hold the channel when the command port is
// off and Data2G cannot tell us.
constexpr int ESTIMATED_BURST_MILLISECONDS = 14000;

} // namespace Data2G
} // namespace TextMessaging

#endif // TEXT_MESSAGING__DATA2G_LINK_H
