#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/net/transport.h"

// Client-side transport seam. The game client talks to exactly one server,
// so there is no peer id in the API. Two implementations select at build
// time: ENet/UDP on native, WebSocket on Emscripten (browsers cannot open
// UDP). See make_client_transport(). Main thread only.
namespace eng {

class IClientTransport {
public:
    virtual ~IClientTransport() = default;

    // Pumps events. Emits exactly one Connected (or Disconnected on failure)
    // for the server, then Message events; NetEvent::peer is unused.
    virtual void poll(std::vector<NetEvent>& out) = 0;

    // Sends one message to the server.
    virtual void send(std::span<const std::uint8_t> data, NetChannel channel, bool reliable) = 0;

    virtual void disconnect() = 0;

    // Round-trip estimate in ms (0 if the transport can't measure it).
    virtual std::uint32_t rtt_ms() const = 0;
    virtual const NetStats& stats() const = 0;

    // Artificial latency/jitter/loss for testing; no-op where unsupported.
    virtual void set_simulation(const NetSimConfig&) {}
};

// Creates the platform's client transport and begins connecting.
//   Native:     ENet to `host`:`port`.
//   Emscripten: WebSocket. If `host` already carries a scheme
//               (ws:// or wss://) it is used verbatim; otherwise the URL is
//               built as ws://`host`:`port`.
// Returns nullptr on immediate failure (e.g. host could not be resolved).
std::unique_ptr<IClientTransport> make_client_transport(const std::string& host,
                                                        std::uint16_t port);

}  // namespace eng
