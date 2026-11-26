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

#include "LanSocket.h"

#include <cstring>

std::string LanAddress::IpString() const {
    return std::to_string((ip >> 24) & 0xFF) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
           std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

std::string LanAddress::ToString() const {
    return IpString() + ":" + std::to_string(port);
}

LanAddress LanSocket::Broadcast(uint16_t port) {
    return LanAddress{ 0xFFFFFFFFu, port };
}

#if defined(__SWITCH__) || defined(__WIIU__)

LanSocket::~LanSocket() = default;
bool LanSocket::IsSupported() {
    return false;
}
std::string LanSocket::Open(uint16_t, bool) {
    return "not supported on this platform";
}
void LanSocket::Close() {
}
bool LanSocket::IsOpen() const {
    return false;
}
uint16_t LanSocket::LocalPort() const {
    return 0;
}
bool LanSocket::SendTo(const LanAddress&, const void*, size_t) {
    return false;
}
int LanSocket::Receive(void*, size_t, LanAddress&) {
    return -1;
}
bool LanSocket::WaitReadable(int) {
    return false;
}
bool LanSocket::Resolve(const std::string&, uint16_t, LanAddress&) {
    return false;
}

#else

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kBadSocket = INVALID_SOCKET;
void CloseSocket(Socket s) {
    closesocket(s);
}
void SetNonBlocking(Socket s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
int PollOne(Socket s, int timeoutMs) {
    WSAPOLLFD fd{ s, POLLRDNORM, 0 };
    return WSAPoll(&fd, 1, timeoutMs);
}
#else
using Socket = int;
constexpr Socket kBadSocket = -1;
void CloseSocket(Socket s) {
    close(s);
}
void SetNonBlocking(Socket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }
}
int PollOne(Socket s, int timeoutMs) {
    pollfd fd{ s, POLLIN, 0 };
    return poll(&fd, 1, timeoutMs);
}
#endif

Socket ToSocket(intptr_t s) {
    return static_cast<Socket>(s);
}

sockaddr_in ToSockaddr(const LanAddress& a) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(a.port);
    addr.sin_addr.s_addr = htonl(a.ip);
    return addr;
}

} // namespace

LanSocket::~LanSocket() {
    Close();
}

bool LanSocket::IsSupported() {
    return true;
}

bool LanSocket::IsOpen() const {
    return mSocket != static_cast<intptr_t>(kBadSocket) && mSocket != -1;
}

std::string LanSocket::Open(uint16_t port, bool allowBroadcast) {
    Close();
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return "network initialization failed";
    }
#endif
    mNetReady = true;

    const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSocket) {
        Close();
        return "cannot create a UDP socket";
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    if (allowBroadcast) {
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseSocket(s);
        Close();
        return "UDP port " + std::to_string(port) + " is in use";
    }
    SetNonBlocking(s);
#if defined(_WIN32)
    // Without this, a datagram to a peer that just closed makes the next recvfrom fail with WSAECONNRESET.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL reportReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &bytes, nullptr, nullptr);
#endif
    mSocket = static_cast<intptr_t>(s);
    return {};
}

void LanSocket::Close() {
    if (IsOpen()) {
        CloseSocket(ToSocket(mSocket));
    }
    mSocket = -1;
#if defined(_WIN32)
    if (mNetReady) {
        WSACleanup();
    }
#endif
    mNetReady = false;
}

uint16_t LanSocket::LocalPort() const {
    if (!IsOpen()) {
        return 0;
    }
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(ToSocket(mSocket), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool LanSocket::SendTo(const LanAddress& to, const void* data, size_t size) {
    if (!IsOpen()) {
        return false;
    }
    const sockaddr_in addr = ToSockaddr(to);
    const auto sent = sendto(ToSocket(mSocket), static_cast<const char*>(data), static_cast<int>(size), 0,
                             reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<decltype(sent)>(size);
}

int LanSocket::Receive(void* buffer, size_t capacity, LanAddress& from) {
    if (!IsOpen()) {
        return -1;
    }
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    const auto got = recvfrom(ToSocket(mSocket), static_cast<char*>(buffer), static_cast<int>(capacity), 0,
                              reinterpret_cast<sockaddr*>(&addr), &len);
    if (got < 0) {
        // Nothing waiting (or a stale ICMP "port unreachable" on Windows): either way, no datagram.
        return -1;
    }
    from.ip = ntohl(addr.sin_addr.s_addr);
    from.port = ntohs(addr.sin_port);
    return static_cast<int>(got);
}

bool LanSocket::WaitReadable(int timeoutMs) {
    return IsOpen() && PollOne(ToSocket(mSocket), timeoutMs) > 0;
}

bool LanSocket::Resolve(const std::string& host, uint16_t port, LanAddress& out) {
    in_addr parsed{};
    if (inet_pton(AF_INET, host.c_str(), &parsed) == 1) {
        out = LanAddress{ ntohl(parsed.s_addr), port };
        return true;
    }
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    bool ok = false;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result != nullptr) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
        out = LanAddress{ ntohl(addr->sin_addr.s_addr), port };
        ok = true;
        freeaddrinfo(result);
    }
#if defined(_WIN32)
    WSACleanup();
#endif
    return ok;
}

#endif
