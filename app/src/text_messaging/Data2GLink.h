//=========================================================================
// Name:            Data2GLink.h
// Purpose:         The byte formats chat uses to talk to an external
//                  data2g-host: KISS framing (with ACKMODE), the command
//                  port's lines, the modes it offers and which one each
//                  Glissando tempo maps to, and the framing of chat frames
//                  inside a connected session's byte stream. No sockets
//                  here, so all of it can be tested on its own.
//
// Written for Glissando from the public KISS and VARA protocol descriptions
// and Data2G's docs/broadcast.md; no Data2G code is used (see
// docs/DATA2G.md).
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
constexpr int DEFAULT_COMMAND_PORT = 8300; // the session data port is the next one

// The broadcast group chat opens. Its bursts carry the name, and every
// codeword a CRC mask hashed from it, so only stations that opened GLISS
// take them in.
constexpr const char* GROUP = "GLISS";

//-------------------------------------------------------------------------
// KISS
//-------------------------------------------------------------------------

constexpr uint8_t KISS_DATA = 0x00;
constexpr uint8_t KISS_ACKMODE = 0x0C;

// A data frame for a KISS port (0 to 15).
std::vector<uint8_t> kissEncode(int port, const std::vector<uint8_t>& data);

// An ACKMODE frame: data2g-host answers with the same tag once the burst
// carrying it has been transmitted.
std::vector<uint8_t> kissEncodeAckMode(int port, uint16_t tag, const std::vector<uint8_t>& data);

struct KissFrame
{
    int port = 0;
    uint8_t command = KISS_DATA;    // low nibble of the command byte
    std::vector<uint8_t> payload;   // data, or for an ACKMODE ack the 2-byte tag
};

// Splits a KISS byte stream into frames, however it arrives in pieces.
class KissDecoder
{
public:
    void feed(const uint8_t* bytes, int length, std::vector<KissFrame>& framesOut);
    void reset();

private:
    std::vector<uint8_t> current_;
    bool escaped_ = false;
    bool inFrame_ = false;
};

//-------------------------------------------------------------------------
// Modes
//-------------------------------------------------------------------------

// One line of the MODES list: "MODE name bandwidth-Hz bytes-per-codeword
// max-codewords seconds-at-1 seconds-at-max".
struct ModeInfo
{
    std::string name;
    int bandwidthHz = 0;
    int bytesPerCodeword = 0;
    int maxCodewords = 0;
    double secondsAtOne = 0.0;
    double secondsAtMax = 0.0;

    bool valid() const { return !name.empty() && bytesPerCodeword > 0 && maxCodewords > 0; }

    // How long a burst carrying these frames takes: one control codeword,
    // then the frames back to back behind a 2-byte length each.
    double burstSeconds(const std::vector<int>& frameBytes) const;
};

// The modes a 2400 Hz data2g-host offered when this was written, for the
// ones the tempos use; what the host's own MODES list says wins.
const std::vector<ModeInfo>& knownModes();

// The mode a Glissando tempo (1 Adagio .. 5 Duet) goes out in: the first of
// its choices the host offers. Adagio is Data2G's most sensitive mode, Duet
// a fast 2.3 kHz one; a host capped at 500 Hz gets narrow modes throughout.
ModeInfo modeForGear(int gear, const std::vector<ModeInfo>& offered);

// Protocol timers for chat sent in a mode: a whole keying is one Data2G
// burst, decoded once all of it has arrived. The command port's PTT, BUSY
// and transmitted reports keep the protocol from waiting on these in the
// usual case.
AirTiming airTiming(const ModeInfo& mode);

//-------------------------------------------------------------------------
// The command port
//-------------------------------------------------------------------------

// What a line from the command port means to us.
struct CommandEvent
{
    enum class Type
    {
        Other,          // IAMALIVE, VERSION and anything chat has no use for
        Ok,
        Wrong,
        Ptt,            // on
        Busy,           // on
        Mode,           // text: the submode a burst went out in
        ModeLine,       // modeInfo: one line of the MODES list
        BcastPort,      // number: the port BCAST OPEN gave
        BcastHeard,     // number, text: the sender's call when sent with FROM
        BcastLost,      // number, count
        BcastDropped,   // number, count
        Connected,      // text: caller, peer: called station
        Disconnected,
        Buffer,         // count: bytes still to send in the session
        Cqframe,
    } type = Type::Other;

    bool on = false;
    int number = 0;
    int count = 0;
    std::string text;
    std::string peer;
    ModeInfo modeInfo;
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

// The callsign as the command port and a BCAST FROM can carry it: letters,
// digits, '/' and '-', at most 10 characters, upper case.
std::string commandCallsign(const std::string& callsign);

//-------------------------------------------------------------------------
// Connected sessions
//-------------------------------------------------------------------------

// Chat frames inside a session's byte stream: 'G', a length byte, then the
// frame, so a stream from a program that is not Glissando is told apart.
std::vector<uint8_t> streamEncode(const std::vector<uint8_t>& frame);

class StreamDecoder
{
public:
    // Appends every complete frame; returns false once the stream has
    // shown it is not ours, after which it takes nothing more.
    bool feed(const uint8_t* bytes, int length, std::vector<std::vector<uint8_t>>& framesOut);
    void reset();

private:
    std::vector<uint8_t> pending_;
    bool foreign_ = false;
};

} // namespace Data2G
} // namespace TextMessaging

#endif // TEXT_MESSAGING__DATA2G_LINK_H
