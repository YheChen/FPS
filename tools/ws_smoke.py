#!/usr/bin/env python3
"""Round-trip smoke test for the WebSocket server transport.

Speaks just enough of RFC 6455 (stdlib only) to open a ws:// or wss://
connection, send a ClientHello on the game's wire format, and verify the
server replies with a ServerWelcome. Proves the handshake, frame masking, and
binary protocol path end-to-end without a browser -- and, over TLS, proves the
reverse proxy and certificate too.

The protocol version is read from game/shared/protocol.h so it cannot go stale.

Usage: python3 tools/ws_smoke.py [host] [port] [--tls|--no-tls]
       (defaults: 127.0.0.1 7778; TLS is implied by port 443)

Point it at a deployed server to check the whole chain including the TLS
proxy:  python3 tools/ws_smoke.py fps.example.com 443
Exits 0 on success, 1 on failure.
"""

import base64
import hashlib
import os
import re
import socket
import ssl
import struct
import sys
from pathlib import Path

MSG_CLIENT_HELLO = 1
MSG_SERVER_WELCOME = 2
MSG_SERVER_REJECT = 3

REJECT_REASONS = {1: "version mismatch", 2: "server full", 3: "bad name"}


def protocol_version():
    """Reads kProtocolVersion out of the header rather than duplicating it.

    A hardcoded copy goes stale silently and then the tool reports a version
    mismatch that looks like a server fault -- which is exactly what happened
    when the protocol went to 4 and this still said 3. The header is always
    two directories up from tools/; --protocol N overrides for a copy of this
    script living outside the repo.
    """
    for i, arg in enumerate(sys.argv):
        if arg == "--protocol" and i + 1 < len(sys.argv):
            return int(sys.argv[i + 1])
    header = Path(__file__).resolve().parent.parent / "game" / "shared" / "protocol.h"
    try:
        text = header.read_text()
    except OSError as exc:
        raise SystemExit(
            f"cannot read {header} to learn the protocol version ({exc}).\n"
            "Pass --protocol N if you are running this outside the repo."
        ) from exc
    match = re.search(r"kProtocolVersion\s*=\s*(\d+)", text)
    if not match:
        raise SystemExit(f"no kProtocolVersion found in {header}")
    return int(match.group(1))


def handshake(sock, host, port):
    # Default ports are omitted from Host per RFC 9110; some proxies route
    # on an exact Host match and reject "example.com:443".
    host_header = host if port in (80, 443) else f"{host}:{port}"
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host_header}\r\n"
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


def build_client_hello(name, version):
    out = bytearray()
    out.append(MSG_CLIENT_HELLO)
    out += struct.pack("<H", version)
    encoded = name.encode()
    out.append(len(encoded))
    out += encoded
    return bytes(out)


def connect(host, port, use_tls):
    """Opens the transport, wrapping it in TLS for a wss:// endpoint.

    A deployed server sits behind a TLS proxy, so verifying a deployment means
    speaking wss:// -- checking plain ws:// on localhost proves the game
    protocol works but says nothing about the half that usually breaks
    (certificate, proxy, port forward).
    """
    sock = socket.create_connection((host, port), timeout=5.0)
    if not use_tls:
        return sock
    context = ssl.create_default_context()
    # server_hostname drives both SNI -- which the proxy needs to pick a
    # certificate -- and hostname verification.
    try:
        return context.wrap_socket(sock, server_hostname=host)
    except ssl.SSLCertVerificationError as exc:
        raise RuntimeError(
            f"TLS certificate for {host} did not verify: {exc.verify_message or exc}.\n"
            "  Caddy may still be provisioning, or the certificate covers a "
            "different name than the one you connected to."
        ) from exc
    except (ssl.SSLError, socket.timeout, TimeoutError) as exc:
        raise RuntimeError(
            f"TLS handshake with {host}:{port} failed: {exc}.\n"
            f"  Is anything terminating TLS on {port}? A plain ws:// server "
            "there will hang exactly like this -- drop --tls to test it directly."
        ) from exc


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}

    host = args[0] if args else "127.0.0.1"
    port = int(args[1]) if len(args) > 1 else 7778
    # 443 means wss:// in practice; --tls/--no-tls override for odd ports.
    use_tls = ("--tls" in flags) or (port == 443 and "--no-tls" not in flags)

    version = protocol_version()

    with connect(host, port, use_tls) as sock:
        scheme = "wss" if use_tls else "ws"
        handshake(sock, host, port)
        print(f"handshake ok -> {scheme}://{host}:{port}")

        send_binary(sock, build_client_hello("smoketest", version))
        print(f"sent ClientHello (protocol v{version})")

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
                reason = payload[1]
                name = REJECT_REASONS.get(reason, f"unknown ({reason})")
                raise RuntimeError(f"server rejected the connection: {name}")
            # Skip anything else (e.g. later broadcasts) and keep looking.
        raise RuntimeError("no ServerWelcome received")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
