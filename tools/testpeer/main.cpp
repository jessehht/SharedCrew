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

// Same rules as the plugin: default.txt first, then the matching aircraft file, no duplicates.
struct Names {
    std::vector<std::string> drefs;  // index -> name (sync and stream mixed, like the plugin)
    std::vector<std::string> commands;
};

struct Parsed {
    std::vector<std::string> matches;
    bool noDefault = false;
    std::vector<std::pair<std::string, std::string>> lines;
};

Parsed parse(const fs::path& p) {
    Parsed out;
    std::ifstream in(p);
    std::string raw;
    while (std::getline(in, raw)) {
        size_t h = raw.find('#');
        if (h != std::string::npos) raw = raw.substr(0, h);
        std::string line = trim(raw);
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string key, arg;
        ss >> key;
        std::getline(ss, arg);
        key = lower(key);
        arg = trim(arg);
        if (key == "match") out.matches.push_back(lower(arg));
        else if (key == "nodefault") out.noDefault = true;
        else if (key == "sync" || key == "stream" || key == "command") out.lines.emplace_back(key, arg);
    }
    return out;
}

Names loadNames(const fs::path& dir, const std::string& acf) {
    Names n;
    std::vector<fs::path> files;
    if (fs::is_directory(dir))
        for (auto& e : fs::directory_iterator(dir))
            if (e.is_regular_file() && lower(e.path().extension().string()) == ".txt" &&
                lower(e.path().filename().string()) != "default.txt")
                files.push_back(e.path());
    std::sort(files.begin(), files.end());
    Parsed aircraft;
    bool found = false;
    for (auto& f : files) {
        Parsed p = parse(f);
        for (auto& m : p.matches)
            if (lower(acf).find(m) != std::string::npos) found = true;
        if (found) {
            aircraft = p;
            printf("  profile: %s\n", f.filename().string().c_str());
            break;
        }
    }
    std::set<std::string> seen;
    auto add = [&](const Parsed& p) {
        for (auto& [k, a] : p.lines) {
            if (!seen.insert(k + " " + a).second) continue;
            if (k == "command") n.commands.push_back(a);
            else n.drefs.push_back(k + " " + a);
        }
    };
    if (!(found && aircraft.noDefault)) add(parse(dir / "default.txt"));
    if (found) add(aircraft);
    return n;
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

    Endpoint peer;
    bool connected = false;
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

    auto send = [&](const Writer& w) { sock.sendTo(peer, w.buf.data(), w.buf.size()); };
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
            Reader r(buf, n);
            if (r.get<uint32_t>() != kMagic) continue;
            uint16_t ver = r.get<uint16_t>();
            Msg type = static_cast<Msg>(r.get<uint8_t>());
            r.get<uint8_t>();
            if (ver != kProtocolVersion) continue;

            if (type == Msg::Hello) {
                std::string acf = r.str();
                if (!connected) {
                    peer = from;
                    connected = true;
                    iFly = false;
                    drefs.clear();
                    outQueue.clear();
                    outId = inId = 1;
                    printf("X-Plane connected from %s, aircraft %s\n", from.str().c_str(), acf.c_str());
                    names = loadNames(profiles, acf);
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
                            std::string nm = idx < names.drefs.size() ? names.drefs[idx].substr(5) : "#" + std::to_string(idx);
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
                for (auto& [id, data] : outQueue) sock.sendTo(peer, data.data(), data.size());
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
                if (!havePos) {
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
                std::string nm = c == 'L' ? "sim/cockpit2/switches/landing_lights_on"
                                          : "sim/cockpit2/switches/beacon_on";
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
