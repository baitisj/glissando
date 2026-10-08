//=========================================================================
// Name:            Data2GTransport.cpp
// Purpose:         Chat through an external data2g-host over TCP: the
//                  GLISS broadcast group on its KISS port, and connected
//                  sessions for single stations.
//=========================================================================

#include "Data2GTransport.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace TextMessaging
{

namespace
{

constexpr int SOCKET_CONNECT_TIMEOUT_MS = 1000;
constexpr int POLL_MS = 50;
constexpr uint64_t FIRST_RETRY_MS = 2000;
constexpr uint64_t MAX_RETRY_MS = 30000;
constexpr int SEND_TIMEOUT_SECONDS = 2;

// The few places where Winsock and POSIX sockets differ.
#if defined(_WIN32)
using ssize_t = long long;

void startSockets()
{
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}

int socketError() { return WSAGetLastError(); }
bool connectPending(int err) { return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS; }
bool interrupted(int err) { return err == WSAEINTR; }
std::string socketErrorText(int err)
{
    if (err == WSAETIMEDOUT) return "Connection timed out";
    if (err == WSAECONNREFUSED) return "Connection refused";
    if (err == WSAECONNRESET) return "Connection reset by peer";
    return "socket error " + std::to_string(err);
}
int pollSockets(pollfd* fds, unsigned long count, int timeoutMs) { return WSAPoll(fds, count, timeoutMs); }
void closeSocket(int fd) { closesocket((SOCKET)fd); }
void setBlocking(int fd, bool blocking)
{
    u_long nonBlocking = blocking ? 0 : 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nonBlocking);
}
void setSendTimeout(int fd, int seconds)
{
    DWORD timeoutMs = (DWORD)seconds * 1000;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
}
constexpr int SEND_FLAGS = 0;
constexpr int TIMED_OUT = WSAETIMEDOUT;
#else
void startSockets() {}
int socketError() { return errno; }
bool connectPending(int err) { return err == EINPROGRESS; }
bool interrupted(int err) { return err == EINTR; }
std::string socketErrorText(int err) { return std::strerror(err); }
int pollSockets(pollfd* fds, unsigned long count, int timeoutMs) { return ::poll(fds, (nfds_t)count, timeoutMs); }
void closeSocket(int fd) { close(fd); }
void setBlocking(int fd, bool blocking)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
}
void setSendTimeout(int fd, int seconds)
{
    timeval timeout{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
constexpr int TIMED_OUT = ETIMEDOUT;
#endif

uint64_t steadyMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Connects with a timeout, then hands back a blocking socket (reads only
// follow poll(), and writes carry a send timeout). -1 on failure, with the
// reason in error.
int connectTo(const std::string& host, int port, std::string& error)
{
    startSockets();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    std::string service = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &found);
    if (rc != 0)
    {
        error = host + ": " + gai_strerror(rc);
        return -1;
    }

    int fd = -1;
    for (addrinfo* ai = found; ai != nullptr; ai = ai->ai_next)
    {
#if defined(_WIN32)
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        fd = (int)s;
#else
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
#endif

        setBlocking(fd, false);
        rc = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
        int err = rc < 0 ? socketError() : 0;
        if (rc < 0 && connectPending(err))
        {
            pollfd p{};
            p.fd = fd;
            p.events = POLLOUT;
            int polled = pollSockets(&p, 1, SOCKET_CONNECT_TIMEOUT_MS);
            if (polled == 1)
            {
                int soError = 0;
                socklen_t len = sizeof(soError);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&soError, &len);
                rc = soError == 0 ? 0 : -1;
                err = soError;
            }
            else
            {
                err = polled == 0 ? TIMED_OUT : socketError();
                rc = -1;
            }
        }
        if (rc == 0)
        {
            setBlocking(fd, true);
            setSendTimeout(fd, SEND_TIMEOUT_SECONDS);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
            break;
        }
        error = host + ":" + service + ": " + socketErrorText(err);
        closeSocket(fd);
        fd = -1;
    }
    freeaddrinfo(found);
    return fd;
}

bool sendAll(int fd, const uint8_t* data, size_t length)
{
    while (length > 0)
    {
        ssize_t sent = send(fd, (const char*)data, (int)length, SEND_FLAGS);
        if (sent < 0 && interrupted(socketError())) continue;
        if (sent <= 0) return false;
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

// One TCP connection to data2g-host, with its reconnect schedule.
struct Socket
{
    const char* name;
    int port = 0;
    int fd = -1;
    uint64_t retryAtMs = 0;
    uint64_t retryDelayMs = FIRST_RETRY_MS;
    bool reportedDown = false;
};

// A command sent on the command port, waiting for its reply.
struct Command
{
    enum class Kind
    {
        Plain,
        BcastOpen,
        BcastMode,
        Modes,
        Connect,
        Disconnect,
    } kind = Kind::Plain;
    std::string line;
    std::string argument;   // BcastMode: the mode; Connect: the station
};

// The one station every burst of a keying is for, or empty.
std::string soleDestination(const std::vector<OutgoingBurst>& bursts)
{
    std::string destination;
    for (const OutgoingBurst& burst : bursts)
    {
        if (burst.destination.empty()) return std::string();
        std::string call = Data2G::commandCallsign(burst.destination);
        if (destination.empty()) destination = call;
        else if (call != destination) return std::string();
    }
    return destination;
}

} // namespace

Data2GTransport::Data2GTransport()
    : gear_(3)
    , pttOn_(false)
    , busy_(false)
    , hasKeying_(false)
    , callsignChanged_(false)
    , session_(SessionState::None)
    , sessionOurs_(false)
    , sessionActivityMs_(0)
    , connectStartedMs_(0)
    , useSessions_(false)
    , dataConnected_(false)
    , sessionAborted_(false)
    , sessionWritten_(0)
    , batchBytes_(0)
    , batchSeen_(false)
    , batchCounted_(false)
    , bufferExact_(-1)
    , clock_(steadyMs)
    , stopping_(false)
{
    // empty
}

Data2GTransport::~Data2GTransport()
{
    stop();
}

void Data2GTransport::setFrameCallback(FrameCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    frameCallback_ = std::move(callback);
}

void Data2GTransport::setLogFunction(LogFunction log)
{
    std::lock_guard<std::mutex> lock(mutex_);
    log_ = std::move(log);
}

void Data2GTransport::setMyCallsign(const std::string& callsign)
{
    std::string call = Data2G::commandCallsign(callsign);
    std::lock_guard<std::mutex> lock(mutex_);
    if (call == myCallsign_) return;
    myCallsign_ = call;
    callsignChanged_ = true;
}

void Data2GTransport::setGear(int gear)
{
    std::lock_guard<std::mutex> lock(mutex_);
    gear_ = gear;
}

void Data2GTransport::setClock(std::function<uint64_t()> monotonicMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(monotonicMs);
}

uint64_t Data2GTransport::now() const
{
    return clock_();
}

void Data2GTransport::log(const std::string& line)
{
    LogFunction log;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        log = log_;
    }
    if (log) log(line);
}

void Data2GTransport::start(const Settings& settings)
{
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = Status();
        status_.running = true;
        hasKeying_ = false;
        pttOn_ = false;
        busy_ = false;
        session_ = SessionState::None;
        sessionAborted_ = false;
        noSessionUntil_.clear();
        callsignChanged_ = false;
        useSessions_ = settings.useSessions;
        dataConnected_ = false;
        released_.clear();
        batchBytes_ = 0;
        batchCounted_ = false;
        batchSeen_ = false;
        bufferExact_ = -1;
    }
    stopping_ = false;
    thread_ = std::thread([this, settings]() { run(settings); });
}

void Data2GTransport::stop()
{
    if (!thread_.joinable()) return;
    stopping_ = true;
    thread_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    status_ = Status();
    hasKeying_ = false;
    pttOn_ = false;
    busy_ = false;
    session_ = SessionState::None;
    sessionAborted_ = false;
    dataConnected_ = false;

    // What the closed session was holding ends here, and the protocol is
    // told, as for a lost session: what went into it failed, and what had
    // not goes again however it can.
    for (const SessionKeying& k : sessionKeyings_)
    {
        reports_.push_back({k.id, k.written ? KeyingReport::Result::Failed : KeyingReport::Result::NotTaken});
    }
    sessionKeyings_.clear();
    batchBytes_ = 0;
}

Data2GTransport::Status Data2GTransport::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Status status = status_;
    status.transmitting = pttOn_;
    status.channelBusy = busy_;
    status.sessionPeer = session_ == SessionState::Connected ? sessionPeer_ : std::string();
    status.sessionConnecting = session_ == SessionState::Connecting;
    status.sessionWaiting = (int)sessionKeyings_.size();
    status.sessionUnackedExact = bufferExact_ == 1;
    return status;
}

Data2G::ModeInfo Data2GTransport::modeForGear(int gear) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Data2G::modeForGear(gear, modes_);
}

AirTiming Data2GTransport::airTiming() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Data2G::airTiming(Data2G::modeForGear(gear_, modes_));
}

double Data2GTransport::airTimeScale(int gear) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> message(1, TEXT_FRAME_BYTES);
    double now = Data2G::modeForGear(gear_, modes_).burstSeconds(message);
    double then = Data2G::modeForGear(gear, modes_).burstSeconds(message);
    return now > 0.0 && then > 0.0 ? then / now : 1.0;
}

bool Data2GTransport::transmit(const std::vector<OutgoingBurst>& bursts)
{
    if (bursts.empty()) return false;
    for (const OutgoingBurst& burst : bursts)
    {
        if (burst.frame.empty() || burst.frame.size() > 255) return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (hasKeying_ || !status_.kissConnected || !status_.commandConnected) return false;

    keying_ = Keying();
    keying_.bursts = bursts;
    keying_.gear = bursts.front().gear != 0 ? bursts.front().gear : gear_;
    keying_.queuedAtMs = now();
    keying_.heldSinceMs = keying_.queuedAtMs;
    hasKeying_ = true;
    return true;
}

// Sessions on, data2g-host's command and data ports up, a callsign to call
// from, and the station not lately found not to take a session.
bool Data2GTransport::sessionPossibleLocked(const std::string& call) const
{
    if (!useSessions_ || !status_.commandConnected || !dataConnected_) return false;
    if (myCallsign_.empty() || call.empty() || call == myCallsign_) return false;

    // A session already open with it, whoever opened it, takes it whatever
    // an earlier unanswered call said.
    if (session_ == SessionState::Connected && sessionPeer_ == call) return true;

    auto hold = noSessionUntil_.find(call);
    return hold == noSessionUntil_.end() || now() >= hold->second;
}

bool Data2GTransport::deliversReliablyTo(const std::string& destination) const
{
    std::string call = Data2G::commandCallsign(destination);
    std::lock_guard<std::mutex> lock(mutex_);
    return sessionPossibleLocked(call);
}

bool Data2GTransport::transmitReliably(const std::vector<OutgoingBurst>& bursts, uint64_t keyingId)
{
    if (bursts.empty()) return false;
    for (const OutgoingBurst& burst : bursts)
    {
        if (burst.frame.empty() || burst.frame.size() > 255) return false;
    }

    std::string peer = soleDestination(bursts);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sessionPossibleLocked(peer)) return false;

    SessionKeying keying;
    keying.id = keyingId;
    keying.peer = peer;
    keying.queuedAtMs = now();
    for (const OutgoingBurst& burst : bursts)
    {
        std::vector<uint8_t> framed = Data2G::streamEncode(burst.frame);
        keying.bytes.insert(keying.bytes.end(), framed.begin(), framed.end());
    }
    sessionKeyings_.push_back(std::move(keying));
    return true;
}

bool Data2GTransport::withdrawReliably(uint64_t keyingId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(sessionKeyings_.begin(), sessionKeyings_.end(),
                           [&](const SessionKeying& k) { return k.id == keyingId; });
    if (it == sessionKeyings_.end() || it->written) return false;
    sessionKeyings_.erase(it);
    return true;
}

std::vector<KeyingReport> Data2GTransport::takeKeyingReports()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<KeyingReport> reports;
    reports.swap(reports_);
    return reports;
}

// The operator let go of the station: what is held for it goes without a
// report (the protocol drops the messages itself), a group keying that is
// only for it and has not been written yet goes too, and the thread ends
// any session with it.
bool Data2GTransport::releaseStation(const std::string& destination)
{
    std::string peer = Data2G::commandCallsign(destination);
    std::lock_guard<std::mutex> lock(mutex_);
    if (peer.empty()) return true;

    bool written = false;
    for (auto it = sessionKeyings_.begin(); it != sessionKeyings_.end();)
    {
        if (it->peer != peer)
        {
            ++it;
            continue;
        }
        written = written || it->written;
        it = sessionKeyings_.erase(it);
    }
    if (written)
    {
        batchBytes_ = 0;
        batchCounted_ = false;
        batchSeen_ = false;
    }

    if (hasKeying_ && keying_.stage == Keying::Stage::Waiting && soleDestination(keying_.bursts) == peer)
    {
        hasKeying_ = false;
    }

    released_.insert(peer);
    return true;
}

// Reports every session keying for the station, those written into its
// session or those not, with the same result.
void Data2GTransport::reportLocked(const std::string& peer, bool written, KeyingReport::Result result)
{
    for (auto it = sessionKeyings_.begin(); it != sessionKeyings_.end();)
    {
        if (it->peer != peer || it->written != written)
        {
            ++it;
            continue;
        }
        reports_.push_back({it->id, result});
        it = sessionKeyings_.erase(it);
    }
    if (written) batchBytes_ = 0;
}

// BUFFER, while connected: how many bytes of ours the far end's modem has
// not acknowledged. The written keyings are settled from the front as the
// count falls past where each one ends. A count from before data2g-host had
// read the batch still says 0 (nothing was outstanding when it was
// written), so the batch counts only once a BUFFER has included it, which
// data2g-host sends straight after reading it. That first count also says
// whether the host counts bytes at all: one that only says 1 for "some" is
// below the size of the batch, and then the batch settles only at 0.
void Data2GTransport::settleSessionLocked()
{
    if (batchBytes_ == 0) return;
    int64_t unacked = status_.sessionUnacked;

    if (!batchCounted_)
    {
        if (unacked > 0)
        {
            batchSeen_ = true;
            if (bufferExact_ < 0 && unacked == 1 && batchBytes_ > 1)
            {
                bufferExact_ = 0;
                if (log_)
                {
                    log_("Data2G reports only whether session bytes are unacknowledged: messages "
                         "written together are settled together");
                }
            }
            if (bufferExact_ != 0)
            {
                // A host further away may read the batch in pieces, each
                // answered with a BUFFER: nothing is settled by count until
                // one has taken in the whole of it.
                if ((uint64_t)unacked < batchBytes_) return;
                if (bufferExact_ < 0 && log_) log_("Data2G counts unacknowledged session bytes: each message is settled as its own");
                bufferExact_ = 1;
            }
            batchCounted_ = true;
        }
        else if (!batchSeen_)
        {
            return;
        }
    }

    bool anyLeft = false;
    for (auto it = sessionKeyings_.begin(); it != sessionKeyings_.end();)
    {
        if (!it->written)
        {
            ++it;
            continue;
        }
        bool acked = unacked == 0 || (bufferExact_ == 1 && (uint64_t)unacked <= sessionWritten_ - it->endOffset);
        if (!acked)
        {
            anyLeft = true;
            break;
        }
        reports_.push_back({it->id, KeyingReport::Result::Delivered});
        it = sessionKeyings_.erase(it);
    }
    if (!anyLeft)
    {
        batchBytes_ = 0;
        batchCounted_ = false;
        batchSeen_ = false;
    }
}

bool Data2GTransport::isTransmitting() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return hasKeying_ || pttOn_;
}

bool Data2GTransport::isChannelBusy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.commandConnected && busy_;
}

void Data2GTransport::run(Settings settings)
{
    Socket kiss{"KISS", settings.kissPort};
    Socket command{"command", settings.commandPort};
    Socket data{"session data", settings.commandPort + 1};
    Data2G::KissDecoder kissDecoder;
    Data2G::LineSplitter lines;
    Data2G::StreamDecoder stream;

    std::deque<Command> commandQueue;
    bool awaitingReply = false;
    Command inFlight;
    std::vector<Data2G::ModeInfo> modeLines;
    std::string triedMode;      // a BCAST MODE refused: send in the port's mode
    uint16_t nextTag = 1;
    std::string myCall;

    auto sendBytes = [&](Socket& socket, const std::vector<uint8_t>& bytes) {
        return socket.fd >= 0 && sendAll(socket.fd, bytes.data(), bytes.size());
    };

    auto queueCommand = [&](Command::Kind kind, const std::string& line, const std::string& argument = "") {
        Command c;
        c.kind = kind;
        c.line = line;
        c.argument = argument;
        commandQueue.push_back(c);
    };

    auto commandPending = [&](Command::Kind kind) {
        if (awaitingReply && inFlight.kind == kind) return true;
        for (const Command& c : commandQueue)
        {
            if (c.kind == kind) return true;
        }
        return false;
    };

    auto closeSocket_ = [&](Socket& socket, const std::string& why) {
        if (socket.fd < 0) return;
        closeSocket(socket.fd);
        socket.fd = -1;
        socket.retryAtMs = steadyMs() + FIRST_RETRY_MS;
        socket.retryDelayMs = FIRST_RETRY_MS;
        log(std::string("Data2G ") + socket.name + " connection closed: " + why);

        if (&socket == &kiss)
        {
            kissDecoder.reset();
            std::lock_guard<std::mutex> lock(mutex_);
            status_.kissConnected = false;
        }
        else if (&socket == &data)
        {
            stream.reset();
            std::lock_guard<std::mutex> lock(mutex_);
            dataConnected_ = false;
            // What was written is in the host now, but its BUFFER can no
            // longer be matched to it; nothing more can be written.
            // The session goes too: its count of unacknowledged bytes covers
            // what was written before, and would settle what comes after.
            if (session_ == SessionState::Connected)
            {
                reportLocked(sessionPeer_, true, KeyingReport::Result::Failed);
                released_.insert(sessionPeer_);
            }
        }
        else
        {
            // data2g-host closes our group and any session with the client.
            lines.reset();
            commandQueue.clear();
            awaitingReply = false;
            triedMode.clear();
            std::lock_guard<std::mutex> lock(mutex_);
            status_.commandConnected = false;
            status_.groupPort = 0;
            status_.groupMode.clear();
            pttOn_ = false;
            busy_ = false;
            if (session_ == SessionState::Connected) reportLocked(sessionPeer_, true, KeyingReport::Result::Failed);
            session_ = SessionState::None;
            sessionAborted_ = false;
            bufferExact_ = -1;
            if (hasKeying_ && keying_.stage == Keying::Stage::Sent) hasKeying_ = false;
            else if (hasKeying_) keying_.stage = Keying::Stage::Waiting;
        }
    };

    auto tryConnect = [&](Socket& socket) {
        uint64_t t = steadyMs();
        if (socket.fd >= 0 || t < socket.retryAtMs) return;

        std::string error;
        int fd = connectTo(settings.host, socket.port, error);
        if (fd < 0)
        {
            socket.retryAtMs = t + socket.retryDelayMs;
            socket.retryDelayMs = std::min(MAX_RETRY_MS, socket.retryDelayMs * 2);
            if (!socket.reportedDown)
            {
                log(std::string("Data2G ") + socket.name + " port not reachable (" + error +
                    "); is data2g-host running? Retrying.");
                socket.reportedDown = true;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            status_.error = error;
            return;
        }

        socket.fd = fd;
        socket.reportedDown = false;
        log(std::string("Data2G ") + socket.name + " port connected at " + settings.host + ":" +
            std::to_string(socket.port));

        if (&socket == &kiss)
        {
            kissDecoder.reset();
            std::lock_guard<std::mutex> lock(mutex_);
            status_.kissConnected = true;
        }
        else if (&socket == &data)
        {
            stream.reset();
            std::lock_guard<std::mutex> lock(mutex_);
            dataConnected_ = true;
        }
        else
        {
            lines.reset();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.commandConnected = true;
                myCall = myCallsign_;
                callsignChanged_ = false;
            }
            // Who we are, sessions answered, our group opened (naming us in
            // every burst), and the modes this host offers.
            if (!myCall.empty())
            {
                queueCommand(Command::Kind::Plain, "MYCALL " + myCall);
                if (settings.useSessions)
                {
                    queueCommand(Command::Kind::Plain, "CHAT ON");
                    queueCommand(Command::Kind::Plain, "LISTEN ON");
                }
            }
            std::string open = std::string("BCAST OPEN ") + Data2G::GROUP;
            if (!myCall.empty()) open += " FROM " + myCall;
            queueCommand(Command::Kind::BcastOpen, open);
            queueCommand(Command::Kind::Modes, "MODES");
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.kissConnected && status_.commandConnected) status_.error.clear();
    };

    auto handleReply = [&](const Data2G::CommandEvent& event) {
        Command done = inFlight;
        awaitingReply = false;
        bool ok = event.type != Data2G::CommandEvent::Type::Wrong;

        switch (done.kind)
        {
            case Command::Kind::Plain:
                if (!ok) log("Data2G refused \"" + done.line + "\"");
                break;
            case Command::Kind::BcastOpen:
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (event.type == Data2G::CommandEvent::Type::BcastPort) status_.groupPort = event.number;
                break;
            }
            case Command::Kind::BcastMode:
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ok) status_.groupMode = done.argument;
                else triedMode = done.argument;
                break;
            }
            case Command::Kind::Modes:
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!modeLines.empty()) modes_ = modeLines;
                modeLines.clear();
                break;
            }
            case Command::Kind::Connect:
                if (!ok)
                {
                    // A session is already under way at the host: these go
                    // to the group, as for a station that does not answer.
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (session_ == SessionState::Connecting) session_ = SessionState::None;
                    noSessionUntil_[done.argument] = now() + NO_SESSION_HOLD_MS;
                    reportLocked(done.argument, false, KeyingReport::Result::NotTaken);
                }
                break;
            case Command::Kind::Disconnect:
                break;
        }
        if (!ok && done.kind == Command::Kind::BcastOpen) log("Data2G refused to open the GLISS group");
    };

    auto handleCommandLine = [&](const std::string& line) {
        Data2G::CommandEvent event = Data2G::parseCommandLine(line);
        using Type = Data2G::CommandEvent::Type;

        bool reply = event.type == Type::Ok || event.type == Type::Wrong || event.type == Type::BcastPort;
        if (reply)
        {
            if (awaitingReply) handleReply(event);
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        switch (event.type)
        {
            case Type::ModeLine:
                if (awaitingReply && inFlight.kind == Command::Kind::Modes) modeLines.push_back(event.modeInfo);
                break;
            case Type::Ptt:
                // Not session activity: data2g-host keys for its idle polls
                // too, which would keep a quiet session from ever closing.
                pttOn_ = event.on;
                break;
            case Type::Busy:
                busy_ = event.on;
                break;
            case Type::Mode:
                status_.mode = event.text;
                break;
            case Type::BcastDropped:
                if (event.number == status_.groupPort && hasKeying_ && keying_.stage == Keying::Stage::Sent)
                {
                    hasKeying_ = false;
                    if (log_) log_("Data2G dropped " + std::to_string(event.count) + " chat frame(s) unsent");
                }
                break;
            case Type::Connected:
            {
                // "CONNECTED caller called": the far end is whichever is not us.
                bool weCalled = event.text == myCall;
                sessionPeer_ = weCalled ? event.peer : event.text;
                sessionOurs_ = weCalled;
                session_ = SessionState::Connected;
                noSessionUntil_.erase(sessionPeer_);
                sessionActivityMs_ = now();
                sessionWritten_ = 0;
                batchBytes_ = 0;
                batchCounted_ = false;
                batchSeen_ = false;
                status_.sessionUnacked = 0;
                stream.reset();
                if (log_) log_("Data2G session with " + sessionPeer_ + (weCalled ? " (we called)" : " (they called)"));
                break;
            }
            case Type::Disconnected:
            {
                bool wasConnecting = session_ == SessionState::Connecting;
                std::string peer = sessionPeer_;
                session_ = SessionState::None;
                sessionAborted_ = false;
                sessionPeer_.clear();
                if (wasConnecting)
                {
                    // The station did not take a session: what was for it
                    // goes to the group, and so does the next for a while.
                    noSessionUntil_[peer] = now() + NO_SESSION_HOLD_MS;
                    reportLocked(peer, false, KeyingReport::Result::NotTaken);
                    if (log_) log_("Data2G: no session with " + peer + "; sending to the GLISS group");
                }
                else if (!peer.empty())
                {
                    // Lost, or closed by the far end, with something of ours
                    // not acknowledged: the host has thrown it away. What was
                    // not written yet waits for the next session.
                    bool lost = std::any_of(sessionKeyings_.begin(), sessionKeyings_.end(),
                                            [](const SessionKeying& k) { return k.written; });
                    reportLocked(peer, true, KeyingReport::Result::Failed);
                    if (lost && log_) log_("Data2G: the session with " + peer + " ended before it acknowledged everything");
                }
                break;
            }
            case Type::Buffer:
                // One after the session has gone is about the next session,
                // which data2g-host starts listening for at once.
                if (session_ != SessionState::Connected) break;
                if (event.count != status_.sessionUnacked) sessionActivityMs_ = now();
                status_.sessionUnacked = event.count;
                settleSessionLocked();
                break;
            default:
                break;
        }
    };

    auto handleKissFrame = [&](const Data2G::KissFrame& frame) {
        int groupPort;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            groupPort = status_.groupPort;
        }
        if (groupPort == 0 || frame.port != groupPort) return;

        if (frame.command == Data2G::KISS_ACKMODE)
        {
            if (frame.payload.size() < 2) return;
            uint16_t tag = (uint16_t)((frame.payload[0] << 8) | frame.payload[1]);
            std::lock_guard<std::mutex> lock(mutex_);
            if (hasKeying_ && keying_.stage == Keying::Stage::Sent)
            {
                keying_.tags.erase(tag);
                if (keying_.tags.empty()) hasKeying_ = false;
            }
            return;
        }

        deliver(frame.payload, false);
    };

    // Moves the session keyings and the group keying on as far as they can
    // go now.
    auto driveKeying = [&]() {
        std::unique_lock<std::mutex> lock(mutex_);
        uint64_t t = now();

        if (callsignChanged_ && command.fd >= 0)
        {
            lock.unlock();
            closeSocket_(command, "callsign changed");
            return;
        }

        // A station the operator let go of: its session ends now. A clean
        // DISCONNECT tells the far end, and data2g-host sends it at once
        // when nothing of ours is unacknowledged; otherwise it would wait
        // for the acknowledgements, so ABORT, which drops the session here
        // and leaves the far end to find it gone.
        if (!released_.empty())
        {
            std::set<std::string> released;
            released.swap(released_);
            if (session_ != SessionState::None && released.count(sessionPeer_) != 0 && !sessionAborted_)
            {
                bool clean = session_ == SessionState::Connected && status_.sessionUnacked == 0;
                if (session_ != SessionState::Disconnecting || !clean)
                {
                    session_ = SessionState::Disconnecting;
                    sessionAborted_ = !clean;
                    queueCommand(Command::Kind::Disconnect, clean ? "DISCONNECT" : "ABORT");
                    if (log_) log_("Data2G: station deselected; ending the session with " + sessionPeer_);
                }
            }
        }

        // A session keying that has waited too long for its session (the
        // channel held by sessions of other stations) goes to the group.
        for (auto it = sessionKeyings_.begin(); it != sessionKeyings_.end();)
        {
            bool calling = (session_ == SessionState::Connecting || session_ == SessionState::Connected) &&
                           sessionPeer_ == it->peer;
            if (it->written || calling || t - it->queuedAtMs < SESSION_WAIT_LIMIT_MS)
            {
                ++it;
                continue;
            }
            noSessionUntil_[it->peer] = t + NO_SESSION_HOLD_MS;
            reports_.push_back({it->id, KeyingReport::Result::NotTaken});
            if (log_) log_("Data2G: no session with " + it->peer + " in time; sending to the GLISS group");
            it = sessionKeyings_.erase(it);
        }

        bool groupWaiting = hasKeying_ && keying_.stage == Keying::Stage::Waiting;
        auto unwrittenFor = [&](const std::string& peer, bool others) {
            return std::any_of(sessionKeyings_.begin(), sessionKeyings_.end(), [&](const SessionKeying& k)
                               { return !k.written && (others ? k.peer != peer : k.peer == peer); });
        };

        if (session_ == SessionState::Connected)
        {
            // Everything for the far end goes in as soon as what went before
            // it is acknowledged, all in one write, so the modem can carry it
            // in as few turns as it likes.
            if (batchBytes_ == 0 && data.fd >= 0 && unwrittenFor(sessionPeer_, false))
            {
                std::vector<uint8_t> bytes;
                for (SessionKeying& k : sessionKeyings_)
                {
                    if (k.written || k.peer != sessionPeer_) continue;
                    bytes.insert(bytes.end(), k.bytes.begin(), k.bytes.end());
                    sessionWritten_ += k.bytes.size();
                    k.written = true;
                    k.endOffset = sessionWritten_;
                }
                batchBytes_ = bytes.size();
                batchCounted_ = false;
                batchSeen_ = false;
                sessionActivityMs_ = t;
                lock.unlock();
                if (!sendBytes(data, bytes)) closeSocket_(data, "write failed");
                return;
            }

            // Nothing of ours left in it: one we opened closes once it has
            // been quiet a while, or at once if something else is waiting
            // (the group gets nothing out during a session); one the far end
            // opened is left to it, unless something else has waited a while.
            bool forPeer = std::any_of(sessionKeyings_.begin(), sessionKeyings_.end(),
                                       [&](const SessionKeying& k) { return k.peer == sessionPeer_; });
            bool otherWork = groupWaiting || unwrittenFor(sessionPeer_, true);
            bool idle = t - sessionActivityMs_ >= SESSION_IDLE_MS;
            bool close = !forPeer && (sessionOurs_ ? otherWork || idle : otherWork && idle);
            if (close && !commandPending(Command::Kind::Disconnect))
            {
                session_ = SessionState::Disconnecting;
                queueCommand(Command::Kind::Disconnect, "DISCONNECT");
                if (log_) log_("Data2G: closing the session with " + sessionPeer_);
            }
        }
        else if (session_ == SessionState::Connecting)
        {
            if (t - connectStartedMs_ >= CONNECT_TIMEOUT_MS && !commandPending(Command::Kind::Disconnect))
            {
                // ABORT answers DISCONNECTED at once.
                session_ = SessionState::Disconnecting;
                sessionAborted_ = true;
                noSessionUntil_[sessionPeer_] = t + NO_SESSION_HOLD_MS;
                reportLocked(sessionPeer_, false, KeyingReport::Result::NotTaken);
                queueCommand(Command::Kind::Disconnect, "ABORT");
                if (log_) log_("Data2G: no answer from " + sessionPeer_ + "; sending to the GLISS group");
            }
        }
        else if (session_ == SessionState::None && !sessionKeyings_.empty())
        {
            // The next station to call, unless the group keying has waited
            // longer.
            const SessionKeying& next = sessionKeyings_.front();
            bool groupFirst = groupWaiting && keying_.queuedAtMs <= next.queuedAtMs;
            if (!groupFirst && data.fd >= 0 && !myCall.empty() && !commandPending(Command::Kind::Connect))
            {
                std::string peer = next.peer;
                session_ = SessionState::Connecting;
                sessionPeer_ = peer;
                connectStartedMs_ = t;
                queueCommand(Command::Kind::Connect, "CONNECT " + myCall + " " + peer, peer);
                if (log_) log_("Data2G: calling " + peer + " for a session");
                return;
            }
        }

        if (!hasKeying_) return;

        if (keying_.stage == Keying::Stage::Sent)
        {
            if (keying_.tags.empty()) hasKeying_ = false;
            else if (t - keying_.queuedAtMs >= NOT_SENT_TIMEOUT_MS)
            {
                hasKeying_ = false;
                if (log_) log_("Data2G never reported the chat keying sent; giving up on it");
            }
            return;
        }

        // A session holds the group back for as long as it takes, so the
        // time it does is not counted against the keying.
        if (session_ != SessionState::None) keying_.heldSinceMs = t;
        if (t - keying_.heldSinceMs >= NOT_SENT_TIMEOUT_MS)
        {
            hasKeying_ = false;
            if (log_) log_("Data2G: the chat keying could not be sent in time");
            return;
        }

        // The GLISS group, in the tempo's mode, once no session is open:
        // data2g-host sends no broadcasts during one.
        if (status_.groupPort == 0 || kiss.fd < 0) return;
        if (session_ != SessionState::None) return;

        Data2G::ModeInfo mode = Data2G::modeForGear(keying_.gear, modes_);
        if (mode.valid() && mode.name != status_.groupMode && mode.name != triedMode)
        {
            if (!commandPending(Command::Kind::BcastMode))
            {
                queueCommand(Command::Kind::BcastMode,
                             "BCAST MODE " + std::to_string(status_.groupPort) + " " + mode.name, mode.name);
            }
            return;
        }
        if (commandPending(Command::Kind::BcastMode)) return;

        std::vector<uint8_t> bytes;
        for (const OutgoingBurst& burst : keying_.bursts)
        {
            uint16_t tag = nextTag++;
            if (nextTag == 0) nextTag = 1;
            keying_.tags.insert(tag);
            std::vector<uint8_t> frame = Data2G::kissEncodeAckMode(status_.groupPort, tag, burst.frame);
            bytes.insert(bytes.end(), frame.begin(), frame.end());
        }
        keying_.stage = Keying::Stage::Sent;
        lock.unlock();
        if (!sendBytes(kiss, bytes)) closeSocket_(kiss, "write failed");
    };

    std::vector<Data2G::KissFrame> kissFrames;
    std::vector<std::string> commandLines;
    std::vector<std::vector<uint8_t>> streamFrames;
    uint8_t buffer[4096];

    while (!stopping_)
    {
        tryConnect(kiss);
        tryConnect(command);
        if (settings.useSessions) tryConnect(data);

        driveKeying();

        // One command at a time: data2g-host's replies carry no tag.
        if (!awaitingReply && !commandQueue.empty() && command.fd >= 0)
        {
            inFlight = commandQueue.front();
            commandQueue.pop_front();
            if (inFlight.kind == Command::Kind::Modes) modeLines.clear();
            std::string line = inFlight.line + "\r";
            awaitingReply = true;
            if (!sendBytes(command, std::vector<uint8_t>(line.begin(), line.end())))
            {
                closeSocket_(command, "write failed");
            }
        }

        pollfd fds[3];
        Socket* sockets[3];
        int count = 0;
        for (Socket* socket : {&kiss, &command, &data})
        {
            if (socket->fd < 0) continue;
            fds[count] = pollfd{};
            fds[count].fd = socket->fd;
            fds[count].events = POLLIN;
            sockets[count] = socket;
            count++;
        }

        if (count == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
            continue;
        }

        int ready = pollSockets(fds, (unsigned long)count, POLL_MS);
        if (ready <= 0) continue;

        for (int i = 0; i < count; i++)
        {
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
            Socket& socket = *sockets[i];
            if (socket.fd < 0) continue;
            ssize_t got = recv(socket.fd, (char*)buffer, sizeof(buffer), 0);
            if (got <= 0)
            {
                int err = got < 0 ? socketError() : 0;
                if (got < 0 && interrupted(err)) continue;
                closeSocket_(socket, got == 0 ? "closed by data2g-host" : socketErrorText(err));
                continue;
            }

            if (&socket == &kiss)
            {
                kissFrames.clear();
                kissDecoder.feed(buffer, (int)got, kissFrames);
                for (const auto& frame : kissFrames) handleKissFrame(frame);
            }
            else if (&socket == &command)
            {
                commandLines.clear();
                lines.feed((const char*)buffer, (int)got, commandLines);
                for (const auto& line : commandLines) handleCommandLine(line);
            }
            else
            {
                streamFrames.clear();
                bool ours = stream.feed(buffer, (int)got, streamFrames);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    sessionActivityMs_ = now();
                }
                for (const auto& frame : streamFrames) deliver(frame, true);
                if (!ours && !commandPending(Command::Kind::Disconnect))
                {
                    log("Data2G: a session that is not Glissando chat; closing it");
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        session_ = SessionState::Disconnecting;
                    }
                    queueCommand(Command::Kind::Disconnect, "DISCONNECT");
                }
            }
        }
    }

    closeSocket_(kiss, "stopped");
    closeSocket_(command, "stopped");
    closeSocket_(data, "stopped");
}

void Data2GTransport::deliver(const std::vector<uint8_t>& bytes, bool viaSession)
{
    Frame frame;
    if (!FrameCodec::decode(bytes.data(), (int)bytes.size(), frame)) return;

    FrameCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = frameCallback_;
    }
    // Data2G does not report a signal to noise ratio.
    if (callback) callback(frame, NAN, viaSession);
}

} // namespace TextMessaging
