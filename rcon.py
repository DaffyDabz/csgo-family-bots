#!/usr/bin/env python3
"""Tiny Source RCON client for the CS:GO server.

  rcon.py <command text>      -> prints the server's reply
  rcon.py --players           -> prints "players: <humans>/<max>" (bots not counted)

Password: /opt/csgo/rcon_password. Port: $CSGO_PORT or 27025. Exit 1 on failure.
"""
import os
import re
import socket
import struct
import sys

HOST = "127.0.0.1"
PORT = int(os.environ.get("CSGO_PORT", "27025"))
PWFILE = os.environ.get("CSGO_RCON_FILE", "/opt/csgo/rcon_password")
AUTH, EXEC, RESP = 3, 2, 0


def pkt(rid, kind, body):
    data = struct.pack("<ii", rid, kind) + body.encode("utf-8") + b"\x00\x00"
    return struct.pack("<i", len(data)) + data


def read_pkt(s):
    def exact(n):
        buf = b""
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("rcon closed")
            buf += chunk
        return buf
    size = struct.unpack("<i", exact(4))[0]
    data = exact(size)
    rid, kind = struct.unpack("<ii", data[:8])
    return rid, kind, data[8:-2].decode("utf-8", "replace")


def rcon(command, timeout=3.0):
    with open(PWFILE) as f:
        pw = f.read().strip()
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    try:
        s.sendall(pkt(1, AUTH, pw))
        while True:                      # an empty RESP comes before the auth reply
            rid, kind, _ = read_pkt(s)
            if kind == 2:
                if rid == -1:
                    raise PermissionError("rcon auth rejected")
                break
        s.sendall(pkt(2, EXEC, command))
        s.sendall(pkt(3, RESP, ""))      # marker: its echo ends a multi-packet reply
        out = []
        while True:
            rid, kind, body = read_pkt(s)
            if rid == 3:
                break
            out.append(body)
        return "".join(out)
    finally:
        s.close()


def main(argv):
    if not argv:
        print(__doc__.strip())
        return 2
    try:
        if argv[0] == "--players":
            st = rcon("status")
            m = re.search(r"players\s*:\s*(\d+)\s+humans?,\s*(\d+)\s+bots?\s*\((\d+)/", st)
            if not m:
                return 1
            print("players: %s/%s" % (m.group(1), m.group(3)))
        else:
            sys.stdout.write(rcon(" ".join(argv)))
        return 0
    except Exception as e:  # noqa: BLE001
        sys.stderr.write("rcon: %s\n" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
