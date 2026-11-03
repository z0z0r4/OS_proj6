#!/usr/bin/env python3
"""Usage: host_server.py [iperf_port [netperf_port]]

Host-side peers for the guest's network tests (reached at 10.0.2.2 through
qemu's user-mode network):

  TCP 5301   iperf sink: reads until the guest closes, then closes
  UDP 5301   iperf UDP sink (counts datagrams, never replies)
  TCP/UDP 12865  netserver: speaks the same wire protocol as src/user/netserver.c
"""
import socket
import struct
import sys
import threading

MAGIC = 0x4e504552
TCP_STREAM, UDP_STREAM, TCP_RR, UDP_RR, TCP_CRR = 1, 2, 3, 4, 5


def recv_exact(c, n):
    buf = b""
    while len(buf) < n:
        d = c.recv(n - len(buf))
        if not d:
            return buf
        buf += d
    return buf


def iperf_conn(c):
    while c.recv(65536):
        pass
    c.close()


def netserver_conn(c):
    import time
    h = recv_exact(c, 16)
    if len(h) < 16:
        c.close()
        return
    magic, typ, req, rsp = struct.unpack("<IIII", h)
    if magic != MAGIC:
        c.close()
        return
    if typ == TCP_STREAM:
        total, start = 0, None
        while True:
            d = c.recv(65536)
            if not d:
                break
            start = start or time.monotonic()
            total += len(d)
        ns = int((time.monotonic() - start) * 1e9) if start else 0
        c.sendall(struct.pack("<QQQ", total, ns, 0))
    else:
        while True:
            d = recv_exact(c, req)
            if len(d) < req:
                break
            c.sendall(b"r" * rsp)
            if typ == TCP_CRR:
                break
    c.close()


def tcp_server(port, handler):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(16)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handler, args=(c,), daemon=True).start()


def udp_server(port, netserver):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    total = msgs = 0
    import time
    start = None
    while True:
        d, addr = s.recvfrom(65536)
        if not netserver or len(d) < 16:
            continue
        magic, typ, req, rsp = struct.unpack("<IIII", d[:16])
        if magic != MAGIC:
            continue
        if typ == UDP_RR:
            s.sendto(b"r" * max(rsp, 16), addr)
        elif typ == UDP_STREAM and rsp == 1:
            ns = int((time.monotonic() - start) * 1e9) if start else 0
            s.sendto(struct.pack("<QQQ", total, ns, msgs), addr)
            total = msgs = 0
            start = None
        elif typ == UDP_STREAM:
            start = start or time.monotonic()
            total += len(d)
            msgs += 1


if __name__ == "__main__":
    iperf_port = int(sys.argv[1]) if len(sys.argv) > 1 else 5301
    netperf_port = int(sys.argv[2]) if len(sys.argv) > 2 else 12865
    for target, args in [(tcp_server, (iperf_port, iperf_conn)),
                         (tcp_server, (netperf_port, netserver_conn)),
                         (udp_server, (iperf_port, False)),
                         (udp_server, (netperf_port, True))]:
        threading.Thread(target=target, args=args, daemon=True).start()
    print("host peers ready", flush=True)
    threading.Event().wait()
