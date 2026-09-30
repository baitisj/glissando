//=========================================================================
// Name:            Data2GTransport.cpp
// Purpose:         Chat through an external data2g-host over TCP.
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

constexpr int CONNECT_TIMEOUT_MS = 1000;
constexpr int POLL_MS = 100;
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
            int polled = pollSockets(&p, 1, CONNECT_TIMEOUT_MS);
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

// One of the two connections, with its reconnect schedule.
struct Link
{
    const char* name;
    int port = 0;
    int fd = -1;
    uint64_t retryAtMs = 0;
    uint64_t retryDelayMs = FIRST_RETRY_MS;
    bool reportedDown = false;
};

} // namespace

Data2GTransport::Data2GTransport()
    : keying_(Keying::Idle)
    , queuedAtMs_(0)
    , pttOn_(false)
    , kissFd_(-1)
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
        keying_ = Keying::Idle;
        pttOn_ = false;
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
    keying_ = Keying::Idle;
    pttOn_ = false;
}

Data2GTransport::Status Data2GTransport::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Status status = status_;
    status.transmitting = pttOn_;
    return status;
}

bool Data2GTransport::transmit(const std::vector<OutgoingBurst>& bursts)
{
    if (bursts.empty()) return false;

    // A keying is every burst at once, so data2g-host puts them in one
    // Data2G burst. Our AX.25 source is the callsign the frame itself
    // carries, which is what Data2G keys its reports on.
    std::vector<uint8_t> bytes;
    for (const OutgoingBurst& burst : bursts)
    {
        Frame frame;
        if (!FrameCodec::decode(burst.frame.data(), (int)burst.frame.size(), frame)) return false;
        std::vector<uint8_t> kiss =
            Data2G::kissEncode(Data2G::payloadForChatFrame(frame.originCallsign, burst.frame));
        bytes.insert(bytes.end(), kiss.begin(), kiss.end());
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (keying_ != Keying::Idle) return false;
    }

    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (kissFd_ < 0) return false;
        if (!sendAll(kissFd_, bytes.data(), bytes.size()))
        {
            // The thread notices the dead socket on its next read.
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    keying_ = Keying::Queued;
    queuedAtMs_ = now();
    return true;
}

bool Data2GTransport::isTransmitting() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pttOn_) return true;

    switch (keying_)
    {
        case Keying::Idle:
            return false;
        case Keying::OnAir:
            return true;
        case Keying::Queued:
        {
            // Without PTT reports the burst's length is all we can go by.
            uint64_t limit = status_.commandConnected
                                 ? NO_PTT_TIMEOUT_MS
                                 : (uint64_t)Data2G::ESTIMATED_BURST_MILLISECONDS;
            return now() - queuedAtMs_ < limit;
        }
    }
    return false;
}

bool Data2GTransport::isChannelBusy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.commandConnected && status_.channelBusy;
}

void Data2GTransport::handleCommandLine(const std::string& line)
{
    Data2G::CommandEvent event = Data2G::parseCommandLine(line);
    std::lock_guard<std::mutex> lock(mutex_);
    switch (event.type)
    {
        case Data2G::CommandEvent::Type::Ptt:
            pttOn_ = event.on;
            if (event.on && keying_ == Keying::Queued) keying_ = Keying::OnAir;
            else if (!event.on && keying_ == Keying::OnAir) keying_ = Keying::Idle;
            break;
        case Data2G::CommandEvent::Type::Busy:
            status_.channelBusy = event.on;
            break;
        case Data2G::CommandEvent::Type::Mode:
            status_.mode = event.mode;
            break;
        case Data2G::CommandEvent::Type::Other:
            break;
    }
}

void Data2GTransport::handleKissFrame(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> chatBytes;
    if (!Data2G::chatFrameFromPayload(payload, chatBytes)) return; // somebody else's traffic

    Frame frame;
    if (!FrameCodec::decode(chatBytes.data(), (int)chatBytes.size(), frame)) return;

    FrameCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = frameCallback_;
    }
    // Data2G does not report a signal to noise ratio over KISS.
    if (callback) callback(frame, NAN);
}

void Data2GTransport::run(Settings settings)
{
    Link kiss{"KISS", settings.kissPort};
    Link command{"command", settings.commandPort};
    Data2G::KissDecoder kissDecoder;
    Data2G::LineSplitter lines;

    auto closeLink = [&](Link& link, const std::string& why) {
        if (link.fd < 0) return;
        if (&link == &kiss)
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            closeSocket(kissFd_);
            kissFd_ = -1;
        }
        else
        {
            closeSocket(link.fd);
        }
        link.fd = -1;
        link.retryAtMs = steadyMs() + FIRST_RETRY_MS;
        link.retryDelayMs = FIRST_RETRY_MS;
        log(std::string("Data2G ") + link.name + " connection closed: " + why);

        std::lock_guard<std::mutex> lock(mutex_);
        if (&link == &kiss)
        {
            status_.kissConnected = false;
        }
        else
        {
            status_.commandConnected = false;
            status_.channelBusy = false;
            pttOn_ = false;
            // No PTT OFF is coming; fall back to the estimate from now.
            if (keying_ == Keying::OnAir)
            {
                keying_ = Keying::Queued;
                queuedAtMs_ = now();
            }
        }
    };

    auto tryConnect = [&](Link& link) {
        uint64_t t = steadyMs();
        if (link.fd >= 0 || t < link.retryAtMs) return;

        std::string error;
        int fd = connectTo(settings.host, link.port, error);
        if (fd < 0)
        {
            link.retryAtMs = t + link.retryDelayMs;
            link.retryDelayMs = std::min(MAX_RETRY_MS, link.retryDelayMs * 2);
            if (!link.reportedDown)
            {
                log(std::string("Data2G ") + link.name + " port not reachable (" + error +
                    "); is data2g-host running? Retrying.");
                link.reportedDown = true;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            status_.error = error;
            return;
        }

        link.fd = fd;
        link.reportedDown = false;
        log(std::string("Data2G ") + link.name + " port connected at " + settings.host + ":" +
            std::to_string(link.port));
        if (&link == &kiss)
        {
            kissDecoder.reset();
            std::lock_guard<std::mutex> lock(sendMutex_);
            kissFd_ = fd;
        }
        else
        {
            lines.reset();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        (&link == &kiss ? status_.kissConnected : status_.commandConnected) = true;
        if (status_.kissConnected && (!settings.useCommandPort || status_.commandConnected))
        {
            status_.error.clear();
        }
    };

    std::vector<std::vector<uint8_t>> frames;
    std::vector<std::string> commandLines;
    uint8_t buffer[4096];

    while (!stopping_)
    {
        tryConnect(kiss);
        if (settings.useCommandPort) tryConnect(command);

        // A keying data2g-host never put on the air is given up on.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (keying_ == Keying::Queued && status_.commandConnected &&
                now() - queuedAtMs_ >= NO_PTT_TIMEOUT_MS)
            {
                keying_ = Keying::Idle;
            }
            else if (keying_ == Keying::Queued && !status_.commandConnected &&
                     now() - queuedAtMs_ >= (uint64_t)Data2G::ESTIMATED_BURST_MILLISECONDS)
            {
                keying_ = Keying::Idle;
            }
        }

        pollfd fds[2];
        Link* links[2];
        int count = 0;
        for (Link* link : {&kiss, &command})
        {
            if (link->fd < 0) continue;
            fds[count] = pollfd{};
            fds[count].fd = link->fd;
            fds[count].events = POLLIN;
            links[count] = link;
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
            Link& link = *links[i];
            ssize_t got = recv(link.fd, (char*)buffer, sizeof(buffer), 0);
            if (got <= 0)
            {
                int err = got < 0 ? socketError() : 0;
                if (got < 0 && interrupted(err)) continue;
                closeLink(link, got == 0 ? "closed by data2g-host" : socketErrorText(err));
                continue;
            }

            if (&link == &kiss)
            {
                frames.clear();
                kissDecoder.feed(buffer, (int)got, frames);
                for (const auto& frame : frames) handleKissFrame(frame);
            }
            else
            {
                commandLines.clear();
                lines.feed((const char*)buffer, (int)got, commandLines);
                for (const auto& line : commandLines) handleCommandLine(line);
            }
        }
    }

    closeLink(kiss, "stopped");
    closeLink(command, "stopped");
}

} // namespace TextMessaging
