#pragma once
// Getting through home routers without a VPN:
//  - UPnP: ask the router to open our UDP port (works on most home routers)
//  - STUN: ask a public server which address the internet sees for our socket
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net.h"

namespace sc {

struct NatResult {
    uint32_t lanIp = 0;

    bool upnpDone = false;
    bool upnpOk = false;
    uint32_t upnpExternalIp = 0;
    uint16_t upnpPort = 0;
    std::string upnpNote;

    bool stunResolved = false;
    std::vector<Endpoint> stunServers;
};

class NatHelper {
public:
    ~NatHelper();

    // Runs in a background thread so X-Plane never stutters.
    void start(uint16_t localPort, bool openRouterPort);
    // Closes the router port again (in the background).
    void stop();
    NatResult result() const;
    // Log lines from the background thread (X-Plane's log may only be used on the main thread).
    std::vector<std::string> takeLogs();
    // Blocks until the background work (including closing the router port) is done.
    void wait();

private:
    void run(uint16_t localPort, bool openRouterPort);

    void addLog(const std::string& s);
    std::vector<std::string> logs_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopFlag_ = false;
    NatResult result_;
};

// STUN (RFC 5389) binding request/response, sent over our own game socket.
std::vector<uint8_t> makeStunRequest();
bool isStunPacket(const uint8_t* data, size_t len);
bool parseStunResponse(const uint8_t* data, size_t len, Endpoint& out);

uint32_t findLanIp();
bool isPrivateIp(uint32_t ip);
std::string ipToString(uint32_t ip);

}  // namespace sc
