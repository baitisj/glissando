//=========================================================================
// Name:            Data2GTransport.h
// Purpose:         Carries chat through a separately running data2g-host.
//                  Broadcasts, and anything else for more than one station,
//                  go to the GLISS broadcast group on its KISS port, in the
//                  mode the Glissando tempo maps to. Messages and pings for a
//                  single station go through a connected (ARQ) session with
//                  it, which data2g-host negotiates, rate-shifts and
//                  acknowledges; its acknowledgements settle each message.
//                  Files go through such a session too, beside the chat.
//
// data2g-host owns the sound card and the PTT; this only talks TCP to it.
// The operator starts it. Nothing of Data2G is built into this program.
//=========================================================================

#ifndef TEXT_MESSAGING__DATA2G_TRANSPORT_H
#define TEXT_MESSAGING__DATA2G_TRANSPORT_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Data2GFileTransfer.h"
#include "Data2GLink.h"
#include "FrameCodec.h"
#include "TextMessagingProtocol.h"

namespace TextMessaging
{

class Data2GTransport : public ITextMessagingTransport
{
public:
    struct Settings
    {
        std::string host = "127.0.0.1";
        int kissPort = Data2G::DEFAULT_KISS_PORT;
        int commandPort = Data2G::DEFAULT_COMMAND_PORT; // session data on the next port

        // Directed traffic through connected sessions; off, everything goes
        // to the GLISS group.
        bool useSessions = true;
    };

    struct Status
    {
        bool running = false;           // start() called, stop() not
        bool kissConnected = false;
        bool commandConnected = false;
        int groupPort = 0;              // the GLISS group's KISS port, 0 before it opens
        bool transmitting = false;      // data2g-host reports PTT on
        bool channelBusy = false;
        std::string groupMode;          // the mode GLISS bursts go out in now
        std::string mode;               // the submode of the last burst sent
        std::string sessionPeer;        // a connected session's far end
        bool sessionConnecting = false;
        int sessionWaiting = 0;         // messages for sessions not yet acknowledged
        int64_t sessionUnacked = 0;     // data2g-host's last BUFFER in the session
        bool sessionUnackedExact = false; // that BUFFER counts bytes (PR #51 host), not just "some"
        std::string error;              // why the last connection failed
    };

    // viaSession: it came through a connected session, which has already
    // acknowledged it to the sender.
    using FrameCallback = std::function<void(const Frame& frame, float snr, bool viaSession)>;
    using LogFunction = std::function<void(const std::string& line)>;

    Data2GTransport();
    ~Data2GTransport() override;

    Data2GTransport(const Data2GTransport&) = delete;
    Data2GTransport& operator=(const Data2GTransport&) = delete;

    // Frames arrive on the transport's own thread. Set before start().
    void setFrameCallback(FrameCallback callback);
    void setLogFunction(LogFunction log);

    // Our callsign, for MYCALL, BCAST FROM and sessions. A change while
    // connected reopens the command connection under the new one.
    void setMyCallsign(const std::string& callsign);

    // The Glissando tempo set now (1 Adagio .. 5 Duet), which a keying with
    // no tempo of its own goes out at.
    void setGear(int gear);

    // Connects, and keeps reconnecting while data2g-host is not there.
    // Calling it again with other settings reconnects with those.
    void start(const Settings& settings);
    void stop();

    Status status() const;

    // The mode a tempo maps to on this host, and the chat timers for the
    // tempo set now.
    Data2G::ModeInfo modeForGear(int gear) const;
    AirTiming airTiming() const;

    // Group keyings: broadcasts and the protocol's replies.
    bool transmit(const std::vector<OutgoingBurst>& bursts) override;
    bool isTransmitting() const override;
    bool isChannelBusy() const override;
    // data2g-host waits for a clear channel, and holds the group back
    // during a session, by itself.
    bool pacesItself() const override { return true; }
    bool withdrawKeying() override;
    double airTimeScale(int gear) const override;

    // Session keyings: with sessions on, a message or ping for one station
    // that has not lately refused one. Each is reported Delivered once the
    // far end's modem has acknowledged all of it, Failed if the session is
    // lost first, and NotTaken if no session could be opened (that station
    // then gets the group for NO_SESSION_HOLD_MS).
    bool deliversReliablyTo(const std::string& destination) const override;
    bool transmitReliably(const std::vector<OutgoingBurst>& bursts, uint64_t keyingId) override;
    std::vector<KeyingReport> takeKeyingReports() override;
    bool withdrawReliably(uint64_t keyingId) override;
    bool releaseStation(const std::string& destination) override;

    // Files for one station, through a session with it (docs/DATA2G.md):
    // possible with sessions on and data2g-host's command and data ports
    // up. A file is offered once a session with the station is open and
    // the file before it is done; the session stays open while anything
    // is under way in it. The rest is the engine's (Data2GFileTransfer.h).
    bool sendsFiles() const;
    uint64_t sendFile(const std::string& destination, const std::string& path, std::string& error);
    bool acceptFile(uint64_t id, const std::string& path, std::string& error);
    bool declineFile(uint64_t id);
    bool cancelFile(uint64_t id);
    void setFileAutoAccept(const std::string& folder, const std::vector<std::string>& calls);
    std::vector<Data2G::FileTransfer> fileTransfers() const;
    uint64_t fileTransferChanges() const;

    // The next piece of a file is written once no more than this much of
    // what went before is unacknowledged, so the modem always has the
    // rest of a piece to send while chat typed meanwhile, or a Cancel,
    // waits at most that long behind it.
    static constexpr uint64_t FILE_PIECE_LOW_WATER = Data2G::FILE_PIECE_BYTES / 4;

    // Test hook: the clock the keying and session timers read.
    void setClock(std::function<uint64_t()> monotonicMs);

    // A group keying data2g-host never puts on the air (it holds broadcasts
    // during a session, and while the channel is busy) is given up after
    // this long.
    static constexpr uint64_t NOT_SENT_TIMEOUT_MS = 120000;

    // A session is closed once nothing has gone either way in it for this
    // long and nothing of ours is waiting on it, so the group can be heard
    // again: long enough for the far end to answer in the same session.
    static constexpr uint64_t SESSION_IDLE_MS = 45000;

    // A station that would not take a session is sent to through the group
    // for this long before a session is tried again.
    static constexpr uint64_t NO_SESSION_HOLD_MS = 10 * 60 * 1000;

    // data2g-host gives up on a CONNECT by itself (five calls, the later ones
    // in its most robust mode); this only covers a host that never answers.
    static constexpr uint64_t CONNECT_TIMEOUT_MS = 180000;

    // A session keying still waiting for its session after this long (the
    // channel taken by somebody else's session, say) goes to the group.
    static constexpr uint64_t SESSION_WAIT_LIMIT_MS = 10 * 60 * 1000;

private:
    // A keying from transmit() to the moment it has left the transmitter.
    struct Keying
    {
        std::vector<OutgoingBurst> bursts;
        int gear = 0;
        uint64_t queuedAtMs = 0;
        uint64_t heldSinceMs = 0;   // NOT_SENT_TIMEOUT runs from here: not while a session holds the group

        enum class Stage
        {
            Waiting,        // for the group's mode, or a session to end
            Sent,           // written; waiting for data2g-host to transmit it
            Done,
        } stage = Stage::Waiting;

        std::set<uint16_t> tags;    // ACKMODE tags not yet reported transmitted
    };

    // A keying for a session, from transmitReliably() to its report.
    struct SessionKeying
    {
        uint64_t id = 0;
        std::string peer;
        std::vector<uint8_t> bytes;     // the frames as the session stream carries them
        uint64_t queuedAtMs = 0;
        bool written = false;
        uint64_t endOffset = 0;         // sessionWritten_ once its last byte was written
    };

    enum class SessionState
    {
        None,
        Connecting,
        Connected,
        Disconnecting,
    };

    void run(Settings settings);
    void deliver(const std::vector<uint8_t>& bytes, bool viaSession);
    bool sessionPossibleLocked(const std::string& call) const;
    bool filesPossibleLocked(const std::string& call) const;
    void reportLocked(const std::string& peer, bool writtenOnly, KeyingReport::Result result);
    void bufferLocked(int64_t count);
    void settleSessionLocked();
    bool sessionWritableLocked() const;
    void wroteLocked(uint64_t bytes);
    void log(const std::string& line);
    uint64_t now() const;

    mutable std::mutex mutex_;
    Status status_;
    std::vector<Data2G::ModeInfo> modes_;   // the host's MODES, once it has said
    std::string myCallsign_;
    int gear_;
    bool pttOn_;
    bool busy_;
    bool hasKeying_;
    Keying keying_;
    bool callsignChanged_;

    // Session bookkeeping, read by the thread under mutex_.
    SessionState session_;
    std::string sessionPeer_;
    bool sessionOurs_;
    uint64_t sessionActivityMs_;
    uint64_t connectStartedMs_;
    std::map<std::string, uint64_t> noSessionUntil_;
    bool useSessions_;
    bool dataConnected_;
    std::set<std::string> released_;    // stations whose session is to end at once
    bool sessionAborted_;               // ABORT sent for the session now ending

    // Session keyings in the order given, and what has become of them.
    std::deque<SessionKeying> sessionKeyings_;
    std::vector<KeyingReport> reports_;

    // What the far end's modem has acknowledged of what we wrote into the
    // session. data2g-host's BUFFER is the count of bytes it has read from
    // us that the far end has not yet acknowledged (exactly, for a client
    // that sent CHAT ON, from Data2G's PR #51; before that, 1 for "some").
    // It can't say how much it has read, so both are kept as the least
    // they can be: a BUFFER that rose by n means at least n more were read
    // (acknowledgements only lower it), and one of b means at least b more
    // were read than acknowledged. A message is settled once the bytes
    // acknowledged pass its end.
    uint64_t sessionWritten_;       // bytes written into this session
    uint64_t sessionRead_;          // at least this many read by data2g-host
    uint64_t sessionAcked_;         // at least this many acknowledged by the far end
    uint64_t lastWriteBytes_;       // the size of the last write
    bool bufferSeen_;               // a BUFFER since the last write showing data2g-host read it
    int bufferExact_;               // -1 not known yet; 1 BUFFER counts bytes; 0 it does not

    // Files through the session, under mutex_ like the rest.
    Data2G::FileTransferEngine files_;

    FrameCallback frameCallback_;
    LogFunction log_;
    std::function<uint64_t()> clock_;

    std::atomic<bool> stopping_;
    std::thread thread_;
};

} // namespace TextMessaging

#endif // TEXT_MESSAGING__DATA2G_TRANSPORT_H
