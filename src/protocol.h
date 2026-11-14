#pragma once
// Wire format for SharedCrew packets. Little-endian, UDP.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sc {

constexpr uint32_t kMagic = 0x57524353;  // "SCRW"
constexpr uint16_t kProtocolVersion = 2;

// SharedCrew server (matchmaking + relay). Packets start with "SCSV", version, type.
constexpr uint8_t kServerMagic[4] = {'S', 'C', 'S', 'V'};
constexpr uint8_t kServerVersion = 1;
enum class Srv : uint8_t {
    Register = 1,    // host -> server: u64 token, u32 lanIp, u16 lanPort
    Registered = 2,  // server -> host: 6 chars code
    Join = 3,        // guest -> server: u64 token, 6 chars code, u32 lanIp, u16 lanPort
    JoinOk = 4,      // server -> guest: host public ip/port, host lan ip/port
    JoinFail = 5,    // server -> guest: u8 reason
    Peer = 6,        // server -> host: guest public ip/port, guest lan ip/port
    Relay = 7,       // either way: a game packet passed through the server
    Unregister = 8,  // host -> server: u64 token
};
constexpr const char* kDefaultServer = "sharedcrew.mijngms.com:51010";
constexpr size_t kMaxPacket = 1200;  // stay under typical internet MTU
constexpr uint16_t kDefaultPort = 51000;

enum class Msg : uint8_t {
    Hello = 1,     // guest -> host: aircraft, profile hash
    Welcome = 2,   // host -> guest: accepted
    Reject = 3,    // host -> guest: reason
    Ping = 4,
    Pong = 5,
    State = 6,     // pilot flying -> other: position, attitude, controls
    Drefs = 7,     // switch/knob values with versions
    Reliable = 8,  // ordered, acknowledged message (commands, control handover)
    Ack = 9,
    Bye = 10,
    Punch = 11,    // host -> guest: opens a path through the host's router
    Back = 12,     // other pilot -> pilot flying: stick, tiller... (profile "back" entries)
};

enum class RelKind : uint8_t {
    Command = 1,       // u16 command index, u8 phase
    TakeControl = 2,   // guest asks host for controls
    ControlState = 3,  // host -> guest: u8 hostIsMaster
    Drefs = 4,         // switch changes, same layout as Msg::Drefs
    ControlFixed = 5,  // host -> guest: u8 hostHasControl (fixed roles)
};

class Writer {
public:
    std::vector<uint8_t> buf;

    template <class T>
    void put(T v) {
        size_t n = buf.size();
        buf.resize(n + sizeof(T));
        std::memcpy(buf.data() + n, &v, sizeof(T));
    }
    void str(const std::string& s) {
        put<uint16_t>(static_cast<uint16_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void header(Msg m) {
        put<uint32_t>(kMagic);
        put<uint16_t>(kProtocolVersion);
        put<uint8_t>(static_cast<uint8_t>(m));
        put<uint8_t>(0);
    }
    size_t size() const { return buf.size(); }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    template <class T>
    T get() {
        T v{};
        if (pos_ + sizeof(T) > n_) {
            ok_ = false;
            return v;
        }
        std::memcpy(&v, p_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    std::string str() {
        uint16_t len = get<uint16_t>();
        if (!ok_ || pos_ + len > n_) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        return s;
    }
    bool good() const { return ok_; }
    size_t remaining() const { return n_ - pos_; }

private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace sc
