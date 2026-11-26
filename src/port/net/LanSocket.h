#pragma once

// Non-blocking UDP for LAN play. Must stay free of libultraship and game headers (see LanSocket.cpp).

#include <cstddef>
#include <cstdint>
#include <string>

struct LanAddress {
    uint32_t ip = 0; // host byte order
    uint16_t port = 0;

    bool IsValid() const {
        return ip != 0 && port != 0;
    }
    bool operator==(const LanAddress& o) const {
        return ip == o.ip && port == o.port;
    }
    bool operator!=(const LanAddress& o) const {
        return !(*this == o);
    }
    std::string ToString() const;   // "192.168.1.20:43100"
    std::string IpString() const;    // "192.168.1.20"
};

class LanSocket {
  public:
    LanSocket() = default;
    ~LanSocket();
    LanSocket(const LanSocket&) = delete;
    LanSocket& operator=(const LanSocket&) = delete;

    static bool IsSupported();

    // Binds to every interface. Port 0 picks a free one. Returns an error message, empty on success.
    // `shared`: other sockets may bind the same port (LAN discovery). Otherwise a port in use is an error, so that a
    // second host on the same PC is told so instead of both silently receiving each other's players.
    std::string Open(uint16_t port, bool allowBroadcast, bool shared = false);
    void Close();
    bool IsOpen() const;
    uint16_t LocalPort() const;

    bool SendTo(const LanAddress& to, const void* data, size_t size);
    // Size of the datagram read, or -1 when nothing is waiting.
    int Receive(void* buffer, size_t capacity, LanAddress& from);
    // Waits until a datagram arrives or the timeout runs out.
    bool WaitReadable(int timeoutMs);

    // "192.168.1.20" or a host name. Returns false when it cannot be resolved.
    static bool Resolve(const std::string& host, uint16_t port, LanAddress& out);
    static LanAddress Broadcast(uint16_t port);

  private:
    intptr_t mSocket = -1;
    bool mNetReady = false;
};
