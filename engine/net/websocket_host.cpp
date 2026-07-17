#include "engine/net/websocket_host.h"

#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include "engine/core/log.h"

// --- cross-platform socket shim --------------------------------------------
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using ws_len_t = int;  // Winsock send/recv take int lengths
#define ENG_CLOSESOCK closesocket
#define ENG_WOULDBLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
using ws_len_t = std::size_t;  // POSIX send/recv take size_t lengths
#define INVALID_SOCKET (-1)
#define ENG_CLOSESOCK ::close
#define ENG_WOULDBLOCK (errno == EWOULDBLOCK || errno == EAGAIN)
#endif

namespace eng {

namespace {

// --- SHA-1 (for the Sec-WebSocket-Accept handshake only) -------------------
std::array<std::uint8_t, 20> sha1(const std::string& input) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const auto rol = [](std::uint32_t v, int b) {
        return static_cast<std::uint32_t>((v << b) | (v >> (32 - b)));
    };

    std::string msg = input;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(msg.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) {
        msg.push_back('\0');
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xff));
    }

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(msg.data() + chunk + i * 4);
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) |
                   (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = tmp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>((h[i] >> 24) & 0xff);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xff);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xff);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xff);
    }
    return out;
}

std::string base64(std::span<const std::uint8_t> data) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kTable[(n >> 18) & 0x3f]);
        out.push_back(kTable[(n >> 12) & 0x3f]);
        out.push_back(kTable[(n >> 6) & 0x3f]);
        out.push_back(kTable[n & 0x3f]);
        i += 3;
    }
    if (i < data.size()) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        const bool two = (i + 1 < data.size());
        if (two) {
            n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        }
        out.push_back(kTable[(n >> 18) & 0x3f]);
        out.push_back(kTable[(n >> 12) & 0x3f]);
        out.push_back(two ? kTable[(n >> 6) & 0x3f] : '=');
        out.push_back('=');
    }
    return out;
}

std::string case_insensitive_find_header(const std::string& request, const std::string& key) {
    // key is lowercase; scan line by line.
    std::size_t pos = 0;
    while (pos < request.size()) {
        const std::size_t eol = request.find("\r\n", pos);
        const std::string line = request.substr(pos, eol - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (char& ch : name) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (name == key) {
                std::string value = line.substr(colon + 1);
                std::size_t start = value.find_first_not_of(" \t");
                std::size_t end = value.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    return value.substr(start, end - start + 1);
                }
            }
        }
        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 2;
    }
    return {};
}

void set_nonblocking(socket_t fd) {
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

void set_nodelay(socket_t fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

constexpr std::uint8_t kOpText = 0x1;
constexpr std::uint8_t kOpBinary = 0x2;
constexpr std::uint8_t kOpClose = 0x8;
constexpr std::uint8_t kOpPing = 0x9;
constexpr std::uint8_t kOpPong = 0xA;

// Appends a server->client frame (never masked) with the given opcode.
void append_frame(std::vector<std::uint8_t>& out, std::uint8_t opcode,
                  std::span<const std::uint8_t> payload) {
    out.push_back(static_cast<std::uint8_t>(0x80 | opcode));  // FIN + opcode
    const std::size_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<std::uint8_t>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(n & 0xff));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) {
            out.push_back(
                static_cast<std::uint8_t>((static_cast<std::uint64_t>(n) >> (i * 8)) & 0xff));
        }
    }
    out.insert(out.end(), payload.begin(), payload.end());
}

}  // namespace

struct WebSocketHost::Impl {
    socket_t listener = INVALID_SOCKET;
    std::size_t max_peers = 8;
    std::uint32_t next_peer_id = 1;
    NetStats stats;

    struct Connection {
        socket_t fd = INVALID_SOCKET;
        bool handshaked = false;
        bool closing = false;
        std::vector<std::uint8_t> in;   // raw bytes received, not yet parsed
        std::vector<std::uint8_t> out;  // framed bytes pending send
        // Reassembly across continuation frames.
        std::vector<std::uint8_t> message;
        std::uint8_t message_opcode = 0;
    };
    std::unordered_map<std::uint32_t, Connection> peers;

    ~Impl() {
        for (auto& [id, conn] : peers) {
            if (conn.fd != INVALID_SOCKET) {
                ENG_CLOSESOCK(conn.fd);
            }
        }
        if (listener != INVALID_SOCKET) {
            ENG_CLOSESOCK(listener);
        }
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    void queue_close(Connection& conn) {
        if (!conn.closing) {
            append_frame(conn.out, kOpClose, {});
            conn.closing = true;
        }
    }

    // Tries to flush a connection's outgoing buffer; keeps the remainder.
    void flush(Connection& conn) {
        std::size_t sent = 0;
        while (sent < conn.out.size()) {
            const auto n = ::send(conn.fd, reinterpret_cast<const char*>(conn.out.data() + sent),
                                  static_cast<ws_len_t>(conn.out.size() - sent), 0);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                stats.bytes_sent += static_cast<std::uint64_t>(n);
            } else {
                break;  // EWOULDBLOCK or error; retry next poll
            }
        }
        conn.out.erase(conn.out.begin(), conn.out.begin() + static_cast<std::ptrdiff_t>(sent));
    }
};

WebSocketHost::WebSocketHost() : impl_(std::make_unique<Impl>()) {}
WebSocketHost::~WebSocketHost() = default;
WebSocketHost::WebSocketHost(WebSocketHost&&) noexcept = default;
WebSocketHost& WebSocketHost::operator=(WebSocketHost&&) noexcept = default;

std::optional<WebSocketHost> WebSocketHost::create_server(std::uint16_t port,
                                                          std::size_t max_peers) {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log::error("WSAStartup failed");
        return std::nullopt;
    }
#endif
    WebSocketHost host;
    host.impl_->max_peers = max_peers;
    host.impl_->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (host.impl_->listener == INVALID_SOCKET) {
        log::error("WebSocket: socket() failed");
        return std::nullopt;
    }
    int one = 1;
    setsockopt(host.impl_->listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(host.impl_->listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log::error("WebSocket: bind to port {} failed (in use?)", port);
        return std::nullopt;
    }
    if (listen(host.impl_->listener, 16) != 0) {
        log::error("WebSocket: listen() failed");
        return std::nullopt;
    }
    set_nonblocking(host.impl_->listener);
    log::info("WebSocket server listening on ws://0.0.0.0:{}", port);
    return host;
}

void WebSocketHost::poll(std::vector<NetEvent>& out) {
    // Accept new connections.
    while (true) {
        const socket_t fd = accept(impl_->listener, nullptr, nullptr);
        if (fd == INVALID_SOCKET) {
            break;
        }
        if (impl_->peers.size() >= impl_->max_peers) {
            ENG_CLOSESOCK(fd);
            continue;
        }
        set_nonblocking(fd);
        set_nodelay(fd);
        const std::uint32_t id = impl_->next_peer_id++;
        impl_->peers[id].fd = fd;
    }

    std::vector<std::uint32_t> dead;
    for (auto& [id, conn] : impl_->peers) {
        // Read whatever is available.
        std::array<std::uint8_t, 4096> buffer;
        bool closed = false;
        while (true) {
            const auto n = recv(conn.fd, reinterpret_cast<char*>(buffer.data()),
                                static_cast<ws_len_t>(buffer.size()), 0);
            if (n > 0) {
                conn.in.insert(conn.in.end(), buffer.begin(),
                               buffer.begin() + static_cast<std::ptrdiff_t>(n));
                impl_->stats.bytes_received += static_cast<std::uint64_t>(n);
            } else if (n == 0) {
                closed = true;  // peer closed
                break;
            } else {
                if (!ENG_WOULDBLOCK) {
                    closed = true;
                }
                break;
            }
        }

        // Handshake once we have the full HTTP header.
        if (!conn.handshaked && !closed) {
            const std::string request(conn.in.begin(), conn.in.end());
            const std::size_t header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const std::string key = case_insensitive_find_header(request, "sec-websocket-key");
                if (key.empty()) {
                    log::warn("WebSocket peer {}: missing Sec-WebSocket-Key", id);
                    closed = true;
                } else {
                    const std::string accept_src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
                    const std::string accept = base64(sha1(accept_src));
                    const std::string response =
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " +
                        accept + "\r\n\r\n";
                    conn.out.insert(conn.out.end(), response.begin(), response.end());
                    conn.in.erase(conn.in.begin(),
                                  conn.in.begin() + static_cast<std::ptrdiff_t>(header_end + 4));
                    conn.handshaked = true;
                    out.push_back({NetEvent::Type::Connected, id, NetChannel::Reliable, {}});
                }
            }
        }

        // Parse complete frames.
        while (conn.handshaked && !closed) {
            if (conn.in.size() < 2) {
                break;
            }
            const std::uint8_t b0 = conn.in[0];
            const std::uint8_t b1 = conn.in[1];
            const bool fin = (b0 & 0x80) != 0;
            const std::uint8_t opcode = b0 & 0x0f;
            const bool masked = (b1 & 0x80) != 0;
            std::uint64_t len = b1 & 0x7f;
            std::size_t header = 2;
            if (len == 126) {
                if (conn.in.size() < 4) {
                    break;
                }
                len = (static_cast<std::uint64_t>(conn.in[2]) << 8) | conn.in[3];
                header = 4;
            } else if (len == 127) {
                if (conn.in.size() < 10) {
                    break;
                }
                len = 0;
                for (int i = 0; i < 8; ++i) {
                    len = (len << 8) | conn.in[2 + static_cast<std::size_t>(i)];
                }
                header = 10;
            }
            // Client frames MUST be masked (RFC 6455 5.1).
            if (!masked) {
                log::warn("WebSocket peer {}: unmasked client frame, closing", id);
                closed = true;
                break;
            }
            const std::size_t mask_off = header;
            const std::size_t data_off = header + 4;
            if (conn.in.size() < data_off + len) {
                break;  // frame incomplete; wait for more bytes
            }
            std::array<std::uint8_t, 4> mask = {conn.in[mask_off], conn.in[mask_off + 1],
                                                conn.in[mask_off + 2], conn.in[mask_off + 3]};
            std::vector<std::uint8_t> payload(len);
            for (std::uint64_t i = 0; i < len; ++i) {
                payload[i] = static_cast<std::uint8_t>(conn.in[data_off + i] ^ mask[i % 4]);
            }
            conn.in.erase(conn.in.begin(),
                          conn.in.begin() + static_cast<std::ptrdiff_t>(data_off + len));

            if (opcode == kOpClose) {
                impl_->queue_close(conn);
                closed = true;
                break;
            }
            if (opcode == kOpPing) {
                append_frame(conn.out, kOpPong, payload);
                continue;
            }
            if (opcode == kOpPong) {
                continue;
            }
            // Data frame (binary/text or continuation). Reassemble.
            if (opcode == kOpBinary || opcode == kOpText) {
                conn.message = std::move(payload);
                conn.message_opcode = opcode;
            } else {  // continuation
                conn.message.insert(conn.message.end(), payload.begin(), payload.end());
            }
            if (fin) {
                ++impl_->stats.packets_received;
                out.push_back(
                    {NetEvent::Type::Message, id, NetChannel::Reliable, std::move(conn.message)});
                conn.message.clear();
            }
        }

        impl_->flush(conn);
        if (closed) {
            dead.push_back(id);
        }
    }

    for (const std::uint32_t id : dead) {
        auto it = impl_->peers.find(id);
        if (it != impl_->peers.end()) {
            ENG_CLOSESOCK(it->second.fd);
            impl_->peers.erase(it);
        }
        out.push_back({NetEvent::Type::Disconnected, id, NetChannel::Reliable, {}});
    }
}

void WebSocketHost::send(std::uint32_t peer, std::span<const std::uint8_t> data, NetChannel, bool) {
    const auto it = impl_->peers.find(peer);
    if (it == impl_->peers.end() || !it->second.handshaked || it->second.closing) {
        return;
    }
    append_frame(it->second.out, kOpBinary, data);
    ++impl_->stats.packets_sent;
    impl_->flush(it->second);
}

void WebSocketHost::broadcast(std::span<const std::uint8_t> data, NetChannel channel,
                              bool reliable) {
    for (auto& [id, conn] : impl_->peers) {
        send(id, data, channel, reliable);
    }
}

void WebSocketHost::disconnect(std::uint32_t peer) {
    const auto it = impl_->peers.find(peer);
    if (it != impl_->peers.end()) {
        impl_->queue_close(it->second);
        impl_->flush(it->second);
    }
}

std::size_t WebSocketHost::peer_count() const {
    return impl_->peers.size();
}

const NetStats& WebSocketHost::stats() const {
    return impl_->stats;
}

}  // namespace eng
