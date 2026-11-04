#include "net.h"

#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
using socket_t = SOCKET;
static const socket_t kInvalid = INVALID_SOCKET;
static int lastError() { return WSAGetLastError(); }
static void closeSock(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kInvalid = -1;
static int lastError() { return errno; }
static void closeSock(socket_t s) { ::close(s); }
#endif

namespace sc {

std::string Endpoint::str() const {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
             port);
    return buf;
}

bool netInit() {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void netShutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::isOpen() const { return sock_ != -1; }

bool UdpSocket::open(uint16_t port, std::string& err) {
    close();
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        err = "Could not create network socket (" + std::to_string(lastError()) + ")";
        return false;
    }

#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    // Stop Windows from reporting "connection reset" on UDP when the other side is not there yet.
    BOOL newBehavior = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior), nullptr, 0, &bytes, nullptr, nullptr);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = "Port " + std::to_string(port) + " is already in use or blocked (" + std::to_string(lastError()) + ")";
        closeSock(s);
        return false;
    }
    sock_ = static_cast<intptr_t>(s);
    return true;
}

void UdpSocket::close() {
    if (sock_ != -1) {
        closeSock(static_cast<socket_t>(sock_));
        sock_ = -1;
    }
}

bool UdpSocket::sendTo(const Endpoint& to, const void* data, size_t len) {
    if (!isOpen()) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(to.ip);
    addr.sin_port = htons(to.port);
    int sent = sendto(static_cast<socket_t>(sock_), static_cast<const char*>(data), static_cast<int>(len), 0,
                      reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<int>(len);
}

int UdpSocket::recvFrom(Endpoint& from, void* buf, size_t cap) {
    if (!isOpen()) return 0;
    for (;;) {
        sockaddr_in addr{};
#ifdef _WIN32
        int addrLen = sizeof(addr);
#else
        socklen_t addrLen = sizeof(addr);
#endif
        int n = recvfrom(static_cast<socket_t>(sock_), static_cast<char*>(buf), static_cast<int>(cap), 0,
                         reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (n > 0) {
            from.ip = ntohl(addr.sin_addr.s_addr);
            from.port = ntohs(addr.sin_port);
            return n;
        }
        if (n == 0) return 0;
#ifdef _WIN32
        int e = lastError();
        if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  // skip and read the next packet
#endif
        return 0;  // would block, or a real error: nothing to read now
    }
}

bool resolveHost(const std::string& host, uint16_t port, Endpoint& out, std::string& err) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        err = "Can't find address \"" + host + "\"";
        return false;
    }
    auto* a = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out.ip = ntohl(a->sin_addr.s_addr);
    out.port = port;
    freeaddrinfo(res);
    return true;
}

}  // namespace sc
