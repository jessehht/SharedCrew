#!/usr/bin/env python3
"""SharedCrew matchmaking + relay server.

- Hosts register and get a short session code (e.g. K7P-4QX).
- A guest joins with that code; the server tells both sides each other's address
  so they can connect directly (UDP hole punching).
- If that fails, game packets are relayed through this server.

Python 3 standard library only. One UDP port. Keeps everything in memory.
"""
import argparse
import asyncio
import logging
import secrets
import struct
import time

MAGIC = b"SCSV"
VERSION = 1

REGISTER, REGISTERED, JOIN, JOIN_OK, JOIN_FAIL, PEER, RELAY, UNREGISTER = range(1, 9)
FAIL_UNKNOWN, FAIL_FULL, FAIL_SLOW_DOWN = 1, 2, 3

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"  # no I, L, O, U
SESSION_TIMEOUT = 60.0          # host must refresh at least this often
RELAY_LIMIT_BYTES = 256 * 1024  # per session per second (a flight uses ~20-60 KB/s)
MAX_SESSIONS = 2000
JOINS_PER_MINUTE = 20           # per IP, stops people guessing codes

log = logging.getLogger("sharedcrew")


def ip_to_int(ip: str) -> int:
    a, b, c, d = (int(x) for x in ip.split("."))
    return (a << 24) | (b << 16) | (c << 8) | d


def int_to_ip(v: int) -> str:
    return f"{(v >> 24) & 255}.{(v >> 16) & 255}.{(v >> 8) & 255}.{v & 255}"


class Session:
    __slots__ = ("code", "host_token", "host_addr", "host_lan", "guest_token", "guest_addr", "guest_lan",
                 "last_seen", "relay_window", "relay_bytes", "relayed_total")

    def __init__(self, code, host_token, host_addr, host_lan):
        self.code = code
        self.host_token = host_token
        self.host_addr = host_addr
        self.host_lan = host_lan
        self.guest_token = None
        self.guest_addr = None
        self.guest_lan = (0, 0)
        self.last_seen = time.monotonic()
        self.relay_window = int(time.monotonic())
        self.relay_bytes = 0
        self.relayed_total = 0


class Server(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport = None
        self.by_code = {}
        self.by_host_token = {}
        self.by_addr = {}  # (ip, port) -> Session, for relaying
        self.join_counts = {}  # ip -> (minute, count)

    # --- helpers

    def send(self, addr, msg_type, payload=b""):
        self.transport.sendto(MAGIC + bytes([VERSION, msg_type]) + payload, addr)

    def new_code(self):
        while True:
            code = "".join(secrets.choice(ALPHABET) for _ in range(6))
            if code not in self.by_code:
                return code

    def drop(self, s):
        self.by_code.pop(s.code, None)
        self.by_host_token.pop(s.host_token, None)
        if self.by_addr.get(s.host_addr) is s:
            del self.by_addr[s.host_addr]
        if s.guest_addr and self.by_addr.get(s.guest_addr) is s:
            del self.by_addr[s.guest_addr]

    def join_allowed(self, ip):
        minute = int(time.monotonic() // 60)
        m, n = self.join_counts.get(ip, (minute, 0))
        if m != minute:
            m, n = minute, 0
        n += 1
        self.join_counts[ip] = (m, n)
        return n <= JOINS_PER_MINUTE

    # --- protocol

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if len(data) < 6 or data[:4] != MAGIC or data[4] != VERSION:
            return
        t = data[5]
        body = data[6:]
        try:
            if t == RELAY:
                self.on_relay(body, addr)
            elif t == REGISTER:
                self.on_register(body, addr)
            elif t == JOIN:
                self.on_join(body, addr)
            elif t == UNREGISTER:
                self.on_unregister(body, addr)
        except struct.error:
            pass

    def on_register(self, body, addr):
        token, lan_ip, lan_port = struct.unpack_from("<QIH", body)
        s = self.by_host_token.get(token)
        if s is None:
            if len(self.by_code) >= MAX_SESSIONS:
                return
            s = Session(self.new_code(), token, addr, (lan_ip, lan_port))
            self.by_code[s.code] = s
            self.by_host_token[token] = s
            log.info("session %s opened by %s:%d", s.code, *addr)
        elif s.host_addr != addr:
            if self.by_addr.get(s.host_addr) is s:
                del self.by_addr[s.host_addr]
            s.host_addr = addr
        s.host_lan = (lan_ip, lan_port)
        s.last_seen = time.monotonic()
        self.by_addr[addr] = s
        self.send(addr, REGISTERED, s.code.encode())

    def on_join(self, body, addr):
        token, code, lan_ip, lan_port = struct.unpack_from("<Q6sIH", body)
        if not self.join_allowed(addr[0]):
            self.send(addr, JOIN_FAIL, bytes([FAIL_SLOW_DOWN]))
            return
        code = code.decode(errors="ignore").upper().replace("O", "0").replace("I", "1").replace("L", "1")
        s = self.by_code.get(code)
        if s is None:
            self.send(addr, JOIN_FAIL, bytes([FAIL_UNKNOWN]))
            return
        if s.guest_token is not None and s.guest_token != token:
            self.send(addr, JOIN_FAIL, bytes([FAIL_FULL]))
            return
        if s.guest_addr and s.guest_addr != addr and self.by_addr.get(s.guest_addr) is s:
            del self.by_addr[s.guest_addr]
        if s.guest_token is None:
            log.info("session %s joined from %s:%d", s.code, *addr)
        s.guest_token = token
        s.guest_addr = addr
        s.guest_lan = (lan_ip, lan_port)
        self.by_addr[addr] = s
        host_ip = ip_to_int(s.host_addr[0])
        guest_ip = ip_to_int(addr[0])
        self.send(addr, JOIN_OK, struct.pack("<IHIH", host_ip, s.host_addr[1], *s.host_lan))
        self.send(s.host_addr, PEER, struct.pack("<IHIH", guest_ip, addr[1], lan_ip, lan_port))

    def on_unregister(self, body, addr):
        (token,) = struct.unpack_from("<Q", body)
        s = self.by_host_token.get(token)
        if s and s.host_addr == addr:
            log.info("session %s closed", s.code)
            self.drop(s)

    def on_relay(self, body, addr):
        s = self.by_addr.get(addr)
        if s is None or s.guest_addr is None:
            return
        if addr == s.host_addr:
            to = s.guest_addr
        elif addr == s.guest_addr:
            to = s.host_addr
        else:
            return
        now = int(time.monotonic())
        if now != s.relay_window:
            s.relay_window, s.relay_bytes = now, 0
        s.relay_bytes += len(body)
        if s.relay_bytes > RELAY_LIMIT_BYTES:
            return
        if s.relayed_total == 0:
            log.info("session %s is using the relay", s.code)
        s.relayed_total += len(body)
        s.last_seen = time.monotonic()
        self.send(to, RELAY, body)

    # --- housekeeping

    def expire(self):
        now = time.monotonic()
        for s in list(self.by_code.values()):
            if now - s.last_seen > SESSION_TIMEOUT:
                log.info("session %s expired (relayed %d KB)", s.code, s.relayed_total // 1024)
                self.drop(s)
        minute = int(now // 60)
        self.join_counts = {ip: v for ip, v in self.join_counts.items() if v[0] == minute}


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=51010)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")

    loop = asyncio.get_running_loop()
    _, server = await loop.create_datagram_endpoint(Server, local_addr=(args.bind, args.port))
    log.info("SharedCrew server listening on UDP %s:%d", args.bind, args.port)
    while True:
        await asyncio.sleep(10)
        server.expire()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
