//=========================================================================
// Name:            SnoopFeed.h
// Purpose:         Everything the station hears, whoever it was meant for.
//
// Written for Glissando. The protocol only takes in frames addressed to
// this station (and broadcasts); everything else is dropped on arrival. The
// feed sees every decoded frame before that happens, reassembles messages
// between other stations as well as our own, and hands both the frames and
// the finished messages to whoever is listening: the snooping window, and
// anything else that wants to show traffic as it is decoded.
//=========================================================================

#ifndef TEXT_MESSAGING__SNOOP_FEED_H
#define TEXT_MESSAGING__SNOOP_FEED_H

#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "FrameCodec.h"
#include "TextMessagingTypes.h"

namespace TextMessaging
{

// Which modem decoded the frame.
enum class SnoopSource
{
    Glissando,
    Data2G,
};

struct SnoopEvent
{
    enum class Kind
    {
        Frame,      // one decoded frame, as it arrives
        Message,    // every fragment of a message is in; text is the whole of it
    };

    Kind kind = Kind::Frame;
    uint64_t sequence = 0;          // counts up from 1 over the feed's life
    std::time_t timestamp = 0;
    SnoopSource source = SnoopSource::Glissando;
    FrameType frameType = FrameType::Broadcast;

    std::string origin;
    // The addressee's callsign when the feed has heard it (frames carry only
    // a CRC of it), "#" and the CRC in hex when not, empty for a broadcast.
    std::string destination;
    bool destinationKnown = false;
    bool broadcast = false;
    bool toMe = false;

    uint16_t airId = 0;
    uint8_t fragmentIndex = 0;
    uint8_t fragmentCount = 1;
    uint8_t burstsFollowing = 0;
    float snr = 0.0f;

    // A text frame: its fragment of the message. A Message event: the whole
    // message. Signalling frames: nothing (see the fields below).
    std::string text;

    // Text frames: which fragments of this message have been heard so far,
    // one bit each. Partial acknowledgements: the fragments the far end says
    // it holds.
    uint32_t fragmentsHeld = 0;

    // A text frame already heard in full once: the sender is retrying
    // because its acknowledgement did not arrive.
    bool repeat = false;

    // PingAck: the SNR the pinged station heard the ping at.
    bool hasReportedSnr = false;
    float reportedSnr = 0.0f;
};

// One line summary of a signalling frame or a fragment, e.g. "ACK #1234" or
// "fragment 2/5". Used by the window and the tests alike.
std::string describeSnoopFrame(const SnoopEvent& event);

class SnoopFeed
{
public:
    using Listener = std::function<void(const SnoopEvent&)>;

    // How many events recent() keeps for a window opened late.
    static constexpr size_t HISTORY = 500;

    // A message whose missing fragments have not turned up after this long
    // is given up on; its fragments were still shown as frames.
    static constexpr std::time_t REASSEMBLY_TIMEOUT_SECONDS = 30 * 60;

    SnoopFeed();

    // Frames from our own callsign are our transmissions looping back through
    // the radio and are ignored; our callsign also marks what was sent to us.
    void setMyCallsign(const std::string& callsign);

    // Teaches the feed a callsign it has not heard transmit, so that frames
    // addressed to it can name it (e.g. a station the operator added by hand).
    void addKnownCallsign(const std::string& callsign);

    // Any thread. Listeners are called on the calling thread, without the
    // feed's state locked, so they may call recent(); they must not add or
    // remove listeners. Once removeListener() returns, its listener is not
    // running and will not be called again.
    void onFrame(const Frame& frame, float snr, SnoopSource source, std::time_t now);

    // Returns an id for removeListener().
    int addListener(Listener listener);
    void removeListener(int id);

    // Oldest first.
    std::vector<SnoopEvent> recent() const;

private:
    struct Reassembly
    {
        std::vector<std::string> fragments;
        uint32_t heldMask = 0;
        float snrSum = 0.0f;
        int snrCount = 0;
        std::time_t lastHeard = 0;
        bool completed = false;
    };

    using Key = std::pair<std::string, uint16_t>;   // origin, air ID

    std::string resolveLocked(uint32_t crc, bool& known) const;
    void learnLocked(const std::string& callsign);
    void pruneLocked(std::time_t now);
    void recordLocked(SnoopEvent& event, std::vector<SnoopEvent>& out);

    mutable std::mutex mutex_;
    std::string myCallsign_;
    uint32_t myCallsignCrc_;
    std::map<uint32_t, std::string> callsignsByCrc_;
    std::map<Key, Reassembly> inbox_;
    std::deque<SnoopEvent> history_;
    uint64_t sequence_;

    mutable std::mutex listenersMutex_;     // held while listeners run
    std::map<int, Listener> listeners_;
    int nextListenerId_;
};

} // namespace TextMessaging

#endif // TEXT_MESSAGING__SNOOP_FEED_H
