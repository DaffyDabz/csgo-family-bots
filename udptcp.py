#!/usr/bin/env python3
"""UDP-over-TCP hop so a CS:GO client on the SAME Windows PC can reach the
server in WSL.

Why: WSL mirrored mode forwards Windows->WSL localhost for TCP but not UDP, the
PC's own LAN IP loops back into Windows (not WSL), and srcds ignores packets
whose source is 127.x anyway. Other PCs on the LAN do not need this: they send
UDP straight to <LAN IP>:27025.

  WSL (entrypoint starts it):  udptcp.py wsl  127.0.0.1:27027  <lan ip>:27025
  Windows (dev only):          udptcp.py win  127.0.0.1:27026  127.0.0.1:27027
  then in the client:          connect 127.0.0.1:27026

Frames on the TCP leg: 2-byte big-endian length + datagram.
"""
import socket
import struct
import sys
import threading


def hostport(s):
    h, _, p = s.rpartition(":")
    return h, int(p)


def recv_exact(c, n):
    buf = b""
    while len(buf) < n:
        chunk = c.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def send_frame(c, data):
    c.sendall(struct.pack(">H", len(data)) + data)


def recv_frame(c):
    return recv_exact(c, struct.unpack(">H", recv_exact(c, 2))[0])


def wsl_side(listen, target):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(listen)
    srv.listen(8)
    print("[udptcp] wsl: tcp %s:%d -> udp %s:%d" % (listen + target), flush=True)

    def session(c):
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.connect(target)

        def back():
            try:
                while True:
                    send_frame(c, u.recv(65535))
            except OSError:
                pass
        threading.Thread(target=back, daemon=True).start()
        try:
            while True:
                u.send(recv_frame(c))
        except (OSError, ConnectionError):
            pass
        finally:
            c.close()
            u.close()

    while True:
        c, _ = srv.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=session, args=(c,), daemon=True).start()


def win_side(listen, tcp_target):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(listen)
    print("[udptcp] win: udp %s:%d -> tcp %s:%d" % (listen + tcp_target), flush=True)
    peers = {}

    def back(addr, c):
        try:
            while True:
                u.sendto(recv_frame(c), addr)
        except (OSError, ConnectionError):
            peers.pop(addr, None)

    while True:
        data, addr = u.recvfrom(65535)
        c = peers.get(addr)
        if c is None:
            c = socket.create_connection(tcp_target)
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            peers[addr] = c
            threading.Thread(target=back, args=(addr, c), daemon=True).start()
        try:
            send_frame(c, data)
        except OSError:
            peers.pop(addr, None)


if __name__ == "__main__":
    mode, a, b = sys.argv[1], hostport(sys.argv[2]), hostport(sys.argv[3])
    (wsl_side if mode == "wsl" else win_side)(a, b)
