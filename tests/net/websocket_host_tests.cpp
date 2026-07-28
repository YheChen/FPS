#include "engine/net/websocket_host.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Same shim as websocket_host.cpp: the client half of these tests is a raw
// socket, because there is no native WebSocket client in the engine (the
// browser provides one, and that path is Emscripten-only). Speaking RFC 6455
// by hand is the point anyway -- a well-behaved client library would refuse to
// send the frames being tested.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define ENG_CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define ENG_CLOSESOCK ::close
#endif

namespace {

// WebSocketHost::create_server does the WSAStartup on Windows, so a raw client
// socket is only ever created after a host exists.
class RawClient {
public:
    explicit RawClient(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == INVALID_SOCKET) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ENG_CLOSESOCK(fd_);
            fd_ = INVALID_SOCKET;
        }
    }

    ~RawClient() {
        if (fd_ != INVALID_SOCKET) {
            ENG_CLOSESOCK(fd_);
        }
    }

    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    bool valid() const { return fd_ != INVALID_SOCKET; }

    bool send_all(const std::vector<std::uint8_t>& bytes) const {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
#if defined(_WIN32)
            const int n = ::send(fd_, reinterpret_cast<const char*>(bytes.data() + sent),
                                 static_cast<int>(bytes.size() - sent), 0);
#else
            const auto n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, 0);
#endif
            if (n <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool send_text(const std::string& text) const { return send_all({text.begin(), text.end()}); }

private:
    socket_t fd_ = INVALID_SOCKET;
};

// A key the server will accept; the reply hash is not checked here because
// these tests are about what happens after the upgrade.
const std::string kHandshake =
    "GET / HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n";

// A masked client frame whose 64-bit length field says `declared` while
// carrying `payload_bytes` actual bytes. Splitting the two is the whole
// exploit: a peer says "16 exabytes" and sends nine bytes.
std::vector<std::uint8_t> masked_frame_with_declared_length(std::uint64_t declared,
                                                            std::size_t payload_bytes) {
    std::vector<std::uint8_t> frame;
    frame.push_back(0x82);          // FIN + binary
    frame.push_back(0x80 | 127);    // masked + 64-bit length follows
    for (int i = 7; i >= 0; --i) {  // network byte order
        frame.push_back(static_cast<std::uint8_t>((declared >> (i * 8)) & 0xff));
    }
    const std::array<std::uint8_t, 4> mask = {0xAA, 0xBB, 0xCC, 0xDD};
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (std::size_t i = 0; i < payload_bytes; ++i) {
        frame.push_back(static_cast<std::uint8_t>(0x00 ^ mask[i % 4]));
    }
    return frame;
}

// Pumps the host until it reports the peer gone, or gives up.
bool poll_until_disconnected(eng::WebSocketHost& host) {
    std::vector<eng::NetEvent> events;
    for (int i = 0; i < 400; ++i) {
        events.clear();
        host.poll(events);
        for (const eng::NetEvent& event : events) {
            if (event.type == eng::NetEvent::Type::Disconnected) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// A 64-bit length is entirely peer-controlled, and every use of it is a hazard:
// as a vector size it is an allocation request, as an index it is a read, and
// `header + len` wraps for values near the top of the range -- which would let
// a nine-byte buffer satisfy a "frame is complete" check. It also has to narrow
// to size_t, which is 32 bits on wasm32.
TEST_CASE("an over-long declared frame length closes the peer", "[websocket]") {
    constexpr std::uint16_t kPort = 47811;
    auto host = eng::WebSocketHost::create_server(kPort, 4);
    REQUIRE(host.has_value());

    RawClient client{kPort};
    REQUIRE(client.valid());
    REQUIRE(client.send_text(kHandshake));

    // Let the upgrade complete before the hostile frame arrives.
    std::vector<eng::NetEvent> events;
    bool connected = false;
    for (int i = 0; i < 400 && !connected; ++i) {
        events.clear();
        host->poll(events);
        for (const eng::NetEvent& event : events) {
            if (event.type == eng::NetEvent::Type::Connected) {
                connected = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(connected);
    REQUIRE(host->peer_count() == 1);

    REQUIRE(client.send_all(masked_frame_with_declared_length(0xFFFFFFFFFFFFFFFFull, 9)));

    CHECK(poll_until_disconnected(*host));
    CHECK(host->peer_count() == 0);
}

// Bounding one frame is not enough on its own: FIN is the peer's to set, so a
// stream of individually-legal continuations can grow the reassembly buffer
// until the process dies.
TEST_CASE("an unterminated message is cut off before it exhausts memory", "[websocket]") {
    constexpr std::uint16_t kPort = 47812;
    auto host = eng::WebSocketHost::create_server(kPort, 4);
    REQUIRE(host.has_value());

    RawClient client{kPort};
    REQUIRE(client.valid());
    REQUIRE(client.send_text(kHandshake));

    std::vector<eng::NetEvent> events;
    bool connected = false;
    for (int i = 0; i < 400 && !connected; ++i) {
        events.clear();
        host->poll(events);
        for (const eng::NetEvent& event : events) {
            if (event.type == eng::NetEvent::Type::Connected) {
                connected = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(connected);

    // 16 KiB per frame, never setting FIN. The cap is 256 KiB, so this runs
    // past it with room to spare while every individual frame stays legal.
    constexpr std::size_t kChunk = 16u * 1024u;
    bool disconnected = false;
    for (int frame = 0; frame < 64 && !disconnected; ++frame) {
        std::vector<std::uint8_t> bytes;
        // Continuation opcode 0x0 with no FIN bit, except the first which
        // opens the message as binary.
        bytes.push_back(frame == 0 ? 0x02 : 0x00);
        bytes.push_back(0x80 | 126);  // masked + 16-bit length
        bytes.push_back(static_cast<std::uint8_t>((kChunk >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(kChunk & 0xff));
        const std::array<std::uint8_t, 4> mask = {0x01, 0x02, 0x03, 0x04};
        bytes.insert(bytes.end(), mask.begin(), mask.end());
        for (std::size_t i = 0; i < kChunk; ++i) {
            bytes.push_back(mask[i % 4]);  // plaintext zeroes
        }
        if (!client.send_all(bytes)) {
            disconnected = true;  // the host hung up mid-write, which is the point
            break;
        }
        events.clear();
        host->poll(events);
        for (const eng::NetEvent& event : events) {
            if (event.type == eng::NetEvent::Type::Disconnected) {
                disconnected = true;
            }
        }
    }

    CHECK((disconnected || poll_until_disconnected(*host)));
    CHECK(host->peer_count() == 0);
}

}  // namespace
