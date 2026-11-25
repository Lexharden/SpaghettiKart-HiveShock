// This file must not include libultraship or game headers: winsock2.h has to come before any windows.h.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "Ws2_32.lib")
#endif
#endif

#include "HiveShockServer.h"

#include <chrono>
#include <utility>
#include <vector>

#if defined(__SWITCH__) || defined(__WIIU__)

HiveShockServer::HiveShockServer(LineHandler onLine) : mOnLine(std::move(onLine)) {
}
HiveShockServer::~HiveShockServer() = default;
bool HiveShockServer::IsSupported() {
    return false;
}
std::string HiveShockServer::Start(uint16_t, bool, uint16_t) {
    return "not supported on this platform";
}
void HiveShockServer::Stop() {
}
void HiveShockServer::SendEvent(std::string) {
}
void HiveShockServer::Run() {
}
void HiveShockServer::FlushEvents() {
}

#else

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxLineBytes = 8192;
constexpr size_t kMaxClients = 8;
constexpr size_t kMaxEvents = 64;
constexpr int kPollMs = 100;
constexpr int kEventTimeoutMs = 200;
constexpr auto kIdleTimeout = std::chrono::seconds(10);

#if defined(_WIN32)
using Socket = SOCKET;
using PollFd = WSAPOLLFD;
const Socket kBadSocket = INVALID_SOCKET;

void CloseSocket(Socket s) {
    closesocket(s);
}
int PollSockets(PollFd* fds, size_t count, int timeoutMs) {
    return WSAPoll(fds, static_cast<ULONG>(count), timeoutMs);
}
bool WouldBlock() {
    const int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS;
}
void SetNonBlocking(Socket s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
#else
using Socket = int;
using PollFd = pollfd;
constexpr Socket kBadSocket = -1;

void CloseSocket(Socket s) {
    close(s);
}
int PollSockets(PollFd* fds, size_t count, int timeoutMs) {
    return poll(fds, static_cast<nfds_t>(count), timeoutMs);
}
bool WouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EINPROGRESS;
}
void SetNonBlocking(Socket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }
}
#endif

constexpr short kReadable = POLLIN | POLLHUP | POLLERR;

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// A write to a closed peer must never raise SIGPIPE and kill the game (macOS has no MSG_NOSIGNAL).
void Configure(Socket s) {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    SetNonBlocking(s);
}

sockaddr_in MakeAddress(bool any, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(any ? INADDR_ANY : INADDR_LOOPBACK);
    return addr;
}

struct Client {
    Socket socket = kBadSocket;
    std::string buffer;
    Clock::time_point lastActivity{};
};

} // namespace

HiveShockServer::HiveShockServer(LineHandler onLine) : mOnLine(std::move(onLine)) {
}

HiveShockServer::~HiveShockServer() {
    Stop();
}

bool HiveShockServer::IsSupported() {
    return true;
}

std::string HiveShockServer::Start(uint16_t port, bool lan, uint16_t eventPort) {
    Stop();
    mEventPort = eventPort;

#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return "network initialization failed";
    }
#endif
    mNetReady = true;

    const Socket sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == kBadSocket) {
        Stop();
        return "could not create the socket";
    }

    int on = 1;
#if defined(_WIN32)
    // On Windows SO_REUSEADDR would let another process hijack the port.
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif

    const sockaddr_in addr = MakeAddress(lan, port);
    if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || listen(sock, 16) != 0) {
        CloseSocket(sock);
        Stop();
        return "cannot listen on port " + std::to_string(port) + " (is another HiveShock game open?)";
    }

    Configure(sock);
    mListen = static_cast<intptr_t>(sock);
    mRun = true;
    mThread = std::thread([this] { Run(); });
    return "";
}

void HiveShockServer::Stop() {
    mRun = false;
    if (mThread.joinable()) {
        mThread.join();
    }
    if (mListen != -1) {
        CloseSocket(static_cast<Socket>(mListen));
        mListen = -1;
    }
    if (mNetReady) {
#if defined(_WIN32)
        WSACleanup();
#endif
        mNetReady = false;
    }
    mClients = 0;
    std::lock_guard<std::mutex> lock(mEventMutex);
    mEvents.clear();
}

void HiveShockServer::SendEvent(std::string line) {
    if (!mRun) {
        return;
    }
    std::lock_guard<std::mutex> lock(mEventMutex);
    if (mEvents.size() < kMaxEvents) {
        mEvents.push_back(std::move(line));
    }
}

void HiveShockServer::FlushEvents() {
    std::string payload;
    {
        std::lock_guard<std::mutex> lock(mEventMutex);
        for (const auto& line : mEvents) {
            payload += line;
            payload += '\n';
        }
        mEvents.clear();
    }
    if (payload.empty()) {
        return;
    }

    const Socket sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == kBadSocket) {
        return;
    }
    Configure(sock);

    const sockaddr_in addr = MakeAddress(false, mEventPort);
    bool connected = connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
    if (!connected && WouldBlock()) {
        PollFd fd{};
        fd.fd = sock;
        fd.events = POLLOUT;
        int error = 0;
        socklen_t len = sizeof(error);
        connected = PollSockets(&fd, 1, kEventTimeoutMs) > 0 &&
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) == 0 && error == 0;
    }

    size_t sent = 0;
    while (connected && sent < payload.size()) {
        const auto n = send(sock, payload.data() + sent, static_cast<int>(payload.size() - sent), kSendFlags);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        PollFd fd{};
        fd.fd = sock;
        fd.events = POLLOUT;
        if (n == 0 || !WouldBlock() || PollSockets(&fd, 1, kEventTimeoutMs) <= 0) {
            break;
        }
    }
    CloseSocket(sock);
}

void HiveShockServer::Run() {
    std::vector<Client> clients;
    std::vector<PollFd> fds;

    const auto emit = [this](std::string line) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line.size() <= kMaxLineBytes) {
            mOnLine(line);
        }
    };

    while (mRun) {
        FlushEvents();

        fds.clear();
        PollFd listenFd{};
        listenFd.fd = static_cast<Socket>(mListen);
        listenFd.events = POLLIN;
        fds.push_back(listenFd);
        for (const auto& client : clients) {
            PollFd fd{};
            fd.fd = client.socket;
            fd.events = POLLIN;
            fds.push_back(fd);
        }

        if (PollSockets(fds.data(), fds.size(), kPollMs) < 0 && !WouldBlock()) {
            // Never spin if poll itself keeps failing.
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            continue;
        }

        const auto now = Clock::now();
        for (size_t i = clients.size(); i-- > 0;) {
            Client& client = clients[i];
            bool closed = false;

            if (fds[i + 1].revents & kReadable) {
                char chunk[1024];
                const auto n = recv(client.socket, chunk, sizeof(chunk), 0);
                if (n > 0) {
                    client.lastActivity = now;
                    client.buffer.append(chunk, static_cast<size_t>(n));
                    size_t newline;
                    while ((newline = client.buffer.find('\n')) != std::string::npos) {
                        emit(client.buffer.substr(0, newline));
                        client.buffer.erase(0, newline + 1);
                    }
                    closed = client.buffer.size() > kMaxLineBytes; // a peer that never sends a newline
                    if (closed) {
                        client.buffer.clear();
                    }
                } else {
                    closed = n == 0 || !WouldBlock();
                }
            }

            if (closed || now - client.lastActivity > kIdleTimeout) {
                // Some clients close right after one JSON object without a newline: don't lose it.
                emit(std::move(client.buffer));
                CloseSocket(client.socket);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }

        if (fds[0].revents & kReadable) {
            const Socket sock = accept(static_cast<Socket>(mListen), nullptr, nullptr);
            if (sock != kBadSocket) {
                if (clients.size() >= kMaxClients) {
                    CloseSocket(sock);
                } else {
                    Configure(sock);
                    clients.push_back(Client{ sock, "", now });
                }
            }
        }
        mClients = static_cast<uint32_t>(clients.size());
    }

    for (const auto& client : clients) {
        CloseSocket(client.socket);
    }
    mClients = 0;
}

#endif
