#include "session.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>

#include "XPLMDataAccess.h"
#include "XPLMGraphics.h"
#include "XPLMPlanes.h"
#include "XPLMUtilities.h"
#include "code.h"
#include "log.h"

namespace sc {

namespace {

constexpr double kHelloInterval = 1.0;
constexpr double kConnectTimeout = 120.0;  // time to swap codes if needed
constexpr double kReplyCodeAfter = 6.0;    // guest: show the reply code after this long
constexpr int kMaxStunTries = 12;
constexpr double kStunKeepAlive = 20.0;  // keeps the path through the router open while waiting
constexpr double kPunchTime = 60.0;
constexpr double kLinkTimeout = 8.0;
constexpr double kPingInterval = 1.0;
constexpr double kStateInterval = 1.0 / 30.0;  // 30 position updates per second
constexpr double kScanInterval = 0.05;         // look for switch changes 20x per second
constexpr double kSnapshotInterval = 1.0;      // full resend of all switches, repairs lost packets
constexpr double kResendInterval = 0.25;
constexpr double kInterpDelay = 0.12;   // play position back slightly late so we can interpolate
constexpr double kMaxExtrapolate = 0.3;
constexpr double kTeleportKm = 20.0;  // farther than this: use X-Plane's proper "place aircraft"
constexpr double kPi = 3.14159265358979323846;

double now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

// Datarefs for position. Looked up once.
struct PosRefs {
    XPLMDataRef lat, lon, elev, psi, theta, phi, p, q, r;
    XPLMDataRef localX, localY, localZ, vx, vy, vz, quat;
    XPLMDataRef overrideJoystick, zulu, date;
    bool ok = false;

    void init() {
        if (ok) return;
        lat = XPLMFindDataRef("sim/flightmodel/position/latitude");
        lon = XPLMFindDataRef("sim/flightmodel/position/longitude");
        elev = XPLMFindDataRef("sim/flightmodel/position/elevation");
        psi = XPLMFindDataRef("sim/flightmodel/position/psi");
        theta = XPLMFindDataRef("sim/flightmodel/position/theta");
        phi = XPLMFindDataRef("sim/flightmodel/position/phi");
        p = XPLMFindDataRef("sim/flightmodel/position/P");
        q = XPLMFindDataRef("sim/flightmodel/position/Q");
        r = XPLMFindDataRef("sim/flightmodel/position/R");
        localX = XPLMFindDataRef("sim/flightmodel/position/local_x");
        localY = XPLMFindDataRef("sim/flightmodel/position/local_y");
        localZ = XPLMFindDataRef("sim/flightmodel/position/local_z");
        vx = XPLMFindDataRef("sim/flightmodel/position/local_vx");
        vy = XPLMFindDataRef("sim/flightmodel/position/local_vy");
        vz = XPLMFindDataRef("sim/flightmodel/position/local_vz");
        quat = XPLMFindDataRef("sim/flightmodel/position/q");
        overrideJoystick = XPLMFindDataRef("sim/operation/override/override_joystick");
        zulu = XPLMFindDataRef("sim/time/zulu_time_sec");
        date = XPLMFindDataRef("sim/time/local_date_days");
        ok = true;
    }
};
PosRefs g_pos;

double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double d2r = kPi / 180.0;
    double dLat = (lat2 - lat1) * d2r, dLon = (lon2 - lon1) * d2r;
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(lat1 * d2r) * std::cos(lat2 * d2r) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

float lerpAngle(float a, float b, double f) {
    float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    float r = a + static_cast<float>(d * f);
    if (r < 0) r += 360.0f;
    if (r >= 360.0f) r -= 360.0f;
    return r;
}

uint64_t newToken() {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ static_cast<uint64_t>(now() * 1e6);
}

// "k7p-4qx" -> "K7P4QX". Empty if it is not a 6-character server code.
std::string normalizeShortCode(const std::string& text) {
    static const char kAlpha[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::string out;
    for (char c : text) {
        if (c == '-' || c == ' ') continue;
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        if (!std::strchr(kAlpha, c) || c == 0) return {};
        out += c;
    }
    return out.size() == 6 ? out : std::string();
}

bool differs(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i) {
        double tol = 1e-5 * std::max(1.0, std::fabs(a[i]));
        if (std::fabs(a[i] - b[i]) > tol) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Start / stop

bool Session::startHost(uint16_t port) {
    stop("", false);
    std::string err;
    if (!sock_.open(port, err)) {
        status_ = err;
        log(err);
        return false;
    }
    g_pos.init();
    role_ = Role::Host;
    phase_ = Phase::Waiting;
    hostIsMaster_ = true;
    startTime_ = now();
    localPort_ = port;
    token_ = newToken();
    nat_.start(port, true, serverAddress_);
    status_ = "Making your session code...";
    log("hosting on port " + std::to_string(port) + ", profile: " + profile_.title);
    return true;
}

bool Session::join(const std::string& codeOrHost, uint16_t port) {
    stop("", false);
    std::string err;
    Endpoint pub, lan;
    std::string shortCode = normalizeShortCode(codeOrHost);
    if (!shortCode.empty()) {
        if (serverAddress_.empty()) {
            status_ = "Short codes need the SharedCrew server, which is turned off";
            return false;
        }
        viaServer_ = true;  // the server will tell us where the host is
        joinCode_ = shortCode;
    } else if (decodeCode(codeOrHost, pub, lan)) {
        // Try the internet address and the home-network address (works when both PCs are at home).
        if (pub.valid()) candidates_.push_back(pub);
        if (lan.valid() && lan != pub) candidates_.push_back(lan);
    } else {
        Endpoint ep;
        if (!resolveHost(codeOrHost, port, ep, err)) {
            status_ = "That is not a valid session code or address";
            return false;
        }
        candidates_.push_back(ep);
    }
    if (!sock_.open(0, err)) {
        status_ = err;
        log(err);
        candidates_.clear();
        viaServer_ = false;
        return false;
    }
    g_pos.init();
    role_ = Role::Guest;
    phase_ = Phase::Connecting;
    hostIsMaster_ = true;
    startTime_ = now();
    lastHello_ = 0;
    localPort_ = sock_.localPort();
    token_ = newToken();
    nat_.start(localPort_, false, viaServer_ ? serverAddress_ : std::string());
    if (viaServer_) {
        status_ = "Looking up session " + joinCode_.substr(0, 3) + "-" + joinCode_.substr(3) + "...";
        log("joining session " + joinCode_ + " via the SharedCrew server");
    } else {
        status_ = "Connecting...";
        std::string list;
        for (auto& c : candidates_) list += " " + c.str();
        log("joining, trying" + list);
    }
    return true;
}

bool Session::letIn(const std::string& replyCode) {
    if (!waitingForGuest()) return false;
    Endpoint pub, lan;
    if (!decodeCode(replyCode, pub, lan)) {
        status_ = "That is not a valid reply code";
        return false;
    }
    punchTargets_.clear();
    if (pub.valid()) punchTargets_.push_back(pub);
    if (lan.valid() && lan != pub) punchTargets_.push_back(lan);
    punchUntil_ = now() + kPunchTime;
    lastPunch_ = 0;
    status_ = "Opening the way for the other pilot...";
    log("letting in guest at " + pub.str());
    return true;
}

std::string Session::myCode() const {
    if (phase_ == Phase::Idle || phase_ == Phase::Connected) return {};
    NatResult nr = nat_.result();
    Endpoint lan{nr.lanIp, localPort_};
    if (role_ == Role::Guest) return stunOk_ ? encodeCode(stunPublic_, lan) : std::string();

    // Host: the short code from the server is the easiest. Give the server a few seconds.
    if (!shortCode_.empty()) return shortCode_.substr(0, 3) + "-" + shortCode_.substr(3);
    const double waited = now() - startTime_;
    if (!serverAddress_.empty() && waited < 5.0 && !(nr.serverResolved && !nr.server.valid())) return {};

    // No server: long code. Wait for the router (UPnP) and the public address (STUN).
    const bool stunFinished = stunOk_ || stunTries_ >= kMaxStunTries;
    if (!(nr.upnpDone || waited > 8.0) || !(stunFinished || nr.upnpOk)) return {};

    Endpoint pub;
    if (nr.upnpOk && (!stunOk_ || stunPublic_.ip == nr.upnpExternalIp))
        pub = Endpoint{nr.upnpExternalIp, nr.upnpPort};
    else if (stunOk_)
        pub = stunPublic_;
    if (!pub.valid() && !lan.valid()) return {};
    return encodeCode(pub, lan);
}

bool Session::needsReplyCode() const {
    // With the server, it swaps addresses for us (and relays if needed), so no reply code.
    return role_ == Role::Guest && !viaServer_ && phase_ == Phase::Connecting && stunOk_ &&
           now() - startTime_ > kReplyCodeAfter;
}

bool Session::isCandidate(const Endpoint& e) const {
    for (const auto& c : candidates_)
        if (c == e) return true;
    return false;
}

void Session::updateNat(double t) {
    for (const auto& line : nat_.takeLogs()) log(line);
    NatResult nr = nat_.result();

    // Ask a public STUN server what our internet address is (and keep the router path open).
    if (nr.stunResolved && !nr.stunServers.empty()) {
        bool ask = (!stunOk_ && stunTries_ < kMaxStunTries && t - lastStun_ >= 0.5) ||
                   (stunOk_ && phase_ != Phase::Connected && t - lastStun_ >= kStunKeepAlive);
        if (ask) {
            auto req = makeStunRequest();
            const Endpoint& server = nr.stunServers[stunTries_ % nr.stunServers.size()];
            sock_.sendTo(server, req.data(), req.size());
            lastStun_ = t;
            ++stunTries_;
            if (!stunOk_ && stunTries_ == kMaxStunTries) log("could not reach a STUN server");
        }
    }

    if (role_ == Role::Host && phase_ == Phase::Waiting) {
        // Send a few packets towards the guest so our router lets their packets in.
        if (!punchTargets_.empty() && t < punchUntil_ && t - lastPunch_ >= 0.3) {
            lastPunch_ = t;
            Writer w;
            w.header(Msg::Punch);
            for (const auto& p : punchTargets_) sendGame(p, w.buf);
        }
        if (punchTargets_.empty() && !myCode().empty()) {
            status_ = nr.upnpOk ? "Waiting for the other pilot (router opened automatically)"
                                : "Waiting for the other pilot";
        }
    }

    if (needsReplyCode()) status_ = "Can't reach the host yet. Send your reply code to the host.";

    if (nr.serverResolved && nr.server.valid()) {
        server_ = nr.server;
        updateServer(t);
    } else if (viaServer_ && nr.serverResolved) {
        stop("Can't find the SharedCrew server. Use the long code instead.", false);
    }
}

void Session::updateServer(double t) {
    if (role_ == Role::Host && phase_ == Phase::Waiting) {
        // Register, and keep registering so the server keeps our code (also keeps the router path open).
        double every = shortCode_.empty() ? 1.0 : 15.0;
        if (t - lastRegister_ >= every) {
            lastRegister_ = t;
            Writer w;
            w.put<uint64_t>(token_);
            w.put<uint32_t>(nat_.result().lanIp);
            w.put<uint16_t>(localPort_);
            sendServer(Srv::Register, w);
        }
    }
    if (role_ == Role::Guest && phase_ == Phase::Connecting && viaServer_) {
        if (!joinOk_ && t - lastJoin_ >= 1.0) {
            if (joinTries_ >= 8) {
                stop("No answer from the SharedCrew server. Use the long code instead.", false);
                return;
            }
            lastJoin_ = t;
            ++joinTries_;
            Writer w;
            w.put<uint64_t>(token_);
            w.buf.insert(w.buf.end(), joinCode_.begin(), joinCode_.end());
            w.put<uint32_t>(nat_.result().lanIp);
            w.put<uint16_t>(localPort_);
            sendServer(Srv::Join, w);
        }
        // Direct connection not working after a few seconds: go through the server.
        if (joinOk_ && !relayAdded_ && t - joinOkAt_ > 4.0) {
            relayAdded_ = true;
            candidates_.push_back(server_);
            status_ = "Connecting through the SharedCrew server...";
            log("direct connection not working, using the relay");
        }
    }
}

void Session::sendServer(Srv type, const Writer& body) {
    if (!server_.valid()) return;
    std::vector<uint8_t> p(kServerMagic, kServerMagic + 4);
    p.push_back(kServerVersion);
    p.push_back(static_cast<uint8_t>(type));
    p.insert(p.end(), body.buf.begin(), body.buf.end());
    sock_.sendTo(server_, p.data(), p.size());
}

void Session::sendGame(const Endpoint& to, const std::vector<uint8_t>& data) {
    if (!to.valid()) return;
    if (server_.valid() && to == server_) {
        Writer w;
        w.buf = data;
        sendServer(Srv::Relay, w);
        return;
    }
    sock_.sendTo(to, data.data(), data.size());
}

void Session::handleServer(const uint8_t* data, size_t len, double t) {
    Reader r(data + 6, len - 6);
    Srv type = static_cast<Srv>(data[5]);
    switch (type) {
        case Srv::Relay:
            // A game packet from the other pilot, passed on by the server.
            handlePacket(server_, data + 6, len - 6, t);
            break;
        case Srv::Registered:
            if (role_ == Role::Host && len >= 12) {
                std::string code(reinterpret_cast<const char*>(data + 6), 6);
                if (code != shortCode_) log("session code " + code);
                shortCode_ = code;
            }
            break;
        case Srv::Peer: {
            if (role_ != Role::Host || phase_ != Phase::Waiting) break;
            Endpoint pub, lan;
            pub.ip = r.get<uint32_t>();
            pub.port = r.get<uint16_t>();
            lan.ip = r.get<uint32_t>();
            lan.port = r.get<uint16_t>();
            if (!r.good()) break;
            punchTargets_.clear();
            if (pub.valid()) punchTargets_.push_back(pub);
            if (lan.valid() && lan != pub) punchTargets_.push_back(lan);
            punchUntil_ = t + kPunchTime;
            lastPunch_ = 0;
            status_ = "Other pilot found, connecting...";
            log("server: guest at " + pub.str());
            break;
        }
        case Srv::JoinOk: {
            if (role_ != Role::Guest || phase_ != Phase::Connecting || joinOk_) break;
            Endpoint pub, lan;
            pub.ip = r.get<uint32_t>();
            pub.port = r.get<uint16_t>();
            lan.ip = r.get<uint32_t>();
            lan.port = r.get<uint16_t>();
            if (!r.good()) break;
            joinOk_ = true;
            joinOkAt_ = t;
            if (pub.valid()) candidates_.push_back(pub);
            if (lan.valid() && lan != pub) candidates_.push_back(lan);
            lastHello_ = 0;  // say hello straight away
            status_ = "Host found, connecting...";
            log("server: host at " + pub.str());
            break;
        }
        case Srv::JoinFail: {
            if (role_ != Role::Guest || phase_ != Phase::Connecting) break;
            uint8_t reason = r.get<uint8_t>();
            stop(reason == 2   ? "That session already has two pilots."
                 : reason == 3 ? "Too many tries. Wait a minute and try again."
                               : "Session code not found. Check the code, or ask the host for a new one.",
                 false);
            break;
        }
        default:
            break;
    }
}

void Session::stop(const std::string& reason, bool notifyPeer) {
    if (notifyPeer && phase_ == Phase::Connected) {
        Writer w;
        w.header(Msg::Bye);
        send(w);
    }
    if (role_ == Role::Host && !shortCode_.empty()) {
        Writer w;
        w.put<uint64_t>(token_);
        sendServer(Srv::Unregister, w);
    }
    setOverrides(false);
    sock_.close();
    nat_.stop();
    server_ = Endpoint{};
    peerToken_ = 0;
    shortCode_.clear();
    lastRegister_ = 0;
    viaServer_ = false;
    joinCode_.clear();
    joinTries_ = 0;
    lastJoin_ = 0;
    joinOk_ = false;
    joinOkAt_ = 0;
    relayAdded_ = false;
    candidates_.clear();
    stunOk_ = false;
    stunPublic_ = Endpoint{};
    stunTries_ = 0;
    lastStun_ = 0;
    punchTargets_.clear();
    punchUntil_ = 0;
    if (!reason.empty()) log("session ended: " + reason);
    role_ = Role::None;
    phase_ = Phase::Idle;
    peer_ = Endpoint{};
    status_ = reason.empty() ? "Not connected" : reason;
    outQueue_.clear();
    inBuffer_.clear();
    snaps_.clear();
    haveOffset_ = false;
    placed_ = false;
    rtt_ = 0;
}

bool Session::iAmFlying() const {
    if (phase_ != Phase::Connected) return true;
    return role_ == Role::Host ? hostIsMaster_ : !hostIsMaster_;
}

void Session::takeControls() {
    if (phase_ != Phase::Connected || iAmFlying()) return;
    if (profile_.fixedRoles) {
        status_ = "In this aircraft both pilots fly at the same time";
        return;
    }
    if (role_ == Role::Host) {
        setHostIsMaster(true);
        sendReliable(RelKind::ControlState, {1});
    } else {
        sendReliable(RelKind::TakeControl, {});
        status_ = "Asking for controls...";
    }
}

void Session::setHostIsMaster(bool v) {
    if (hostIsMaster_ == v) return;
    hostIsMaster_ = v;
    snaps_.clear();
    haveOffset_ = false;
    log(iAmFlying() ? "you have the controls" : "the other pilot has the controls");
    status_ = iAmFlying() ? "Connected - you have the controls" : "Connected - other pilot has the controls";
}

// ---------------------------------------------------------------------------
// Main loop

void Session::update() {
    if (phase_ == Phase::Idle) return;
    const double t = now();
    receiveAll(t);
    if (phase_ == Phase::Idle) return;  // a Bye or Reject may have stopped us
    if (phase_ != Phase::Connected) updateNat(t);

    if (phase_ == Phase::Connecting) {
        if (t - lastHello_ >= kHelloInterval) sendHello();
        if (t - startTime_ > kConnectTimeout) stop("No answer from the host. Check the session code.", false);
        return;
    }
    if (phase_ != Phase::Connected) return;

    if (t - lastRecv_ > kLinkTimeout) {
        stop("Connection lost", false);
        return;
    }
    if (t - lastPing_ >= kPingInterval) {
        lastPing_ = t;
        Writer w;
        w.header(Msg::Ping);
        w.put<double>(t);
        send(w);
    }
    resendReliable(t);

    // Every change goes reliably and in order with button presses, so a pressed command
    // (e.g. gear toggle) and the switch it moves always arrive in the same order on both PCs.
    // MCDU keys and other short-lived values are checked every frame, the rest 20x per second.
    scanLocalChanges(true);
    if (t - lastScan_ >= kScanInterval) {
        lastScan_ = t;
        scanLocalChanges(false);
    }
    sendChangesReliably();
    if (t - lastSnapshot_ >= kSnapshotInterval) {
        lastSnapshot_ = t;
        sendDrefs(false);
    }

    if (iAmFlying()) {
        setOverrides(false);
        if (t - lastState_ >= kStateInterval) {
            lastState_ = t;
            sendState(t);
        }
    } else {
        applyRemoteState(t);
        if (t - lastState_ >= kStateInterval) {
            lastState_ = t;
            sendBack();
        }
    }
}

void Session::send(const Writer& w) {
    sendGame(peer_, w.buf);
}

void Session::receiveAll(double t) {
    uint8_t buf[2048];
    Endpoint from;
    for (int i = 0; i < 500; ++i) {
        int n = sock_.recvFrom(from, buf, sizeof(buf));
        if (n <= 0) break;
        if (n >= 6 && std::memcmp(buf, kServerMagic, 4) == 0) {
            if (buf[4] == kServerVersion && server_.valid() && from == server_)
                handleServer(buf, static_cast<size_t>(n), t);
            if (phase_ == Phase::Idle) break;
            continue;
        }
        if (isStunPacket(buf, static_cast<size_t>(n))) {
            Endpoint pub;
            if (!stunOk_ && parseStunResponse(buf, static_cast<size_t>(n), pub)) {
                stunPublic_ = pub;
                stunOk_ = true;
                log("internet address: " + pub.str());
            }
            continue;
        }
        handlePacket(from, buf, static_cast<size_t>(n), t);
        if (phase_ == Phase::Idle) break;
    }
}

void Session::handlePacket(const Endpoint& from, const uint8_t* data, size_t len, double t) {
    Reader r(data, len);
    if (r.get<uint32_t>() != kMagic) return;
    uint16_t version = r.get<uint16_t>();
    Msg type = static_cast<Msg>(r.get<uint8_t>());
    r.get<uint8_t>();
    if (!r.good()) return;

    // Host: first contact
    if (role_ == Role::Host && type == Msg::Hello) {
        std::string acf = r.str();
        uint32_t hash = r.get<uint32_t>();
        std::string plugVer = r.str();
        uint64_t token = r.remaining() >= 8 ? r.get<uint64_t>() : 0;
        if (phase_ == Phase::Connected && from != peer_) {
            // Same guest arriving over a second path (direct + relay): ignore it.
            if (token != 0 && token == peerToken_) return;
            Writer w;
            w.header(Msg::Reject);
            w.str("This session already has two pilots.");
            sendGame(from, w.buf);
            return;
        }
        if (phase_ == Phase::Connected) {  // our Welcome got lost, send it again
            sendWelcome();
            return;
        }
        std::string reason;
        if (version != kProtocolVersion) {
            reason = "Different SharedCrew version (host has " SHAREDCREW_VERSION ", you have " + plugVer + ").";
        } else if (acf != acfFile_) {
            reason = "Different aircraft. The host is flying " + acfFile_ + ", you have " + acf + " loaded.";
        } else if (hash != profile_.hash) {
            reason = "Aircraft profiles are not the same. Make sure both pilots have the same SharedCrew version.";
        }
        if (!reason.empty()) {
            Writer w;
            w.header(Msg::Reject);
            w.str(reason);
            sendGame(from, w.buf);
            log("rejected " + from.str() + ": " + reason);
            return;
        }
        peer_ = from;
        peerToken_ = token;
        phase_ = Phase::Connected;
        lastRecv_ = t;
        status_ = "Connected - you have the controls";
        log("pilot joined from " + from.str() + (server_.valid() && from == server_ ? " (through the relay)" : ""));
        sendWelcome();
        beginSync();
        return;
    }

    // Guest: the host answers on whichever address got through.
    if (role_ == Role::Guest && phase_ == Phase::Connecting && (type == Msg::Welcome || type == Msg::Reject) &&
        isCandidate(from))
        peer_ = from;

    if (from != peer_ || version != kProtocolVersion) return;
    lastRecv_ = t;

    switch (type) {
        case Msg::Welcome: {
            if (role_ != Role::Guest || phase_ != Phase::Connecting) return;
            bool hostMaster = r.get<uint8_t>() != 0;
            float zulu = r.get<float>();
            int32_t date = r.get<int32_t>();
            if (!r.good()) return;
            phase_ = Phase::Connected;
            hostIsMaster_ = hostMaster;
            // Same date and time as the host (the test peer sends -1 to leave it alone).
            if (g_pos.date && date >= 0) XPLMSetDatai(g_pos.date, date);
            if (g_pos.zulu && zulu >= 0.0f) XPLMSetDataf(g_pos.zulu, zulu);
            status_ = iAmFlying() ? "Connected - you have the controls" : "Connected - other pilot has the controls";
            log("connected to " + peer_.str());
            beginSync();
            break;
        }
        case Msg::Reject: {
            std::string reason = r.str();
            stop("Host refused: " + reason, false);
            break;
        }
        case Msg::Ping: {
            double ts = r.get<double>();
            Writer w;
            w.header(Msg::Pong);
            w.put<double>(ts);
            send(w);
            break;
        }
        case Msg::Pong: {
            double ts = r.get<double>();
            if (r.good()) rtt_ = t - ts;
            break;
        }
        case Msg::State:
            if (phase_ == Phase::Connected) receiveState(r, t);
            break;
        case Msg::Back:
            if (phase_ == Phase::Connected && iAmFlying()) applyBack(r);
            break;
        case Msg::Drefs:
            if (phase_ == Phase::Connected) applyDrefs(r);
            break;
        case Msg::Reliable: {
            uint32_t id = r.get<uint32_t>();
            if (!r.good()) return;
            Writer ack;
            ack.header(Msg::Ack);
            ack.put<uint32_t>(id);
            send(ack);
            if (id < nextInId_) return;  // duplicate
            std::vector<uint8_t> rest(data + (len - r.remaining()), data + len);
            inBuffer_[id] = std::move(rest);
            // Deliver in order
            while (!inBuffer_.empty() && inBuffer_.begin()->first == nextInId_) {
                auto msg = std::move(inBuffer_.begin()->second);
                inBuffer_.erase(inBuffer_.begin());
                ++nextInId_;
                Reader mr(msg.data(), msg.size());
                RelKind kind = static_cast<RelKind>(mr.get<uint8_t>());
                if (mr.good()) handleReliable(kind, mr);
                if (phase_ != Phase::Connected) return;
            }
            break;
        }
        case Msg::Ack: {
            uint32_t id = r.get<uint32_t>();
            outQueue_.erase(std::remove_if(outQueue_.begin(), outQueue_.end(),
                                           [id](const Pending& p) { return p.id == id; }),
                            outQueue_.end());
            break;
        }
        case Msg::Bye:
            stop("The other pilot left the session", false);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Handshake

void Session::sendHello() {
    lastHello_ = now();
    Writer w;
    w.header(Msg::Hello);
    w.str(acfFile_);
    w.put<uint32_t>(profile_.hash);
    w.str(SHAREDCREW_VERSION);
    w.put<uint64_t>(token_);
    w.str(acfRelPath_);  // only used by the test peer to find the aircraft's files
    for (const auto& c : candidates_) sendGame(c, w.buf);
}

void Session::sendWelcome() {
    Writer w;
    w.header(Msg::Welcome);
    w.put<uint8_t>(hostIsMaster_ ? 1 : 0);
    w.put<float>(g_pos.zulu ? XPLMGetDataf(g_pos.zulu) : 0.0f);
    w.put<int32_t>(g_pos.date ? XPLMGetDatai(g_pos.date) : 0);
    send(w);
}

void Session::beginSync() {
    // The host's cockpit wins at the start: its switches get version 1, the guest's version 0.
    const uint8_t me = role_ == Role::Host ? 0 : 1;
    for (auto& d : profile_.drefs) {
        d.version = role_ == Role::Host ? 1 : 0;
        d.origin = me;
        d.dirty = false;
        if (d.resolved()) d.read(d.lastKnown);
    }
    for (auto& c : profile_.commands) c.suppressBegin = c.suppressEnd = 0;
    nextOutId_ = nextInId_ = 1;
    outQueue_.clear();
    inBuffer_.clear();
    snaps_.clear();
    haveOffset_ = false;
    placed_ = false;
    lastSnapshot_ = 0;  // send a full snapshot straight away
    lastPing_ = 0;
}

// ---------------------------------------------------------------------------
// Reliable messages (commands, control hand-over)

void Session::sendReliable(RelKind kind, const std::vector<uint8_t>& payload) {
    Writer w;
    w.header(Msg::Reliable);
    w.put<uint32_t>(nextOutId_);
    w.put<uint8_t>(static_cast<uint8_t>(kind));
    w.buf.insert(w.buf.end(), payload.begin(), payload.end());
    send(w);
    outQueue_.push_back({nextOutId_, w.buf, now()});
    ++nextOutId_;
}

void Session::resendReliable(double t) {
    for (auto& p : outQueue_) {
        if (t - p.lastSent >= kResendInterval) {
            p.lastSent = t;
            sendGame(peer_, p.data);
        }
    }
}

void Session::handleReliable(RelKind kind, Reader& r) {
    switch (kind) {
        case RelKind::Command: {
            uint16_t idx = r.get<uint16_t>();
            uint8_t phase = r.get<uint8_t>();
            if (!r.good() || idx >= profile_.commands.size()) return;
            auto& c = profile_.commands[idx];
            if (!c.ref) return;
            if (phase == xplm_CommandBegin) {
                c.suppressBegin++;
                XPLMCommandBegin(c.ref);
            } else if (phase == xplm_CommandEnd) {
                c.suppressEnd++;
                XPLMCommandEnd(c.ref);
            }
            break;
        }
        case RelKind::Drefs:
            applyDrefs(r);
            break;
        case RelKind::TakeControl:
            if (role_ == Role::Host && !profile_.fixedRoles) {
                setHostIsMaster(false);
                sendReliable(RelKind::ControlState, {0});
            }
            break;
        case RelKind::ControlState: {
            uint8_t v = r.get<uint8_t>();
            if (r.good() && role_ == Role::Guest) setHostIsMaster(v != 0);
            break;
        }
    }
}

void Session::onLocalCommand(int index, int phase) {
    if (index < 0 || index >= static_cast<int>(profile_.commands.size())) return;
    auto& c = profile_.commands[index];
    // Replays of the other pilot's presses must not be sent back.
    if (phase == xplm_CommandBegin && c.suppressBegin > 0) {
        c.suppressBegin--;
        return;
    }
    if (phase == xplm_CommandEnd && c.suppressEnd > 0) {
        c.suppressEnd--;
        return;
    }
    if (phase_ != Phase::Connected || phase == xplm_CommandContinue) return;
    std::vector<uint8_t> payload(3);
    uint16_t idx = static_cast<uint16_t>(index);
    std::memcpy(payload.data(), &idx, 2);
    payload[2] = static_cast<uint8_t>(phase);
    sendReliable(RelKind::Command, payload);
}

// ---------------------------------------------------------------------------
// Switches and knobs (two-way, last change wins)

void Session::scanLocalChanges(bool priority) {
    const uint8_t me = role_ == Role::Host ? 0 : 1;
    std::vector<double> cur;
    for (auto& d : profile_.drefs) {
        if (d.priority != priority || !sendsChanges(d) || !d.resolved()) continue;
        d.read(cur);
        if (differs(cur, d.lastKnown)) {
            d.lastKnown = cur;
            d.version++;
            d.origin = me;
            d.dirty = true;
        }
    }
}

void Session::sendDrefs(bool onlyDirty) {
    Writer w;
    uint16_t count = 0;
    size_t countPos = 0;
    auto startPacket = [&]() {
        w = Writer{};
        w.header(Msg::Drefs);
        countPos = w.size();
        w.put<uint16_t>(0);
        count = 0;
    };
    auto flush = [&]() {
        if (count == 0) return;
        std::memcpy(w.buf.data() + countPos, &count, sizeof(count));
        send(w);
    };

    startPacket();
    for (size_t i = 0; i < profile_.drefs.size(); ++i) {
        auto& d = profile_.drefs[i];
        if (!sendsChanges(d) || !d.resolved()) continue;
        if (onlyDirty && !d.dirty) continue;
        if (onlyDirty) d.dirty = false;
        size_t need = 2 + 4 + 1 + 1 + d.lastKnown.size() * 8;
        if (w.size() + need > kMaxPacket) {
            flush();
            startPacket();
        }
        w.put<uint16_t>(static_cast<uint16_t>(i));
        w.put<uint32_t>(d.version);
        w.put<uint8_t>(d.origin);
        w.put<uint8_t>(static_cast<uint8_t>(d.lastKnown.size()));
        for (double v : d.lastKnown) w.put<double>(v);
        ++count;
    }
    flush();
}

void Session::sendChangesReliably() {
    // Same layout as a Drefs packet, but as ordered, acknowledged messages.
    Writer w;
    uint16_t count = 0;
    auto flush = [&]() {
        if (count == 0) return;
        std::vector<uint8_t> payload;
        payload.resize(2);
        std::memcpy(payload.data(), &count, 2);
        payload.insert(payload.end(), w.buf.begin(), w.buf.end());
        sendReliable(RelKind::Drefs, payload);
        w = Writer{};
        count = 0;
    };
    for (size_t i = 0; i < profile_.drefs.size(); ++i) {
        auto& d = profile_.drefs[i];
        if (!d.dirty) continue;
        d.dirty = false;
        size_t need = 2 + 4 + 1 + 1 + d.lastKnown.size() * 8;
        if (w.size() + need > kMaxPacket - 40) flush();
        w.put<uint16_t>(static_cast<uint16_t>(i));
        w.put<uint32_t>(d.version);
        w.put<uint8_t>(d.origin);
        w.put<uint8_t>(static_cast<uint8_t>(d.lastKnown.size()));
        for (double v : d.lastKnown) w.put<double>(v);
        ++count;
    }
    flush();
}

void Session::applyDrefs(Reader& r) {
    uint16_t n = r.get<uint16_t>();
    std::vector<double> vals;
    for (uint16_t k = 0; k < n && r.good(); ++k) {
        uint16_t idx = r.get<uint16_t>();
        uint32_t ver = r.get<uint32_t>();
        uint8_t origin = r.get<uint8_t>();
        uint8_t cnt = r.get<uint8_t>();
        vals.resize(cnt);
        for (auto& v : vals) v = r.get<double>();
        if (!r.good()) return;
        if (idx >= profile_.drefs.size()) continue;
        auto& d = profile_.drefs[idx];
        if (!acceptsChanges(d) || !d.resolved() || static_cast<int>(cnt) != d.count) continue;

        bool newer = ver > d.version || (ver == d.version && origin > d.origin);
        if (!newer) continue;
        d.version = ver;
        d.origin = origin;
        d.dirty = false;
        if (!d.writable) {
            if (!d.warnedReadOnly) {
                log("can't write " + d.label() + " (read-only). Use a command for it in the profile.");
                d.warnedReadOnly = true;
            }
            continue;
        }
        if (differs(vals, d.lastKnown)) {
            d.write(vals);
            d.read(d.lastKnown);  // what the aircraft actually accepted
        }
    }
}

// ---------------------------------------------------------------------------
// Position and controls (one-way from the pilot flying)

void Session::sendState(double t) {
    Writer w;
    w.header(Msg::State);
    w.put<double>(t);
    w.put<double>(XPLMGetDatad(g_pos.lat));
    w.put<double>(XPLMGetDatad(g_pos.lon));
    w.put<double>(XPLMGetDatad(g_pos.elev));
    w.put<float>(XPLMGetDataf(g_pos.psi));
    w.put<float>(XPLMGetDataf(g_pos.theta));
    w.put<float>(XPLMGetDataf(g_pos.phi));
    w.put<float>(XPLMGetDataf(g_pos.p));
    w.put<float>(XPLMGetDataf(g_pos.q));
    w.put<float>(XPLMGetDataf(g_pos.r));

    std::vector<double> vals;
    uint16_t nStream = 0;
    for (const auto& d : profile_.drefs)
        if (d.kind == EntryKind::Stream) ++nStream;
    w.put<uint16_t>(nStream);
    for (const auto& d : profile_.drefs) {
        if (d.kind != EntryKind::Stream) continue;
        if (!d.resolved()) {
            w.put<uint8_t>(0);
            continue;
        }
        d.read(vals);
        w.put<uint8_t>(static_cast<uint8_t>(vals.size()));
        for (double v : vals) w.put<float>(static_cast<float>(v));
    }
    if (w.size() <= kMaxPacket * 2) send(w);
}

void Session::receiveState(Reader& r, double t) {
    if (iAmFlying()) return;  // left over from before a hand-over
    Snap s;
    s.t = r.get<double>();
    s.lat = r.get<double>();
    s.lon = r.get<double>();
    s.elev = r.get<double>();
    s.psi = r.get<float>();
    s.theta = r.get<float>();
    s.phi = r.get<float>();
    s.p = r.get<float>();
    s.q = r.get<float>();
    s.r = r.get<float>();
    uint16_t n = r.get<uint16_t>();
    s.stream.resize(n);
    for (auto& v : s.stream) {
        uint8_t cnt = r.get<uint8_t>();
        v.resize(cnt);
        for (auto& f : v) f = r.get<float>();
    }
    if (!r.good()) return;
    if (!snaps_.empty() && s.t <= snaps_.back().t) return;  // out of order

    // Estimate the clock difference between the two PCs (lowest delay seen, slowly drifting up).
    double off = t - s.t;
    if (!haveOffset_ || off < clockOffset_) {
        clockOffset_ = off;
        haveOffset_ = true;
    } else {
        clockOffset_ += (off - clockOffset_) * 0.01;
    }
    snaps_.push_back(std::move(s));
    while (snaps_.size() > 64) snaps_.pop_front();
}

bool Session::sample(double t, Snap& out) const {
    if (snaps_.empty()) return false;
    if (snaps_.size() == 1 || t <= snaps_.front().t) {
        out = t <= snaps_.front().t ? snaps_.front() : snaps_.back();
        return true;
    }
    const Snap* a = nullptr;
    const Snap* b = nullptr;
    if (t >= snaps_.back().t) {
        a = &snaps_[snaps_.size() - 2];
        b = &snaps_.back();
        t = std::min(t, b->t + kMaxExtrapolate);
    } else {
        for (size_t i = 0; i + 1 < snaps_.size(); ++i) {
            if (snaps_[i].t <= t && t <= snaps_[i + 1].t) {
                a = &snaps_[i];
                b = &snaps_[i + 1];
                break;
            }
        }
    }
    if (!a || !b || b->t <= a->t) {
        out = snaps_.back();
        return true;
    }
    double f = (t - a->t) / (b->t - a->t);
    auto lerp = [f](double x, double y) { return x + (y - x) * f; };
    out.t = t;
    out.lat = lerp(a->lat, b->lat);
    out.lon = lerp(a->lon, b->lon);
    out.elev = lerp(a->elev, b->elev);
    out.psi = lerpAngle(a->psi, b->psi, f);
    out.theta = static_cast<float>(lerp(a->theta, b->theta));
    out.phi = lerpAngle(a->phi, b->phi, f);
    if (out.phi > 180.0f) out.phi -= 360.0f;
    out.p = static_cast<float>(lerp(a->p, b->p));
    out.q = static_cast<float>(lerp(a->q, b->q));
    out.r = static_cast<float>(lerp(a->r, b->r));
    out.stream = b->stream;
    if (a->stream.size() == b->stream.size()) {
        for (size_t i = 0; i < out.stream.size(); ++i) {
            if (a->stream[i].size() != b->stream[i].size()) continue;
            for (size_t j = 0; j < out.stream[i].size(); ++j)
                out.stream[i][j] = static_cast<float>(lerp(a->stream[i][j], b->stream[i][j]));
        }
    }
    return true;
}

void Session::setOverrides(bool on) {
    if (on == overridesOn_ || !g_pos.ok) return;
    overridesOn_ = on;
    // With fixed roles each pilot keeps their own stick (the aircraft combines them), so
    // only the profile's own overrides are used.
    if (g_pos.overrideJoystick && !profile_.fixedRoles) XPLMSetDatai(g_pos.overrideJoystick, on ? 1 : 0);
    std::vector<double> v;
    for (auto& d : profile_.drefs) {
        if (d.kind != EntryKind::Override || !d.resolved()) continue;
        v.assign(d.count, on ? 1.0 : 0.0);
        d.write(v);
    }
}

bool Session::sendsChanges(const DrefEntry& d) const {
    return d.kind == EntryKind::Sync || (d.kind == EntryKind::MasterSync && iAmFlying());
}

bool Session::acceptsChanges(const DrefEntry& d) const {
    return d.kind == EntryKind::Sync || (d.kind == EntryKind::MasterSync && !iAmFlying());
}

void Session::sendBack() {
    uint16_t n = 0;
    for (const auto& d : profile_.drefs)
        if (d.kind == EntryKind::Back) ++n;
    if (n == 0) return;
    Writer w;
    w.header(Msg::Back);
    w.put<uint16_t>(n);
    std::vector<double> vals;
    for (const auto& d : profile_.drefs) {
        if (d.kind != EntryKind::Back) continue;
        if (!d.resolved()) {
            w.put<uint8_t>(0);
            continue;
        }
        d.read(vals);
        w.put<uint8_t>(static_cast<uint8_t>(vals.size()));
        for (double v : vals) w.put<float>(static_cast<float>(v));
    }
    send(w);
}

void Session::applyBack(Reader& r) {
    uint16_t n = r.get<uint16_t>();
    size_t k = 0;
    std::vector<double> vals;
    for (auto& d : profile_.drefs) {
        if (d.kind != EntryKind::Back) continue;
        if (k++ >= n) break;
        uint8_t cnt = r.get<uint8_t>();
        vals.resize(cnt);
        for (auto& v : vals) v = r.get<float>();
        if (!r.good()) return;
        if (d.resolved() && d.writable && static_cast<int>(cnt) == d.count) d.write(vals);
    }
}

void Session::applyRemoteState(double t) {
    if (snaps_.empty() || !haveOffset_) return;

    const double renderT = t - clockOffset_ - kInterpDelay;
    Snap s, s0;
    if (!sample(renderT, s) || !sample(renderT - 0.05, s0)) return;

    // First time, far away: let X-Plane move us properly so scenery loads.
    if (!placed_) {
        placed_ = true;
        double myLat = XPLMGetDatad(g_pos.lat), myLon = XPLMGetDatad(g_pos.lon);
        if (distanceKm(myLat, myLon, s.lat, s.lon) > kTeleportKm) {
            log("other pilot is far away, moving aircraft to their position");
            XPLMPlaceUserAtLocation(s.lat, s.lon, static_cast<float>(s.elev), s.psi, 0.0f);
            return;
        }
    }

    setOverrides(true);

    double x, y, z, x0, y0, z0;
    XPLMWorldToLocal(s.lat, s.lon, s.elev, &x, &y, &z);
    XPLMWorldToLocal(s0.lat, s0.lon, s0.elev, &x0, &y0, &z0);
    double dt = s.t - s0.t;
    if (dt <= 1e-4) dt = 0.05;

    XPLMSetDatad(g_pos.localX, x);
    XPLMSetDatad(g_pos.localY, y);
    XPLMSetDatad(g_pos.localZ, z);
    XPLMSetDataf(g_pos.vx, static_cast<float>((x - x0) / dt));
    XPLMSetDataf(g_pos.vy, static_cast<float>((y - y0) / dt));
    XPLMSetDataf(g_pos.vz, static_cast<float>((z - z0) / dt));
    XPLMSetDataf(g_pos.psi, s.psi);
    XPLMSetDataf(g_pos.theta, s.theta);
    XPLMSetDataf(g_pos.phi, s.phi);
    XPLMSetDataf(g_pos.p, s.p);
    XPLMSetDataf(g_pos.q, s.q);
    XPLMSetDataf(g_pos.r, s.r);

    // Attitude quaternion (formula from the X-Plane SDK docs).
    const double hp = kPi / 360.0 * s.psi, ht = kPi / 360.0 * s.theta, hf = kPi / 360.0 * s.phi;
    float quat[4];
    quat[0] = static_cast<float>(std::cos(hp) * std::cos(ht) * std::cos(hf) + std::sin(hp) * std::sin(ht) * std::sin(hf));
    quat[1] = static_cast<float>(std::cos(hp) * std::cos(ht) * std::sin(hf) - std::sin(hp) * std::sin(ht) * std::cos(hf));
    quat[2] = static_cast<float>(std::cos(hp) * std::sin(ht) * std::cos(hf) + std::sin(hp) * std::cos(ht) * std::sin(hf));
    quat[3] = static_cast<float>(-std::cos(hp) * std::sin(ht) * std::sin(hf) + std::sin(hp) * std::cos(ht) * std::cos(hf));
    XPLMSetDatavf(g_pos.quat, quat, 0, 4);

    // Controls, throttle, fuel... from the pilot flying.
    size_t k = 0;
    std::vector<double> vals;
    for (auto& d : profile_.drefs) {
        if (d.kind != EntryKind::Stream) continue;
        if (k >= s.stream.size()) break;
        const auto& src = s.stream[k++];
        if (!d.resolved() || static_cast<int>(src.size()) != d.count) continue;
        vals.assign(src.begin(), src.end());
        d.write(vals);
    }

    // Drop old samples
    while (snaps_.size() > 2 && snaps_[1].t < renderT - 1.0) snaps_.pop_front();
}

}  // namespace sc
