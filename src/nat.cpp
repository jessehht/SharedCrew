#include "nat.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static const socket_t kBad = INVALID_SOCKET;
static void closeSock(socket_t s) { closesocket(s); }
static void setTimeout(socket_t s, int ms) {
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kBad = -1;
static void closeSock(socket_t s) { ::close(s); }
static void setTimeout(socket_t s, int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
#endif

namespace sc {

namespace {

constexpr uint32_t kStunCookie = 0x2112A442;
constexpr int kLeaseSeconds = 7200;
constexpr int kRefreshSeconds = 1800;

std::string lowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------
// Tiny HTTP client for the router (LAN only, plain http)

struct Url {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};

bool parseUrl(const std::string& url, Url& out) {
    std::string u = url;
    if (lowerStr(u.substr(0, 7)) != "http://") return false;
    u = u.substr(7);
    size_t slash = u.find('/');
    std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : u.substr(slash);
    size_t colon = hostPort.find(':');
    if (colon != std::string::npos) {
        out.host = hostPort.substr(0, colon);
        out.port = static_cast<uint16_t>(std::atoi(hostPort.substr(colon + 1).c_str()));
    } else {
        out.host = hostPort;
    }
    return !out.host.empty();
}

std::string dechunk(const std::string& body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find("\r\n", pos);
        if (eol == std::string::npos) break;
        size_t len = std::strtoul(body.substr(pos, eol - pos).c_str(), nullptr, 16);
        if (len == 0) break;
        pos = eol + 2;
        out += body.substr(pos, len);
        pos += len + 2;
    }
    return out;
}

// Returns status code (0 on failure) and fills body.
int httpRequest(const Url& url, const std::string& request, std::string& body) {
    Endpoint ep;
    std::string err;
    if (!resolveHost(url.host, url.port, ep, err)) return 0;
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) return 0;
    setTimeout(s, 3000);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ep.ip);
    addr.sin_port = htons(ep.port);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSock(s);
        return 0;
    }
    size_t sent = 0;
    while (sent < request.size()) {
        int n = send(s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    std::string resp;
    char buf[4096];
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.size() > 512 * 1024) break;
    }
    closeSock(s);

    size_t hdrEnd = resp.find("\r\n\r\n");
    if (hdrEnd == std::string::npos || resp.size() < 12) return 0;
    int status = std::atoi(resp.substr(9, 3).c_str());
    std::string headers = lowerStr(resp.substr(0, hdrEnd));
    body = resp.substr(hdrEnd + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) body = dechunk(body);
    return status;
}

std::string xmlValue(const std::string& xml, const std::string& tag, size_t from = 0, size_t to = std::string::npos) {
    // Matches <tag> or <prefix:tag>
    std::string lx = lowerStr(xml.substr(from, to == std::string::npos ? std::string::npos : to - from));
    std::string lt = lowerStr(tag);
    size_t pos = 0;
    while ((pos = lx.find(lt + ">", pos)) != std::string::npos) {
        size_t lt0 = lx.rfind('<', pos);
        if (lt0 != std::string::npos && lx[lt0 + 1] != '/' &&
            (lt0 + 1 == pos || lx.substr(lt0 + 1, pos - lt0 - 1).find(':') != std::string::npos)) {
            size_t start = pos + lt.size() + 1;
            size_t end = lx.find('<', start);
            if (end == std::string::npos) return {};
            return xml.substr(from + start, end - start);
        }
        pos += lt.size();
    }
    return {};
}

// ---------------------------------------------------------------------------
// UPnP

struct Gateway {
    Url control;
    std::string serviceType;
};

std::vector<std::string> ssdpDiscover() {
    std::vector<std::string> locations;
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBad) return locations;
    setTimeout(s, 500);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    const char* types[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                           "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                           "urn:schemas-upnp-org:service:WANIPConnection:1"};
    for (const char* st : types) {
        std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n") +
                          "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " + st + "\r\n\r\n";
        sendto(s, msg.c_str(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    char buf[2048];
    while (std::chrono::steady_clock::now() < until) {
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string resp(buf);
        std::string low = lowerStr(resp);
        size_t p = low.find("\nlocation:");
        if (p == std::string::npos) continue;
        size_t start = resp.find_first_not_of(' ', p + 10);
        size_t end = resp.find('\r', start);
        std::string loc = resp.substr(start, end - start);
        bool dup = false;
        for (auto& l : locations) dup |= l == loc;
        if (!dup) locations.push_back(loc);
    }
    closeSock(s);
    return locations;
}

bool findGateway(const std::string& location, Gateway& gw) {
    Url url;
    if (!parseUrl(location, url)) return false;
    std::string req = "GET " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port) +
                      "\r\nConnection: close\r\n\r\n";
    std::string xml;
    if (httpRequest(url, req, xml) != 200) return false;

    // Look through <service> blocks for a WAN connection service.
    std::string lx = lowerStr(xml);
    for (const char* want : {"wanipconnection", "wanpppconnection"}) {
        size_t pos = 0;
        while ((pos = lx.find("<service>", pos)) != std::string::npos) {
            size_t end = lx.find("</service>", pos);
            if (end == std::string::npos) break;
            std::string type = xmlValue(xml, "serviceType", pos, end);
            if (lowerStr(type).find(want) != std::string::npos) {
                std::string ctrl = xmlValue(xml, "controlURL", pos, end);
                if (ctrl.empty()) break;
                gw.serviceType = type;
                if (lowerStr(ctrl.substr(0, 7)) == "http://") return parseUrl(ctrl, gw.control);
                gw.control = url;
                gw.control.path = ctrl[0] == '/' ? ctrl : "/" + ctrl;
                return true;
            }
            pos = end;
        }
    }
    return false;
}

int soap(const Gateway& gw, const std::string& action, const std::string& args, std::string& body) {
    std::string env =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<u:" + action + " xmlns:u=\"" + gw.serviceType + "\">" + args + "</u:" + action +
        "></s:Body></s:Envelope>";
    std::string req = "POST " + gw.control.path + " HTTP/1.1\r\n" + "Host: " + gw.control.host + ":" +
                      std::to_string(gw.control.port) + "\r\n" +
                      "Content-Type: text/xml; charset=\"utf-8\"\r\n" + "SOAPAction: \"" + gw.serviceType + "#" +
                      action + "\"\r\n" + "Content-Length: " + std::to_string(env.size()) + "\r\n" +
                      "Connection: close\r\n\r\n" + env;
    return httpRequest(gw.control, req, body);
}

bool addMapping(const Gateway& gw, uint16_t port, uint32_t lanIp, int lease, std::string& error) {
    std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
                       "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port) +
                       "</NewInternalPort><NewInternalClient>" + ipToString(lanIp) +
                       "</NewInternalClient><NewEnabled>1</NewEnabled>"
                       "<NewPortMappingDescription>SharedCrew</NewPortMappingDescription><NewLeaseDuration>" +
                       std::to_string(lease) + "</NewLeaseDuration>";
    std::string body;
    int status = soap(gw, "AddPortMapping", args, body);
    if (status == 200) return true;
    error = xmlValue(body, "errorCode");
    if (error.empty()) error = "HTTP " + std::to_string(status);
    return false;
}

void deleteMapping(const Gateway& gw, uint16_t port) {
    std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
                       "</NewExternalPort><NewProtocol>UDP</NewProtocol>";
    std::string body;
    soap(gw, "DeletePortMapping", args, body);
}

uint32_t externalIp(const Gateway& gw) {
    std::string body;
    if (soap(gw, "GetExternalIPAddress", "", body) != 200) return 0;
    std::string ip = xmlValue(body, "NewExternalIPAddress");
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return 0;
    return ntohl(a.s_addr);
}

std::mt19937& rng() {
    static std::mt19937 r(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public helpers

std::string ipToString(uint32_t ip) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

bool isPrivateIp(uint32_t ip) {
    uint8_t a = ip >> 24, b = (ip >> 16) & 255;
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) ||
           (a == 100 && b >= 64 && b < 128) || (a == 169 && b == 254) || ip == 0;
}

uint32_t findLanIp() {
    // "Connect" a UDP socket to a public address (nothing is sent) and read which local address was picked.
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBad) return 0;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    uint32_t ip = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in me{};
#ifdef _WIN32
        int len = sizeof(me);
#else
        socklen_t len = sizeof(me);
#endif
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) ip = ntohl(me.sin_addr.s_addr);
    }
    closeSock(s);
    return ip;
}

std::vector<uint8_t> makeStunRequest() {
    std::vector<uint8_t> p(20, 0);
    p[1] = 0x01;  // Binding Request
    p[4] = 0x21;
    p[5] = 0x12;
    p[6] = 0xA4;
    p[7] = 0x42;
    for (int i = 8; i < 20; ++i) p[i] = static_cast<uint8_t>(rng()() & 255);
    return p;
}

bool isStunPacket(const uint8_t* d, size_t len) {
    return len >= 20 && (d[0] & 0xC0) == 0 && d[4] == 0x21 && d[5] == 0x12 && d[6] == 0xA4 && d[7] == 0x42;
}

bool parseStunResponse(const uint8_t* d, size_t len, Endpoint& out) {
    if (!isStunPacket(d, len) || d[0] != 0x01 || d[1] != 0x01) return false;  // Binding Success
    size_t msgLen = (static_cast<size_t>(d[2]) << 8) | d[3];
    size_t end = std::min(len, 20 + msgLen);
    size_t pos = 20;
    bool found = false;
    while (pos + 4 <= end) {
        uint16_t type = static_cast<uint16_t>((d[pos] << 8) | d[pos + 1]);
        uint16_t alen = static_cast<uint16_t>((d[pos + 2] << 8) | d[pos + 3]);
        const uint8_t* v = d + pos + 4;
        if (pos + 4 + alen > end) break;
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {
            uint16_t port = static_cast<uint16_t>((v[2] << 8) | v[3]);
            uint32_t ip = (uint32_t(v[4]) << 24) | (uint32_t(v[5]) << 16) | (uint32_t(v[6]) << 8) | v[7];
            if (type == 0x0020) {
                port ^= static_cast<uint16_t>(kStunCookie >> 16);
                ip ^= kStunCookie;
            }
            out.ip = ip;
            out.port = port;
            found = true;
            if (type == 0x0020) break;  // XOR-MAPPED-ADDRESS is the preferred one
        }
        pos += 4 + ((alen + 3) & ~size_t(3));
    }
    return found;
}

// ---------------------------------------------------------------------------
// NatHelper

NatHelper::~NatHelper() {
    stop();
    wait();
}

void NatHelper::wait() {
    if (thread_.joinable()) thread_.join();
}

void NatHelper::start(uint16_t localPort, bool openRouterPort, const std::string& serverAddress) {
    stop();
    wait();
    serverAddress_ = serverAddress;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stopFlag_ = false;
        result_ = NatResult{};
    }
    thread_ = std::thread(&NatHelper::run, this, localPort, openRouterPort);
}

void NatHelper::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stopFlag_ = true;
    cv_.notify_all();
}

void NatHelper::addLog(const std::string& s) {
    std::lock_guard<std::mutex> lk(mutex_);
    logs_.push_back(s);
}

std::vector<std::string> NatHelper::takeLogs() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> out;
    out.swap(logs_);
    return out;
}

NatResult NatHelper::result() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return result_;
}

void NatHelper::run(uint16_t localPort, bool openRouterPort) {
    auto stopped = [this] {
        std::lock_guard<std::mutex> lk(mutex_);
        return stopFlag_;
    };

    uint32_t lan = findLanIp();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        result_.lanIp = lan;
    }

    // SharedCrew server (matchmaking + relay), "host:port".
    if (!serverAddress_.empty()) {
        std::string host = serverAddress_;
        uint16_t port = 51010;
        size_t colon = host.rfind(':');
        if (colon != std::string::npos) {
            port = static_cast<uint16_t>(std::atoi(host.substr(colon + 1).c_str()));
            host = host.substr(0, colon);
        }
        Endpoint ep;
        std::string err;
        bool ok = resolveHost(host, port, ep, err);
        if (!ok) addLog("can't find the SharedCrew server " + serverAddress_);
        std::lock_guard<std::mutex> lk(mutex_);
        if (ok) result_.server = ep;
        result_.serverResolved = true;
    }

    // STUN servers (public, free). Resolved here so DNS never blocks X-Plane.
    std::vector<Endpoint> servers;
    const std::pair<const char*, uint16_t> list[] = {
        {"stun.l.google.com", 19302}, {"stun.cloudflare.com", 3478}, {"stun1.l.google.com", 19302}};
    for (auto& [host, port] : list) {
        Endpoint ep;
        std::string err;
        if (resolveHost(host, port, ep, err)) servers.push_back(ep);
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        result_.stunServers = servers;
        result_.stunResolved = true;
    }
    if (stopped()) return;

    if (!openRouterPort) return;

    // UPnP: find the router and open our port.
    Gateway gw;
    bool haveGw = false;
    for (const auto& loc : ssdpDiscover()) {
        if (findGateway(loc, gw)) {
            haveGw = true;
            break;
        }
    }
    std::string note;
    bool ok = false;
    uint32_t ext = 0;
    if (!haveGw) {
        note = "router does not offer UPnP";
    } else {
        std::string err;
        ok = addMapping(gw, localPort, lan, kLeaseSeconds, err);
        if (!ok && err == "725") ok = addMapping(gw, localPort, lan, 0, err);  // router only allows permanent
        if (ok) {
            ext = externalIp(gw);
            note = "router port " + std::to_string(localPort) + " opened";
            if (ext && isPrivateIp(ext)) note += " (but the router is behind another router)";
        } else {
            note = "router refused to open the port (" + err + ")";
        }
    }
    addLog("UPnP: " + note);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        result_.upnpDone = true;
        result_.upnpOk = ok && ext != 0 && !isPrivateIp(ext);
        result_.upnpExternalIp = ext;
        result_.upnpPort = localPort;
        result_.upnpNote = note;
    }
    if (!ok) return;

    // Keep the mapping alive until the session stops, then remove it.
    for (;;) {
        std::unique_lock<std::mutex> lk(mutex_);
        if (cv_.wait_for(lk, std::chrono::seconds(kRefreshSeconds), [this] { return stopFlag_; })) break;
        lk.unlock();
        std::string err;
        addMapping(gw, localPort, lan, kLeaseSeconds, err);
    }
    deleteMapping(gw, localPort);
    addLog("UPnP: router port " + std::to_string(localPort) + " closed again");
}

}  // namespace sc
