#include "engine/net/composite_transport.h"

#include <utility>

#include "engine/core/assert.h"

namespace eng {

CompositeTransport::CompositeTransport(std::vector<std::unique_ptr<IServerTransport>> children)
    : children_(std::move(children)) {
    ENG_ASSERT(!children_.empty(), "CompositeTransport needs at least one child");
    ENG_ASSERT(children_.size() <= 256, "child index must fit in 8 bits");
}

void CompositeTransport::poll(std::vector<NetEvent>& out) {
    for (std::size_t c = 0; c < children_.size(); ++c) {
        scratch_.clear();
        children_[c]->poll(scratch_);
        for (NetEvent& event : scratch_) {
            event.peer = make_global(c, event.peer);
            out.push_back(std::move(event));
        }
    }
}

void CompositeTransport::send(std::uint32_t peer, std::span<const std::uint8_t> data,
                              NetChannel channel, bool reliable) {
    const std::size_t c = child_of(peer);
    if (c < children_.size()) {
        children_[c]->send(local_of(peer), data, channel, reliable);
    }
}

void CompositeTransport::broadcast(std::span<const std::uint8_t> data, NetChannel channel,
                                   bool reliable) {
    for (auto& child : children_) {
        child->broadcast(data, channel, reliable);
    }
}

void CompositeTransport::disconnect(std::uint32_t peer) {
    const std::size_t c = child_of(peer);
    if (c < children_.size()) {
        children_[c]->disconnect(local_of(peer));
    }
}

std::size_t CompositeTransport::peer_count() const {
    std::size_t total = 0;
    for (const auto& child : children_) {
        total += child->peer_count();
    }
    return total;
}

const NetStats& CompositeTransport::stats() const {
    stats_ = {};
    for (const auto& child : children_) {
        const NetStats& s = child->stats();
        stats_.bytes_sent += s.bytes_sent;
        stats_.bytes_received += s.bytes_received;
        stats_.packets_sent += s.packets_sent;
        stats_.packets_received += s.packets_received;
    }
    return stats_;
}

}  // namespace eng
