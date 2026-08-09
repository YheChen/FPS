#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Career statistics that survive a restart.
//
// WHAT THIS IS NOT: an achievement record. Players are identified by the name
// they typed, there are no accounts, and nothing authenticates any of it --
// so a row here is a CLAIM, and anyone willing to type your name can add to
// it. That is stated on the wire (LeaderboardEntry) and in the client UI
// rather than buried here, because a leaderboard that looks authoritative and
// is not would be worse than none.
//
// The file is a small self-describing binary blob written through the same
// ByteWriter/ByteReader used for the network protocol, so a corrupt or
// truncated file is rejected by the same discipline as a hostile packet.
// Writes go to a temporary file and are renamed over the target, which is
// atomic on POSIX and on Windows -- a crash mid-save leaves the previous file
// intact rather than a half-written one.
namespace game {

struct PlayerRecord {
    std::string name;
    std::uint32_t kills = 0;
    std::uint32_t deaths = 0;
    std::uint32_t matches = 0;
};

// Bounded because names are unauthenticated: without a cap, anyone can grow
// this file without limit by joining under fresh names. When full, the record
// with the least total activity is evicted -- which keeps the players who
// actually played, and means the cheapest attack costs an attacker more
// effort than it costs the server.
inline constexpr std::size_t kMaxStatsRecords = 500;

class StatsStore {
public:
    StatsStore() = default;

    // A missing file is a normal first run and leaves the store healthy and
    // empty. A file that exists but does not parse is NOT normal: it loads
    // nothing and leaves the store UNHEALTHY, which makes save() refuse.
    // Silently starting over would turn one bad read into permanent data
    // loss on the next write.
    static StatsStore open(std::filesystem::path path);

    void record_kill(std::string_view name);
    void record_death(std::string_view name);
    void record_match(std::string_view name);

    // Writes via a temporary file and an atomic rename. Returns false and
    // writes nothing if the store is unhealthy or the path is unset.
    bool save() const;

    // Highest kills first; ties broken by fewer deaths, then by name so the
    // order is stable rather than dependent on hash iteration.
    std::vector<PlayerRecord> top(std::size_t count) const;

    const PlayerRecord* find(std::string_view name) const;
    std::size_t size() const { return records_.size(); }
    bool healthy() const { return healthy_; }
    const std::filesystem::path& path() const { return path_; }

    // Serialization is exposed for tests: a round-trip through bytes is the
    // property worth checking, and it should not need a filesystem.
    std::vector<std::uint8_t> serialize() const;
    static std::optional<StatsStore> deserialize(const std::vector<std::uint8_t>& bytes);

private:
    PlayerRecord& touch(std::string_view name);
    void evict_if_full();

    std::unordered_map<std::string, PlayerRecord> records_;
    std::filesystem::path path_;
    bool healthy_ = true;
};

}  // namespace game
