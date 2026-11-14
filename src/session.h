#pragma once
// One shared-cockpit session between two X-Plane instances.
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "nat.h"
#include "net.h"
#include "profile.h"
#include "protocol.h"

namespace sc {

enum class Role { None, Host, Guest };
enum class Phase { Idle, Waiting, Connecting, Connected };

class Session {
public:
    explicit Session(Profile& profile) : profile_(profile) {}

    // acfRelPath: path inside X-Plane's Aircraft folder (never the full path: that shows your user name).
    void setAircraft(const std::string& acfFile, const std::string& acfRelPath) {
        acfFile_ = acfFile;
        acfRelPath_ = acfRelPath;
    }
    // SharedCrew server "host:port" for short codes and relay. Empty = off.
    void setServer(const std::string& address) { serverAddress_ = address; }

    bool startHost(uint16_t port);
    // codeOrHost: a session code, or an IP address / host name.
    bool join(const std::string& codeOrHost, uint16_t port);
    // Host: open a path to a guest that can't get through (their reply code).
    bool letIn(const std::string& replyCode);
    void stop(const std::string& reason, bool notifyPeer = true);

    // The code to send to the other pilot. Empty while still being worked out.
    std::string myCode() const;
    // Guest: true once it is time to show the reply code.
    bool needsReplyCode() const;
    bool waitingForGuest() const { return role_ == Role::Host && phase_ == Phase::Waiting; }

    // Call every frame, before the flight model runs.
    void update();

    // Pilot flying hand-over.
    void takeControls();

    // Called by the command handler when a synced command is pressed locally.
    // Returns false if the press came from the other pilot (replay), true otherwise.
    void onLocalCommand(int index, int phase);

    // Status for the window
    Role role() const { return role_; }
    Phase phase() const { return phase_; }
    bool connected() const { return phase_ == Phase::Connected; }
    bool iAmFlying() const;
    // Both pilots fly together and the controls can't be handed over (e.g. ToLiss Airbus).
    bool fixedRoles() const { return profile_.fixedRoles; }
    double pingMs() const { return rtt_ * 1000.0; }
    const std::string& status() const { return status_; }
    std::string peerText() const { return peer_.valid() ? peer_.str() : std::string(); }

private:
    struct Snap {
        double t = 0;
        double lat = 0, lon = 0, elev = 0;
        float psi = 0, theta = 0, phi = 0, p = 0, q = 0, r = 0;
        std::vector<std::vector<float>> stream;
    };
    struct Pending {
        uint32_t id;
        std::vector<uint8_t> data;
        double lastSent;
    };

    void send(const Writer& w);
    void receiveAll(double now);
    void handlePacket(const Endpoint& from, const uint8_t* data, size_t len, double now);
    void handleReliable(RelKind kind, Reader& r);

    void sendHello();
    void sendWelcome();
    void beginSync();
    void sendReliable(RelKind kind, const std::vector<uint8_t>& payload);
    void resendReliable(double now);

    void scanLocalChanges(bool priority);
    void sendPriorityChanges();
    void sendDrefs(bool onlyDirty);
    void applyDrefs(Reader& r);

    bool sendsChanges(const DrefEntry& d) const;
    bool acceptsChanges(const DrefEntry& d) const;
    void sendBack();
    void applyBack(Reader& r);

    void sendState(double now);
    void receiveState(Reader& r, double now);
    bool sample(double t, Snap& out) const;
    void applyRemoteState(double now);
    void setOverrides(bool on);
    void setHostIsMaster(bool v);
    void updateNat(double now);
    void updateServer(double now);
    bool isCandidate(const Endpoint& e) const;
    // All game packets go through here: packets to the server are wrapped for the relay.
    void sendGame(const Endpoint& to, const std::vector<uint8_t>& data);
    void sendServer(Srv type, const Writer& body);
    void handleServer(const uint8_t* data, size_t len, double now);

    Profile& profile_;
    std::string acfFile_;
    std::string acfRelPath_;
    UdpSocket sock_;
    Endpoint peer_;
    Role role_ = Role::None;
    Phase phase_ = Phase::Idle;
    std::string status_ = "Not connected";

    double startTime_ = 0;
    double lastRecv_ = 0;
    double lastHello_ = 0;
    double lastPing_ = 0;
    double lastState_ = 0;
    double lastScan_ = 0;
    double lastSnapshot_ = 0;
    double rtt_ = 0;

    bool hostIsMaster_ = true;
    bool overridesOn_ = false;
    bool placed_ = false;

    uint32_t nextOutId_ = 1;
    uint32_t nextInId_ = 1;
    std::deque<Pending> outQueue_;
    std::map<uint32_t, std::vector<uint8_t>> inBuffer_;

    std::deque<Snap> snaps_;
    double clockOffset_ = 0;
    bool haveOffset_ = false;

    // Getting through routers
    NatHelper nat_;
    uint16_t localPort_ = 0;
    std::vector<Endpoint> candidates_;  // guest: addresses to try for the host
    Endpoint stunPublic_;
    bool stunOk_ = false;
    int stunTries_ = 0;
    double lastStun_ = 0;
    std::vector<Endpoint> punchTargets_;
    double punchUntil_ = 0;
    double lastPunch_ = 0;

    // SharedCrew server
    std::string serverAddress_;
    Endpoint server_;
    uint64_t token_ = 0;         // identifies us to the server and to the host
    uint64_t peerToken_ = 0;     // host: token of the guest that got in
    std::string shortCode_;      // host: code from the server
    double lastRegister_ = 0;
    bool viaServer_ = false;     // guest: joined with a short code
    std::string joinCode_;
    int joinTries_ = 0;
    double lastJoin_ = 0;
    bool joinOk_ = false;
    double joinOkAt_ = 0;
    bool relayAdded_ = false;
};

}  // namespace sc
