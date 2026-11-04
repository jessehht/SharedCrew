#!/usr/bin/env python3
"""Checks a running SharedCrew server: register, join, address swap and relay.
Usage: python test_server.py [host] [port]"""
import socket
import struct
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 51010
SRV = (HOST, PORT)
HDR = b"SCSV\x01"


def sock():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    s.settimeout(3)
    return s


def recv(s, want):
    while True:
        data, _ = s.recvfrom(4096)
        if data[:5] == HDR and data[5] == want:
            return data[6:]


ok = True
host, guest, other = sock(), sock(), sock()

host.sendto(HDR + bytes([1]) + struct.pack("<QIH", 111, 0xC0A80002, 51000), SRV)
code = recv(host, 2).decode()
print("1. host registered, code", code[:3] + "-" + code[3:])

guest.sendto(HDR + bytes([3]) + struct.pack("<Q", 222) + code.lower().encode() + struct.pack("<IH", 0xC0A80003, 5000), SRV)
hip, hport, hlan, hlanport = struct.unpack("<IHIH", recv(guest, 4))
print("2. guest joined, host public port", hport, "lan port", hlanport)
ok &= hlanport == 51000

gip, gport, glan, glanport = struct.unpack("<IHIH", recv(host, 6))
print("3. host told about guest, guest lan port", glanport)
ok &= glanport == 5000

guest.sendto(HDR + bytes([7]) + b"SCRW-hello-from-guest", SRV)
got = recv(host, 7)
print("4. relay guest -> host:", got)
ok &= got == b"SCRW-hello-from-guest"

host.sendto(HDR + bytes([7]) + b"SCRW-welcome", SRV)
got = recv(guest, 7)
print("5. relay host -> guest:", got)
ok &= got == b"SCRW-welcome"

other.sendto(HDR + bytes([3]) + struct.pack("<Q", 333) + code.encode() + struct.pack("<IH", 0, 0), SRV)
reason = recv(other, 5)[0]
print("6. third pilot refused, reason", reason)
ok &= reason == 2

other.sendto(HDR + bytes([3]) + struct.pack("<Q", 333) + b"ZZZZZZ" + struct.pack("<IH", 0, 0), SRV)
reason = recv(other, 5)[0]
print("7. unknown code refused, reason", reason)
ok &= reason == 1

host.sendto(HDR + bytes([8]) + struct.pack("<Q", 111), SRV)
print("ALL OK" if ok else "SOMETHING FAILED")
sys.exit(0 if ok else 1)
