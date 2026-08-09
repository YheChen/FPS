#include "game/server/stats_store.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "engine/core/log.h"
#include "engine/net/byte_buffer.h"
#include "game/shared/protocol.h"

namespace game {

namespace {

// Distinct from any protocol magic: this file outlives the process that wrote
// it, so it has to say what it is rather than be identified by where it sits.
constexpr char kMagic[8] = {'F', 'P', 'S', 'S', 'T', 'A', 'T', '\0'};
constexpr std::uint16_t kStatsVersion = 1;

// A file that declared a huge count would otherwise be an allocation request.
// The cap is the store's own, so a file cannot smuggle in more than the
// running server would ever keep.
constexpr std::uint32_t kMaxRecordsOnDisk = kMaxStatsRecords;

std::uint64_t activity(const PlayerRecord& record) {
    return static_cast<std::uint64_t>(record.kills) + record.deaths + record.matches;
}

}  // namespace

std::vector<std::uint8_t> StatsStore::serialize() const {
    eng::ByteWriter writer;
    for (const char c : kMagic) {
        writer.u8(static_cast<std::uint8_t>(c));
    }
    writer.u16(kStatsVersion);
    writer.u32(static_cast<std::uint32_t>(records_.size()));
    // Sorted, so the same store always produces the same bytes. Hash order
    // would make two identical stores compare unequal and make diffs noise.
    std::vector<const PlayerRecord*> ordered;
    ordered.reserve(records_.size());
    for (const auto& [name, record] : records_) {
        ordered.push_back(&record);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const PlayerRecord* a, const PlayerRecord* b) { return a->name < b->name; });
    for (const PlayerRecord* record : ordered) {
        writer.str(record->name);
        writer.u32(record->kills);
        writer.u32(record->deaths);
        writer.u32(record->matches);
    }
    const std::span<const std::uint8_t> written = writer.data();
    return std::vector<std::uint8_t>{written.begin(), written.end()};
}

std::optional<StatsStore> StatsStore::deserialize(const std::vector<std::uint8_t>& bytes) {
    eng::ByteReader reader{{bytes.data(), bytes.size()}};
    for (const char expected : kMagic) {
        const auto byte = reader.u8();
        if (!byte || *byte != static_cast<std::uint8_t>(expected)) {
            return std::nullopt;
        }
    }
    const auto version = reader.u16();
    if (!version || *version != kStatsVersion) {
        return std::nullopt;
    }
    const auto count = reader.u32();
    if (!count || *count > kMaxRecordsOnDisk) {
        return std::nullopt;
    }

    StatsStore store;
    for (std::uint32_t i = 0; i < *count; ++i) {
        PlayerRecord record;
        const auto name = reader.str(kMaxNameLength);
        const auto kills = reader.u32();
        const auto deaths = reader.u32();
        const auto matches = reader.u32();
        if (!name || name->empty() || !kills || !deaths || !matches) {
            return std::nullopt;
        }
        record.name = *name;
        record.kills = *kills;
        record.deaths = *deaths;
        record.matches = *matches;
        store.records_.emplace(record.name, std::move(record));
    }
    // Trailing bytes mean the file is not what it claims, even though every
    // field parsed. Accepting it would accept a truncated-then-appended file.
    if (!reader.finished()) {
        return std::nullopt;
    }
    return store;
}

StatsStore StatsStore::open(std::filesystem::path path) {
    StatsStore store;
    store.path_ = std::move(path);

    std::error_code ec;
    if (!std::filesystem::exists(store.path_, ec)) {
        eng::log::info("Stats: starting a new file at '{}'", store.path_.string());
        return store;
    }

    std::ifstream file(store.path_, std::ios::binary);
    if (!file) {
        eng::log::error("Stats: '{}' exists but could not be opened; not saving over it",
                        store.path_.string());
        store.healthy_ = false;
        return store;
    }
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>()};
    auto parsed = deserialize(bytes);
    if (!parsed) {
        // Deliberately fatal to saving rather than to starting: the server
        // keeps running and keeps playing, it just refuses to overwrite a
        // file it could not read. Losing a session of stats beats losing all
        // of them.
        eng::log::error(
            "Stats: '{}' is corrupt or from another version ({} bytes). "
            "Keeping it and NOT saving over it -- move it aside to start fresh.",
            store.path_.string(), bytes.size());
        store.healthy_ = false;
        return store;
    }

    parsed->path_ = store.path_;
    eng::log::info("Stats: loaded {} players from '{}'", parsed->records_.size(),
                   store.path_.string());
    return *parsed;
}

bool StatsStore::save() const {
    if (!healthy_) {
        return false;  // already logged at open()
    }
    if (path_.empty()) {
        return false;
    }

    // Same directory, so the rename below cannot cross a filesystem boundary
    // -- which is the one way an "atomic" rename silently becomes a copy.
    std::filesystem::path temporary = path_;
    temporary += ".tmp";

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            eng::log::error("Stats: cannot write '{}'", temporary.string());
            return false;
        }
        const std::vector<std::uint8_t> bytes = serialize();
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            eng::log::error("Stats: write to '{}' failed", temporary.string());
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path_, ec);
    if (ec) {
        eng::log::error("Stats: could not replace '{}': {}", path_.string(), ec.message());
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

PlayerRecord& StatsStore::touch(std::string_view name) {
    const std::string key{name};
    auto it = records_.find(key);
    if (it != records_.end()) {
        return it->second;
    }
    evict_if_full();
    PlayerRecord record;
    record.name = key;
    return records_.emplace(key, std::move(record)).first->second;
}

void StatsStore::evict_if_full() {
    if (records_.size() < kMaxStatsRecords) {
        return;
    }
    auto victim = records_.begin();
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (activity(it->second) < activity(victim->second)) {
            victim = it;
        }
    }
    records_.erase(victim);
}

void StatsStore::record_kill(std::string_view name) {
    ++touch(name).kills;
}

void StatsStore::record_death(std::string_view name) {
    ++touch(name).deaths;
}

void StatsStore::record_match(std::string_view name) {
    ++touch(name).matches;
}

const PlayerRecord* StatsStore::find(std::string_view name) const {
    const auto it = records_.find(std::string{name});
    return it == records_.end() ? nullptr : &it->second;
}

std::vector<PlayerRecord> StatsStore::top(std::size_t count) const {
    std::vector<PlayerRecord> ordered;
    ordered.reserve(records_.size());
    for (const auto& [name, record] : records_) {
        ordered.push_back(record);
    }
    std::sort(ordered.begin(), ordered.end(), [](const PlayerRecord& a, const PlayerRecord& b) {
        if (a.kills != b.kills) {
            return a.kills > b.kills;
        }
        if (a.deaths != b.deaths) {
            return a.deaths < b.deaths;
        }
        // Name last, so the order is total and therefore stable. Without it
        // two equal players would swap places depending on hash iteration.
        return a.name < b.name;
    });
    if (ordered.size() > count) {
        ordered.resize(count);
    }
    return ordered;
}

}  // namespace game
