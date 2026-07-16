#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Thin ENet wrapper. Two channels (docs/packet-format.md):
//   Channel 0 (Reliable)   - reliable, ordered: handshake, events
//   Channel 1 (Sequenced)  - unreliable, sequenced: inputs, snapshots
// Peers are referenced by stable uint32 ids; ENet types never leak out.
// Main thread only.
namespace eng {

enum class NetChannel : std::uint8_t {
    Reliable = 0,
    Sequenced = 1,
};

struct NetEvent {
    enum class Type : std::uint8_t {
        Connected,
        Disconnected,
        Message,
    };
    Type type = Type::Message;
    std::uint32_t peer = 0;
    NetChannel channel = NetChannel::Reliable;
    std::vector<std::uint8_t> data;  // only for Message
};

struct NetStats {
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;
};

// Artificial network conditions for testing (applied per direction, i.e.
// latency_ms is ONE-WAY). Loss applies only to the unreliable Sequenced
// channel - dropping "reliable" sends above ENet would break semantics that
// real loss would not (ENet retransmits under the hood).
struct NetSimConfig {
    int latency_ms = 0;
    int jitter_ms = 0;        // uniform extra [0, jitter_ms]
    float loss_percent = 0.0f;

    bool enabled() const {
        return latency_ms > 0 || jitter_ms > 0 || loss_percent > 0.0f;
    }
};

class NetHost {
public:
    static std::optional<NetHost> create_server(std::uint16_t port, std::size_t max_peers);
    static std::optional<NetHost> create_client();

    ~NetHost();
    NetHost(NetHost&&) noexcept;
    NetHost& operator=(NetHost&&) noexcept;
    NetHost(const NetHost&) = delete;
    NetHost& operator=(const NetHost&) = delete;

    // Client only: begins connecting; the Connected event arrives via poll().
    // Returns the peer id the connection will use.
    std::optional<std::uint32_t> connect(const std::string& host, std::uint16_t port);

    // Pumps all pending events (non-blocking).
    void poll(std::vector<NetEvent>& out);

    void send(std::uint32_t peer, std::span<const std::uint8_t> data, NetChannel channel,
              bool reliable);
    void broadcast(std::span<const std::uint8_t> data, NetChannel channel, bool reliable);

    // Graceful disconnect; the Disconnected event arrives via poll().
    void disconnect(std::uint32_t peer);

    // Round-trip time estimate for a peer, milliseconds (0 if unknown).
    std::uint32_t rtt_ms(std::uint32_t peer) const;

    std::size_t peer_count() const;
    const NetStats& stats() const;

    // Enable/adjust simulated latency/jitter/loss (deterministic RNG).
    void set_simulation(const NetSimConfig& config);
    const NetSimConfig& simulation() const;

private:
    NetHost();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eng
