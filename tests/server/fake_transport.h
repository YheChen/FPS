#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine/net/byte_buffer.h"
#include "engine/net/transport.h"
#include "game/shared/protocol.h"

// An IServerTransport with no sockets behind it: inbound events are scripted
// by the test and drained by poll(), and every outbound send/broadcast is
// recorded in full -- peer, channel, reliability and bytes.
//
// Recording the bytes rather than a summary is the point. ServerGame's
// contract is "what did the client actually receive", and most of the rules
// worth testing (who is told about a kill, what a rejected client is told,
// what is never sent to a bot) are only visible in the addressing and the
// payload together.
namespace test {

struct Sent {
    std::uint32_t peer = 0;
    // True when it went out via broadcast() rather than send(). ServerGame
    // never uses broadcast() -- it fans out with per-peer send() so bots,
    // which have no peer, can be skipped -- so this staying false is itself
    // an assertion worth making.
    bool broadcast = false;
    eng::NetChannel channel = eng::NetChannel::Reliable;
    bool reliable = true;
    std::vector<std::uint8_t> data;

    std::optional<game::MessageType> type() const {
        eng::ByteReader reader{{data.data(), data.size()}};
        return game::read_message_type(reader);
    }
};

class FakeTransport final : public eng::IServerTransport {
public:
    // --- inbound: what the "network" hands the server on the next poll ---
    void queue(eng::NetEvent event) { inbound_.push_back(std::move(event)); }

    void queue_connect(std::uint32_t peer) {
        queue({eng::NetEvent::Type::Connected, peer, eng::NetChannel::Reliable, {}});
    }
    void queue_disconnect(std::uint32_t peer) {
        queue({eng::NetEvent::Type::Disconnected, peer, eng::NetChannel::Reliable, {}});
    }
    void queue_message(std::uint32_t peer, std::vector<std::uint8_t> data,
                       eng::NetChannel channel = eng::NetChannel::Reliable) {
        queue({eng::NetEvent::Type::Message, peer, channel, std::move(data)});
    }

    // --- IServerTransport ------------------------------------------------
    void poll(std::vector<eng::NetEvent>& out) override {
        for (auto& event : inbound_) {
            out.push_back(std::move(event));
        }
        inbound_.clear();
    }

    void send(std::uint32_t peer, std::span<const std::uint8_t> data, eng::NetChannel channel,
              bool reliable) override {
        sent.push_back(Sent{peer, false, channel, reliable, {data.begin(), data.end()}});
    }

    void broadcast(std::span<const std::uint8_t> data, eng::NetChannel channel,
                   bool reliable) override {
        sent.push_back(Sent{0, true, channel, reliable, {data.begin(), data.end()}});
    }

    void disconnect(std::uint32_t peer) override { disconnected.push_back(peer); }

    std::size_t peer_count() const override { return peer_count_value; }
    const eng::NetStats& stats() const override { return stats_; }

    // --- recorded output --------------------------------------------------
    std::vector<Sent> sent;
    std::vector<std::uint32_t> disconnected;
    std::size_t peer_count_value = 0;

    // Forgets everything sent so far. Lets a test set up a situation and then
    // assert only on what the step under test produced.
    void clear() {
        sent.clear();
        disconnected.clear();
    }

    std::size_t count(game::MessageType type) const {
        std::size_t total = 0;
        for (const Sent& message : sent) {
            if (message.type() == type) {
                ++total;
            }
        }
        return total;
    }

    std::size_t count_to(std::uint32_t peer, game::MessageType type) const {
        std::size_t total = 0;
        for (const Sent& message : sent) {
            if (!message.broadcast && message.peer == peer && message.type() == type) {
                ++total;
            }
        }
        return total;
    }

    bool anything_sent_to(std::uint32_t peer) const {
        for (const Sent& message : sent) {
            if (!message.broadcast && message.peer == peer) {
                return true;
            }
        }
        return false;
    }

    bool used_broadcast() const {
        for (const Sent& message : sent) {
            if (message.broadcast) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<eng::NetEvent> inbound_;
    eng::NetStats stats_;
};

// Decodes every recorded message of `type`, oldest first, using the matching
// protocol reader (e.g. `game::read_player_died`). Messages that fail to
// decode are dropped rather than silently mangled -- the server is supposed
// to emit well-formed messages, so an empty result is a real failure signal.
template <typename Read>
auto decode_all(const std::vector<Sent>& sent, game::MessageType type, Read read) {
    using Message = typename std::invoke_result_t<Read, eng::ByteReader&>::value_type;
    std::vector<Message> out;
    for (const Sent& message : sent) {
        eng::ByteReader reader{{message.data.data(), message.data.size()}};
        const auto actual = game::read_message_type(reader);
        if (!actual || *actual != type) {
            continue;
        }
        if (auto decoded = read(reader)) {
            out.push_back(std::move(*decoded));
        }
    }
    return out;
}

// The same, restricted to what one peer received by unicast.
template <typename Read>
auto decode_to(const std::vector<Sent>& sent, std::uint32_t peer, game::MessageType type,
               Read read) {
    std::vector<Sent> mine;
    for (const Sent& message : sent) {
        if (!message.broadcast && message.peer == peer) {
            mine.push_back(message);
        }
    }
    return decode_all(mine, type, read);
}

// --- client -> server encoders ---------------------------------------------
//
// game::write(ClientHello) always stamps the current protocol version and a
// name the writer already believes is legal, so it cannot express the two
// hellos the server most needs to refuse. These build the bytes directly.

inline std::vector<std::uint8_t> hello_bytes(std::uint16_t version, std::string_view name) {
    eng::ByteWriter writer;
    writer.u8(static_cast<std::uint8_t>(game::MessageType::ClientHello));
    writer.u16(version);
    writer.str(name);
    return {writer.data().begin(), writer.data().end()};
}

inline std::vector<std::uint8_t> input_bytes(std::uint32_t sequence,
                                             const game::InputCommand& command,
                                             std::uint32_t view_tick = 0) {
    game::InputPacket packet;
    packet.newest_sequence = sequence;
    packet.client_tick = sequence;
    packet.view_tick = view_tick;
    packet.commands.push_back(command);
    eng::ByteWriter writer;
    game::write(writer, packet);
    return {writer.data().begin(), writer.data().end()};
}

}  // namespace test
