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
    , sessionBuffer_(0)
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
        noSessionUntil_.clear();
        callsignChanged_ = false;
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
}

Data2GTransport::Status Data2GTransport::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Status status = status_;
    status.transmitting = pttOn_;
    status.channelBusy = busy_;
    status.sessionPeer = session_ == SessionState::Connected ? sessionPeer_ : std::string();
    status.sessionConnecting = session_ == SessionState::Connecting;
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
    keying_.destination = soleDestination(bursts);
    keying_.gear = bursts.front().gear != 0 ? bursts.front().gear : gear_;
    keying_.queuedAtMs = now();
    hasKeying_ = true;
    return true;
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
            session_ = SessionState::None;
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
                    std::lock_guard<std::mutex> lock(mutex_);
                    session_ = SessionState::None;
                    noSessionUntil_[done.argument] = now() + NO_SESSION_HOLD_MS;
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
                pttOn_ = event.on;
                if (session_ == SessionState::Connected) sessionActivityMs_ = now();
                if (hasKeying_ && keying_.stage == Keying::Stage::Sent && keying_.viaSession)
                {
                    if (event.on) keying_.sawPtt = true;
                    else if (keying_.sawPtt) keying_.pttCycled = true;
                }
                break;
            case Type::Busy:
                busy_ = event.on;
                break;
            case Type::Mode:
                status_.mode = event.text;
                break;
            case Type::BcastDropped:
                if (event.number == status_.groupPort && hasKeying_ && keying_.stage == Keying::Stage::Sent &&
                    !keying_.viaSession)
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
                sessionActivityMs_ = now();
                sessionBuffer_ = 0;
                stream.reset();
                if (log_) log_("Data2G session with " + sessionPeer_ + (weCalled ? " (we called)" : " (they called)"));
                break;
            }
            case Type::Disconnected:
            {
                bool wasConnecting = session_ == SessionState::Connecting;
                std::string peer = sessionPeer_;
                session_ = SessionState::None;
                sessionPeer_.clear();
                if (wasConnecting)
                {
                    // The station did not take a session: the keying goes to
                    // the group, and so will the next ones for a while.
                    noSessionUntil_[peer] = now() + NO_SESSION_HOLD_MS;
                    if (log_) log_("Data2G: no session with " + peer + "; sending to the GLISS group");
                }
                if (hasKeying_ && keying_.stage == Keying::Stage::Sent && keying_.viaSession) hasKeying_ = false;
                break;
            }
            case Type::Buffer:
                sessionBuffer_ = event.count;
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
            if (hasKeying_ && keying_.stage == Keying::Stage::Sent && !keying_.viaSession)
            {
                keying_.tags.erase(tag);
                if (keying_.tags.empty()) hasKeying_ = false;
            }
            return;
        }

        deliver(frame.payload);
    };

    // Moves the keying on as far as it can go now.
    auto driveKeying = [&]() {
        std::unique_lock<std::mutex> lock(mutex_);
        uint64_t t = now();

        if (callsignChanged_ && command.fd >= 0)
        {
            lock.unlock();
            closeSocket_(command, "callsign changed");
            return;
        }

        // A session we opened, gone quiet: close it, so broadcasts can go.
        bool keyingForPeer = hasKeying_ && keying_.destination == sessionPeer_ && settings.useSessions;
        if (session_ == SessionState::Connected && sessionOurs_ && !keyingForPeer && sessionBuffer_ == 0 &&
            (t - sessionActivityMs_ >= SESSION_IDLE_MS || (hasKeying_ && keying_.stage == Keying::Stage::Waiting)) &&
            !commandPending(Command::Kind::Disconnect))
        {
            session_ = SessionState::Disconnecting;
            queueCommand(Command::Kind::Disconnect, "DISCONNECT");
            if (log_) log_("Data2G: closing the session with " + sessionPeer_);
        }

        if (!hasKeying_) return;

        if (keying_.stage == Keying::Stage::Sent)
        {
            bool done = keying_.viaSession ? (keying_.pttCycled && sessionBuffer_ == 0) : keying_.tags.empty();
            if (done) hasKeying_ = false;
            else if (t - keying_.queuedAtMs >= NOT_SENT_TIMEOUT_MS)
            {
                hasKeying_ = false;
                if (log_) log_("Data2G never reported the chat keying sent; giving up on it");
            }
            return;
        }

        if (t - keying_.queuedAtMs >= NOT_SENT_TIMEOUT_MS)
        {
            hasKeying_ = false;
            if (log_) log_("Data2G: the chat keying could not be sent in time");
            return;
        }

        // A single station: through a session with it, unless it would not
        // take one lately.
        const std::string& peer = keying_.destination;
        bool viaSession = settings.useSessions && !peer.empty() && !myCall.empty() && data.fd >= 0;
        if (viaSession)
        {
            auto hold = noSessionUntil_.find(peer);
            if (hold != noSessionUntil_.end())
            {
                if (t < hold->second) viaSession = false;
                else noSessionUntil_.erase(hold);
            }
        }

        if (viaSession)
        {
            if (session_ == SessionState::Connected && sessionPeer_ == peer)
            {
                std::vector<uint8_t> bytes;
                for (const OutgoingBurst& burst : keying_.bursts)
                {
                    std::vector<uint8_t> framed = Data2G::streamEncode(burst.frame);
                    bytes.insert(bytes.end(), framed.begin(), framed.end());
                }
                keying_.stage = Keying::Stage::Sent;
                keying_.viaSession = true;
                keying_.sawPtt = pttOn_;
                sessionActivityMs_ = t;
                sessionBuffer_ = (int)bytes.size();
                lock.unlock();
                if (!sendBytes(data, bytes)) closeSocket_(data, "write failed");
                return;
            }
            if (session_ == SessionState::None && !commandPending(Command::Kind::Connect))
            {
                session_ = SessionState::Connecting;
                sessionPeer_ = peer;
                connectStartedMs_ = t;
                queueCommand(Command::Kind::Connect, "CONNECT " + myCall + " " + peer, peer);
                if (log_) log_("Data2G: calling " + peer + " for a session");
                return;
            }
            if (session_ == SessionState::Connecting && t - connectStartedMs_ >= CONNECT_TIMEOUT_MS &&
                !commandPending(Command::Kind::Disconnect))
            {
                // ABORT answers DISCONNECTED, which sends the keying to the group.
                session_ = SessionState::Disconnecting;
                noSessionUntil_[peer] = t + NO_SESSION_HOLD_MS;
                queueCommand(Command::Kind::Disconnect, "ABORT");
                return;
            }
            if (session_ == SessionState::Connected && sessionPeer_ != peer && !sessionOurs_)
            {
                // Somebody else's session: wait for them to close it.
                return;
            }
            return;
        }

        // The GLISS group, in the tempo's mode.
        if (status_.groupPort == 0 || kiss.fd < 0) return;
        if (session_ == SessionState::Connecting || session_ == SessionState::Disconnecting) return;
        if (session_ == SessionState::Connected && sessionOurs_) return; // closing above

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
        keying_.viaSession = false;
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
                for (const auto& frame : streamFrames) deliver(frame);
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

void Data2GTransport::deliver(const std::vector<uint8_t>& bytes)
{
    Frame frame;
    if (!FrameCodec::decode(bytes.data(), (int)bytes.size(), frame)) return;

    FrameCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = frameCallback_;
    }
    // Data2G does not report a signal to noise ratio.
    if (callback) callback(frame, NAN);
}

} // namespace TextMessaging
