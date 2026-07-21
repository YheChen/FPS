#include "engine/net/client_transport.h"

#include <utility>

#include "engine/core/log.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/websocket.h>

#include <string>
#else
#include "engine/net/transport.h"
#endif

namespace eng {

#if defined(__EMSCRIPTEN__)

namespace {

// Builds a ws:// URL from a host[:port], leaving an explicit ws://sch alone.
std::string to_ws_url(const std::string& host, std::uint16_t port) {
    if (host.rfind("ws://", 0) == 0 || host.rfind("wss://", 0) == 0) {
        return host;
    }
    return "ws://" + host + ":" + std::to_string(port);
}

}  // namespace

// Browser WebSocket transport. Callbacks (open/message/close/error) run on
// the main thread between frames; they push into queues that poll() drains.
class WebSocketClientTransport final : public IClientTransport {
public:
    explicit WebSocketClientTransport(const std::string& url) {
        if (!emscripten_websocket_is_supported()) {
            log::error("WebSockets are not supported in this browser");
            failed_ = true;
            return;
        }
        EmscriptenWebSocketCreateAttributes attr;
        emscripten_websocket_init_create_attributes(&attr);
        attr.url = url.c_str();
        socket_ = emscripten_websocket_new(&attr);
        if (socket_ <= 0) {
            log::error("emscripten_websocket_new failed for '{}'", url);
            failed_ = true;
            return;
        }
        emscripten_websocket_set_onopen_callback(socket_, this, on_open);
        emscripten_websocket_set_onmessage_callback(socket_, this, on_message);
        emscripten_websocket_set_onerror_callback(socket_, this, on_error);
        emscripten_websocket_set_onclose_callback(socket_, this, on_close);
        log::info("WebSocket connecting to {}", url);
    }

    ~WebSocketClientTransport() override {
        if (socket_ > 0) {
            emscripten_websocket_close(socket_, 1000, "bye");
            emscripten_websocket_delete(socket_);
        }
    }

    WebSocketClientTransport(const WebSocketClientTransport&) = delete;
    WebSocketClientTransport& operator=(const WebSocketClientTransport&) = delete;

    void poll(std::vector<NetEvent>& out) override {
        for (NetEvent& event : queue_) {
            out.push_back(std::move(event));
        }
        queue_.clear();
    }

    void send(std::span<const std::uint8_t> data, NetChannel, bool) override {
        if (socket_ > 0 && open_) {
            emscripten_websocket_send_binary(
                socket_, const_cast<void*>(static_cast<const void*>(data.data())),
                static_cast<std::uint32_t>(data.size()));
            stats_.bytes_sent += data.size();
            ++stats_.packets_sent;
        }
    }

    void disconnect() override {
        if (socket_ > 0) {
            emscripten_websocket_close(socket_, 1000, "bye");
        }
    }

    std::uint32_t rtt_ms() const override { return 0; }
    const NetStats& stats() const override { return stats_; }

    bool failed() const { return failed_; }

private:
    static EM_BOOL on_open(int, const EmscriptenWebSocketOpenEvent*, void* user) {
        auto* self = static_cast<WebSocketClientTransport*>(user);
        self->open_ = true;
        self->queue_.push_back({NetEvent::Type::Connected, 0, NetChannel::Reliable, {}});
        return EM_TRUE;
    }
    static EM_BOOL on_message(int, const EmscriptenWebSocketMessageEvent* e, void* user) {
        auto* self = static_cast<WebSocketClientTransport*>(user);
        if (!e->isText && e->numBytes > 0) {
            NetEvent event;
            event.type = NetEvent::Type::Message;
            event.channel = NetChannel::Reliable;
            event.data.assign(e->data, e->data + e->numBytes);
            self->stats_.bytes_received += e->numBytes;
            ++self->stats_.packets_received;
            self->queue_.push_back(std::move(event));
        }
        return EM_TRUE;
    }
    static EM_BOOL on_error(int, const EmscriptenWebSocketErrorEvent*, void* user) {
        static_cast<WebSocketClientTransport*>(user)->failed_ = true;
        return EM_TRUE;
    }
    static EM_BOOL on_close(int, const EmscriptenWebSocketCloseEvent*, void* user) {
        auto* self = static_cast<WebSocketClientTransport*>(user);
        self->open_ = false;
        self->queue_.push_back({NetEvent::Type::Disconnected, 0, NetChannel::Reliable, {}});
        return EM_TRUE;
    }

    EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
    bool open_ = false;
    bool failed_ = false;
    std::vector<NetEvent> queue_;
    NetStats stats_;
};

std::unique_ptr<IClientTransport> make_client_transport(const std::string& host,
                                                        std::uint16_t port) {
    auto transport = std::make_unique<WebSocketClientTransport>(to_ws_url(host, port));
    if (transport->failed()) {
        return nullptr;
    }
    return transport;
}

#else  // native: ENet

// Wraps the ENet NetHost as a single-server client transport.
class EnetClientTransport final : public IClientTransport {
public:
    static std::unique_ptr<EnetClientTransport> create(const std::string& host,
                                                       std::uint16_t port) {
        auto net = NetHost::create_client();
        if (!net) {
            return nullptr;
        }
        const auto peer = net->connect(host, port);
        if (!peer) {
            return nullptr;
        }
        return std::unique_ptr<EnetClientTransport>(
            new EnetClientTransport(std::move(*net), *peer));
    }

    void poll(std::vector<NetEvent>& out) override { net_.poll(out); }
    void send(std::span<const std::uint8_t> data, NetChannel channel, bool reliable) override {
        net_.send(server_peer_, data, channel, reliable);
    }
    void disconnect() override { net_.disconnect(server_peer_); }
    std::uint32_t rtt_ms() const override { return net_.rtt_ms(server_peer_); }
    const NetStats& stats() const override { return net_.stats(); }
    void set_simulation(const NetSimConfig& config) override { net_.set_simulation(config); }

private:
    EnetClientTransport(NetHost net, std::uint32_t peer)
        : net_(std::move(net)), server_peer_(peer) {}

    NetHost net_;
    std::uint32_t server_peer_;
};

std::unique_ptr<IClientTransport> make_client_transport(const std::string& host,
                                                        std::uint16_t port) {
    return EnetClientTransport::create(host, port);
}

#endif

}  // namespace eng
