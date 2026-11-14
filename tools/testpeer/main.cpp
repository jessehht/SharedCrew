// SharedCrew test peer: pretends to be the second pilot, so you can test on one PC.
// Run it, then in X-Plane join 127.0.0.1 port 51000.
#include <conio.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../src/code.h"
#include "../../src/nat.h"
#include "../../src/profile_files.h"
#include "../../src/net.h"
#include "../../src/protocol.h"

using namespace sc;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

double now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// Same rules as the plugin (shared code in profile_files.cpp).
struct Names {
    std::vector<std::string> drefs;  // index -> "kind name", like the plugin's order
    std::vector<std::string> commands;
    bool fixedRoles = false;
};

Names loadNames(const fs::path& dir, const std::string& acf, const std::string& acfPath) {
    Names n;
    ProfileSpec spec;
    std::string err;
    if (!readProfileSpec(dir.u8string(), acf, acfPath, spec, err,
                         [](const std::string& s) { printf("  %s\n", s.c_str()); })) {
        printf("  %s\n", err.c_str());
        return n;
    }
    printf("  profile: %s%s\n", spec.title.c_str(), spec.fixedRoles ? " (both pilots fly, X-Plane computes)" : "");
    for (auto& [k, a] : spec.entries) {
        if (k == "command")
            n.commands.push_back(a);
        else
            n.drefs.push_back(k + " " + a);
    }
    n.fixedRoles = spec.fixedRoles;
    return n;
}

std::string shortName(const std::string& entry) {
    size_t sp = entry.find(' ');
    return sp == std::string::npos ? entry : entry.substr(sp + 1);
}
int indexOf(const std::vector<std::string>& v, const std::string& name) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == name) return static_cast<int>(i);
    return -1;
}

struct DrefState {
    uint32_t version = 0;
    uint8_t origin = 0;
    std::vector<double> vals;
};

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    uint16_t port = kDefaultPort;
    fs::path profiles = fs::path(argv[0]).parent_path() / ".." / "SharedCrew" / "profiles";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--port") port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
        if (std::string(argv[i]) == "--profiles") profiles = argv[i + 1];
    }
    std::string serverAddr;
    bool relayOnly = false;
    fs::path xplaneDir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\X-Plane 12";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--server") serverAddr = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[i + 1] : kDefaultServer;
        if (a == "--relay-only") relayOnly = true;
        if (a == "--xplane" && i + 1 < argc) xplaneDir = fs::u8path(argv[i + 1]);
    }
    if (relayOnly && serverAddr.empty()) serverAddr = kDefaultServer;

    printf("SharedCrew test peer\n");
    printf("--------------------\n");
    printf("In X-Plane: Plugins > SharedCrew, Host address 127.0.0.1, port %u, click Join host.\n\n", port);

    netInit();
    UdpSocket sock;
    std::string err;
    if (!sock.open(port, err)) {
        printf("Error: %s\n", err.c_str());
        printf("Press a key to close.\n");
        _getch();
        return 1;
    }

    // --profile-check <path inside Aircraft>: show which profile an aircraft gets, then quit.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--profile-check") continue;
        fs::path acfPath = xplaneDir / "Aircraft" / fs::u8path(argv[i + 1]);
        Names nm = loadNames(profiles, acfPath.filename().u8string(), acfPath.u8string());
        std::map<std::string, int> kinds;
        for (auto& e : nm.drefs) kinds[e.substr(0, e.find(' '))]++;
        for (auto& [k, v] : kinds) printf("  %-8s %d\n", k.c_str(), v);
        printf("  %-8s %zu\n", "command", nm.commands.size());
        sock.close();
        return 0;
    }

    // --nat-check: test the router/internet part on its own, then quit.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--nat-check") continue;
        printf("Router check on port %u\n", port);
        NatHelper nat;
        nat.start(port, true);
        Endpoint pub;
        bool stunOk = false;
        int tries = 0;
        double lastSend = -1;
        const double start = now();
        while (now() - start < 12.0) {
            NatResult r = nat.result();
            if (!stunOk && r.stunResolved && !r.stunServers.empty() && now() - lastSend > 0.5 && tries < 12) {
                auto req = makeStunRequest();
                sock.sendTo(r.stunServers[tries % r.stunServers.size()], req.data(), req.size());
                lastSend = now();
                ++tries;
            }
            uint8_t buf[1500];
            Endpoint from;
            int n;
            while ((n = sock.recvFrom(from, buf, sizeof(buf))) > 0)
                if (!stunOk && parseStunResponse(buf, n, pub)) stunOk = true;
            for (auto& l : nat.takeLogs()) printf("  %s\n", l.c_str());
            if (stunOk && r.upnpDone) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        NatResult r = nat.result();
        printf("  home network address : %s\n", ipToString(r.lanIp).c_str());
        printf("  internet address     : %s\n", stunOk ? pub.str().c_str() : "not found (STUN failed)");
        printf("  UPnP                 : %s\n", r.upnpDone ? r.upnpNote.c_str() : "no answer");
        if (r.upnpOk) printf("  router says internet : %s\n", ipToString(r.upnpExternalIp).c_str());
        Endpoint codePub = r.upnpOk ? Endpoint{r.upnpExternalIp, port} : pub;
        std::string code = encodeCode(codePub, Endpoint{r.lanIp, port});
        Endpoint a, b;
        bool round = decodeCode(code, a, b) && a == codePub && b == Endpoint{r.lanIp, port};
        printf("  session code         : %s (%s)\n", code.c_str(), round ? "reads back OK" : "READ-BACK FAILED");
        nat.stop();
        nat.wait();
        for (auto& l : nat.takeLogs()) printf("  %s\n", l.c_str());
        sock.close();
        netShutdown();
        return 0;
    }

    // --server: register with the SharedCrew server, so X-Plane can join with a short code.
    Endpoint server;
    uint64_t token = (static_cast<uint64_t>(GetTickCount64()) << 20) ^ GetCurrentProcessId();
    double lastRegister = -100;
    std::string shortCode;
    if (!serverAddr.empty()) {
        std::string h = serverAddr;
        uint16_t sp = 51010;
        size_t colon = h.rfind(':');
        if (colon != std::string::npos) {
            sp = static_cast<uint16_t>(std::atoi(h.substr(colon + 1).c_str()));
            h = h.substr(0, colon);
        }
        if (!resolveHost(h, sp, server, err)) {
            printf("Can't find server %s\n", serverAddr.c_str());
            return 1;
        }
        printf("Using SharedCrew server %s%s\n", server.str().c_str(),
               relayOnly ? " (relay only: direct connections are ignored)" : "");
    }
    auto sendServer = [&](Srv type, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> p(kServerMagic, kServerMagic + 4);
        p.push_back(kServerVersion);
        p.push_back(static_cast<uint8_t>(type));
        p.insert(p.end(), body.begin(), body.end());
        sock.sendTo(server, p.data(), p.size());
    };

    Endpoint peer;    bool connected = false;
    bool iFly = false;
    Names names;
    std::map<int, DrefState> drefs;
    uint32_t outId = 1, inId = 1;
    std::map<uint32_t, std::vector<uint8_t>> outQueue;
    double lastRecv = 0, lastPing = 0, lastState = 0, lastResend = 0, lastPrint = 0;

    // Last state from X-Plane (when X-Plane flies), and our own flight (when we fly)
    double lat = 0, lon = 0, elev = 0;
    float psi = 0, theta = 0, phi = 0;
    bool havePos = false;
    std::vector<std::vector<float>> stream;
    double flyStart = 0;

    auto rawSend = [&](const std::vector<uint8_t>& data) {
        if (server.valid() && peer == server)
            sendServer(Srv::Relay, data);
        else
            sock.sendTo(peer, data.data(), data.size());
    };
    auto send = [&](const Writer& w) { rawSend(w.buf); };
    auto sendReliable = [&](RelKind kind, const std::vector<uint8_t>& payload) {
        Writer w;
        w.header(Msg::Reliable);
        w.put<uint32_t>(outId);
        w.put<uint8_t>(static_cast<uint8_t>(kind));
        w.buf.insert(w.buf.end(), payload.begin(), payload.end());
        send(w);
        outQueue[outId++] = w.buf;
    };
    auto setDref = [&](const std::string& name, double value) {
        int idx = indexOf(names.drefs, "sync " + name);
        if (idx < 0 || !drefs.count(idx)) {
            printf("  %s: not seen from X-Plane yet\n", name.c_str());
            return;
        }
        auto& d = drefs[idx];
        d.version++;
        d.origin = 0;
        for (auto& v : d.vals) v = value;
        Writer w;
        w.header(Msg::Drefs);
        w.put<uint16_t>(1);
        w.put<uint16_t>(static_cast<uint16_t>(idx));
        w.put<uint32_t>(d.version);
        w.put<uint8_t>(d.origin);
        w.put<uint8_t>(static_cast<uint8_t>(d.vals.size()));
        for (double v : d.vals) w.put<double>(v);
        send(w);
        printf("  -> %s = %g\n", name.c_str(), value);
    };
    auto pressCommand = [&](const std::string& name) {
        int idx = indexOf(names.commands, name);
        if (idx < 0) {
            printf("  command %s not in profile\n", name.c_str());
            return;
        }
        for (uint8_t phase : {uint8_t(0), uint8_t(2)}) {
            std::vector<uint8_t> p(3);
            uint16_t i = static_cast<uint16_t>(idx);
            memcpy(p.data(), &i, 2);
            p[2] = phase;
            sendReliable(RelKind::Command, p);
        }
        printf("  -> pressed %s\n", name.c_str());
    };
    auto printHelp = [] {
        printf("\nKeys:\n");
        printf("  T  take the controls (test peer flies circles from your position)\n");
        printf("  G  give the controls back to X-Plane\n");
        printf("  L  landing lights on/off      B  beacon on/off\n");
        printf("  A  press autopilot HDG button\n");
        printf("  Q  quit\n\n");
    };

    printf("Waiting for X-Plane...\n");
    bool running = true;
    while (running) {
        double t = now();

        // --- receive
        uint8_t buf[4096];
        Endpoint from;
        int n;
        while ((n = sock.recvFrom(from, buf, sizeof(buf))) > 0) {
            if (n >= 6 && memcmp(buf, kServerMagic, 4) == 0) {
                if (!server.valid() || from != server) continue;
                Srv st = static_cast<Srv>(buf[5]);
                if (st == Srv::Registered && n >= 12) {
                    std::string code(reinterpret_cast<char*>(buf + 6), 6);
                    if (code != shortCode) {
                        shortCode = code;
                        printf("\nSession code: %s-%s   (in X-Plane: paste it in 'Code or IP' and click Join)\n\n",
                               code.substr(0, 3).c_str(), code.substr(3).c_str());
                    }
                    continue;
                }
                if (st == Srv::Peer) {
                    printf("  server: X-Plane is looking for us\n");
                    continue;
                }
                if (st != Srv::Relay) continue;
                memmove(buf, buf + 6, n - 6);  // unwrap the relayed game packet
                n -= 6;
                from = server;
            }            Reader r(buf, n);
            if (r.get<uint32_t>() != kMagic) continue;
            uint16_t ver = r.get<uint16_t>();
            Msg type = static_cast<Msg>(r.get<uint8_t>());
            r.get<uint8_t>();
            if (ver != kProtocolVersion) continue;

            if (type == Msg::Hello) {
                if (relayOnly && from != server) continue;
                std::string acf = r.str();
                r.get<uint32_t>();  // profile hash
                r.str();            // version
                if (r.remaining() >= 8) r.get<uint64_t>();
                std::string rel = r.remaining() >= 2 ? r.str() : std::string();
                std::string acfPath = rel.empty() ? std::string() : (xplaneDir / "Aircraft" / fs::u8path(rel)).u8string();
                if (!connected) {
                    peer = from;
                    connected = true;
                    iFly = false;
                    drefs.clear();
                    outQueue.clear();
                    outId = inId = 1;
                    printf("X-Plane connected from %s, aircraft %s\n", from.str().c_str(), acf.c_str());
                    names = loadNames(profiles, acf, acfPath);
                    printf("  %zu datarefs, %zu commands in profile\n", names.drefs.size(), names.commands.size());
                    printHelp();
                }
                Writer w;
                w.header(Msg::Welcome);
                w.put<uint8_t>(0);         // X-Plane flies first
                w.put<float>(-1.0f);       // don't change X-Plane's time
                w.put<int32_t>(-1);
                send(w);
                lastRecv = t;
                continue;
            }
            if (!connected || from != peer) continue;
            lastRecv = t;

            switch (type) {
                case Msg::Ping: {
                    double ts = r.get<double>();
                    Writer w;
                    w.header(Msg::Pong);
                    w.put<double>(ts);
                    send(w);
                    break;
                }
                case Msg::State: {
                    if (iFly) break;
                    r.get<double>();
                    lat = r.get<double>();
                    lon = r.get<double>();
                    elev = r.get<double>();
                    psi = r.get<float>();
                    theta = r.get<float>();
                    phi = r.get<float>();
                    r.get<float>();
                    r.get<float>();
                    r.get<float>();
                    uint16_t ns = r.get<uint16_t>();
                    stream.assign(ns, {});
                    for (auto& v : stream) {
                        uint8_t c = r.get<uint8_t>();
                        v.resize(c);
                        for (auto& f : v) f = r.get<float>();
                    }
                    havePos = r.good();
                    break;
                }
                case Msg::Drefs: {
                    uint16_t cnt = r.get<uint16_t>();
                    for (uint16_t k = 0; k < cnt && r.good(); ++k) {
                        uint16_t idx = r.get<uint16_t>();
                        uint32_t v = r.get<uint32_t>();
                        uint8_t o = r.get<uint8_t>();
                        uint8_t c = r.get<uint8_t>();
                        std::vector<double> vals(c);
                        for (auto& x : vals) x = r.get<double>();
                        auto& d = drefs[idx];
                        bool newer = v > d.version || (v == d.version && o > d.origin) || d.vals.empty();
                        if (!newer) continue;
                        bool changed = !d.vals.empty() && d.vals != vals;
                        d.version = v;
                        d.origin = o;
                        d.vals = vals;
                        if (changed) {
                            std::string nm = idx < names.drefs.size() ? shortName(names.drefs[idx]) : "#" + std::to_string(idx);
                            printf("  <- X-Plane changed %s =", nm.c_str());
                            for (size_t i = 0; i < vals.size() && i < 4; ++i) printf(" %g", vals[i]);
                            printf(vals.size() > 4 ? " ...\n" : "\n");
                        }
                    }
                    break;
                }
                case Msg::Reliable: {
                    uint32_t id = r.get<uint32_t>();
                    Writer ack;
                    ack.header(Msg::Ack);
                    ack.put<uint32_t>(id);
                    send(ack);
                    if (id != inId) break;  // X-Plane resends; good enough for a test tool
                    ++inId;
                    RelKind kind = static_cast<RelKind>(r.get<uint8_t>());
                    if (kind == RelKind::Command) {
                        uint16_t idx = r.get<uint16_t>();
                        uint8_t ph = r.get<uint8_t>();
                        if (ph == 0)
                            printf("  <- X-Plane pressed %s\n",
                                   idx < names.commands.size() ? names.commands[idx].c_str() : "?");
                    } else if (kind == RelKind::TakeControl) {
                        printf("  <- X-Plane asks for the controls: OK\n");
                        iFly = false;
                        sendReliable(RelKind::ControlState, {0});
                    }
                    break;
                }
                case Msg::Ack: {
                    uint32_t id = r.get<uint32_t>();
                    outQueue.erase(id);
                    break;
                }
                case Msg::Bye:
                    printf("X-Plane disconnected.\n\nWaiting for X-Plane...\n");
                    connected = false;
                    break;
                default:
                    break;
            }
        }

        if (server.valid() && !connected && t - lastRegister > (shortCode.empty() ? 1.0 : 15.0)) {
            lastRegister = t;
            Writer w;
            w.put<uint64_t>(token);
            w.put<uint32_t>(0);
            w.put<uint16_t>(port);
            sendServer(Srv::Register, w.buf);
        }

        if (connected) {
            if (t - lastRecv > 8.0) {
                printf("Lost X-Plane.\n\nWaiting for X-Plane...\n");
                connected = false;
            }
            if (t - lastPing > 1.0) {
                lastPing = t;
                Writer w;
                w.header(Msg::Ping);
                w.put<double>(t);
                send(w);
            }
            if (t - lastResend > 0.25) {
                lastResend = t;
                for (auto& [id, data] : outQueue) rawSend(data);
            }

            // Fly: 70 m/s (about 135 kt), gentle left/right turns, constant altitude.
            if (iFly && t - lastState >= 1.0 / 30.0) {
                double dt = lastState > 0 ? t - lastState : 0.0;
                lastState = t;
                double phase = std::fmod(t - flyStart, 120.0);
                double turn = phase < 20 ? 0.0 : (phase < 80 ? 3.0 : (phase < 100 ? 0.0 : -3.0));  // deg/s
                psi = static_cast<float>(std::fmod(psi + turn * dt + 360.0, 360.0));
                phi = static_cast<float>(turn * 6.0);  // ~18 deg bank in a standard turn
                theta = 2.0f;
                const double v = 70.0;
                lat += v * std::cos(psi * kPi / 180.0) * dt / 111320.0;
                lon += v * std::sin(psi * kPi / 180.0) * dt / (111320.0 * std::cos(lat * kPi / 180.0));

                Writer w;
                w.header(Msg::State);
                w.put<double>(t);
                w.put<double>(lat);
                w.put<double>(lon);
                w.put<double>(elev);
                w.put<float>(psi);
                w.put<float>(theta);
                w.put<float>(phi);
                w.put<float>(0.0f);
                w.put<float>(0.0f);
                w.put<float>(static_cast<float>(turn));
                w.put<uint16_t>(static_cast<uint16_t>(stream.size()));
                for (auto& s : stream) {
                    w.put<uint8_t>(static_cast<uint8_t>(s.size()));
                    for (float f : s) w.put<float>(f);
                }
                send(w);

                if (t - lastPrint > 5.0) {
                    lastPrint = t;
                    printf("  flying: lat %.5f lon %.5f alt %.0f ft hdg %.0f\n", lat, lon, elev * 3.28084, psi);
                }
            }
        }

        // --- keys
        while (_kbhit()) {
            int c = toupper(_getch());
            if (c == 'Q') running = false;
            if (!connected) continue;
            if (c == 'T') {
                if (names.fixedRoles) {
                    printf("  this aircraft has fixed roles: X-Plane always computes it, so T is not used\n");
                } else if (!havePos) {
                    printf("  no position from X-Plane yet\n");
                } else if (!iFly) {
                    iFly = true;
                    flyStart = t;
                    lastState = 0;
                    sendReliable(RelKind::ControlState, {1});
                    printf("  -> test peer has the controls. Your aircraft should now follow.\n");
                    if (elev < 300) printf("  (tip: start in the air, the test peer flies at constant altitude)\n");
                }
            }
            if (c == 'G' && iFly) {
                iFly = false;
                sendReliable(RelKind::ControlState, {0});
                printf("  -> controls given back to X-Plane\n");
            }
            if (c == 'L' || c == 'B') {
                // The standard switch, or the first two-way value with "landing"/"beacon" in its name.
                std::string nm = c == 'L' ? "sim/cockpit2/switches/landing_lights_on"
                                          : "sim/cockpit2/switches/beacon_on";
                if (indexOf(names.drefs, "sync " + nm) < 0) {
                    const char* word = c == 'L' ? "land" : "beacon";
                    for (auto& e : names.drefs)
                        if (e.rfind("sync ", 0) == 0 && lower(e).find(word) != std::string::npos) {
                            nm = shortName(e);
                            break;
                        }
                }
                int idx = indexOf(names.drefs, "sync " + nm);
                double cur = idx >= 0 && drefs.count(idx) && !drefs[idx].vals.empty() ? drefs[idx].vals[0] : 0;
                setDref(nm, cur > 0.5 ? 0.0 : 1.0);
            }
            if (c == 'A') pressCommand("sim/autopilot/heading");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (connected) {
        Writer w;
        w.header(Msg::Bye);
        send(w);
    }
    sock.close();
    netShutdown();
    return 0;
}
