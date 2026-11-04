#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace sc {

// IPv4 address + port, both in host byte order.
struct Endpoint {
    uint32_t ip = 0;
    uint16_t port = 0;

    bool valid() const { return ip != 0 && port != 0; }
    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Endpoint& o) const { return !(*this == o); }
    std::string str() const;
};

// Non-blocking UDP socket. Everything runs on X-Plane's main thread.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // port 0 = let the OS choose.
    bool open(uint16_t port, std::string& err);
    void close();
    bool isOpen() const;

    bool sendTo(const Endpoint& to, const void* data, size_t len);
    // Returns bytes received, 0 when nothing is waiting.
    int recvFrom(Endpoint& from, void* buf, size_t cap);
    uint16_t localPort() const;

private:
    intptr_t sock_ = -1;
};

bool netInit();
void netShutdown();
bool resolveHost(const std::string& host, uint16_t port, Endpoint& out, std::string& err);

}  // namespace sc
