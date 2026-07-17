#!/usr/bin/env python3
"""Round-trip smoke test for the WebSocket server transport.

Speaks just enough of RFC 6455 (stdlib only: socket, hashlib, base64, os) to
open a ws:// connection, send a ClientHello matching the game's wire format
(protocol v3), and verify the server replies with a ServerWelcome. Proves the
handshake, frame masking, and binary protocol path end-to-end without a
browser.

Usage: python3 tools/ws_smoke.py [host] [port]   (default 127.0.0.1 7778)
Exits 0 on success, 1 on failure.
"""

import base64
import hashlib
import os
import socket
import struct
import sys

PROTOCOL_VERSION = 3
MSG_CLIENT_HELLO = 1
MSG_SERVER_WELCOME = 2
MSG_SERVER_REJECT = 3


def handshake(sock, host, port):
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode())

    response = b""
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("server closed during handshake")
        response += chunk
    if b"101" not in response.split(b"\r\n", 1)[0]:
        raise RuntimeError(f"expected 101, got: {response.splitlines()[0]!r}")

    expected = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
    ).decode()
    if expected.encode() not in response:
        raise RuntimeError("Sec-WebSocket-Accept mismatch")


def send_binary(sock, payload):
    # Client frames must be masked (RFC 6455 5.1).
    header = bytearray([0x82])  # FIN + binary opcode
    n = len(payload)
    if n < 126:
        header.append(0x80 | n)
    elif n <= 0xFFFF:
        header.append(0x80 | 126)
        header += struct.pack(">H", n)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", n)
    mask = os.urandom(4)
    header += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(header) + masked)


def recv_binary(sock, timeout=5.0):
    sock.settimeout(timeout)
    buf = bytearray()

    def read(n):
        while len(buf) < n:
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError("server closed while reading frame")
            buf.extend(chunk)
        out = bytes(buf[:n])
        del buf[:n]
        return out

    b0, b1 = read(2)
    opcode = b0 & 0x0F
    length = b1 & 0x7F
    if length == 126:
        length = struct.unpack(">H", read(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read(8))[0]
    # Server frames are never masked.
    payload = read(length)
    return opcode, payload


def build_client_hello(name):
    out = bytearray()
    out.append(MSG_CLIENT_HELLO)
    out += struct.pack("<H", PROTOCOL_VERSION)
    encoded = name.encode()
    out.append(len(encoded))
    out += encoded
    return bytes(out)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7778

    with socket.create_connection((host, port), timeout=5.0) as sock:
        handshake(sock, host, port)
        print(f"handshake ok -> {host}:{port}")

        send_binary(sock, build_client_hello("smoketest"))
        print("sent ClientHello")

        # The first game message back should be ServerWelcome.
        for _ in range(10):
            opcode, payload = recv_binary(sock)
            if opcode == 0x8:
                raise RuntimeError("server sent close")
            if not payload:
                continue
            msg_type = payload[0]
            if msg_type == MSG_SERVER_WELCOME:
                player_id = payload[1]
                tick_rate = payload[2]
                snapshot_rate = payload[3]
                server_tick = struct.unpack_from("<I", payload, 4)[0]
                print(
                    f"got ServerWelcome: player_id={player_id} "
                    f"tick_rate={tick_rate} snapshot_rate={snapshot_rate} "
                    f"server_tick={server_tick}"
                )
                if tick_rate != 60 or snapshot_rate != 20:
                    raise RuntimeError("unexpected rates in ServerWelcome")
                print("PASS")
                return 0
            if msg_type == MSG_SERVER_REJECT:
                raise RuntimeError(f"server rejected: reason={payload[1]}")
            # Skip anything else (e.g. later broadcasts) and keep looking.
        raise RuntimeError("no ServerWelcome received")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
