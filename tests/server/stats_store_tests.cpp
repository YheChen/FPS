#include "game/server/stats_store.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

// A directory that cleans up after itself, so a failing assertion cannot
// leave files behind that make the NEXT run fail for a different reason.
class TempDir {
public:
    TempDir() {
        base_ = std::filesystem::temp_directory_path() /
                ("fps_stats_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(base_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path file(const char* name) const { return base_ / name; }

private:
    std::filesystem::path base_;
};

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE("stats survive a save and reload", "[stats]") {
    TempDir dir;
    const auto path = dir.file("stats.bin");

    {
        game::StatsStore store = game::StatsStore::open(path);
        CHECK(store.healthy());
        CHECK(store.size() == 0);
        store.record_kill("alice");
        store.record_kill("alice");
        store.record_death("alice");
        store.record_match("alice");
        store.record_death("bob");
        REQUIRE(store.save());
    }

    // A whole new process would see exactly this.
    const game::StatsStore reloaded = game::StatsStore::open(path);
    REQUIRE(reloaded.healthy());
    CHECK(reloaded.size() == 2);
    const game::PlayerRecord* alice = reloaded.find("alice");
    REQUIRE(alice != nullptr);
    CHECK(alice->kills == 2);
    CHECK(alice->deaths == 1);
    CHECK(alice->matches == 1);
    CHECK(reloaded.find("bob")->deaths == 1);
    CHECK(reloaded.find("nobody") == nullptr);
}

TEST_CASE("a missing file is a first run, not a failure", "[stats]") {
    TempDir dir;
    const game::StatsStore store = game::StatsStore::open(dir.file("does_not_exist.bin"));
    CHECK(store.healthy());
    CHECK(store.size() == 0);
}

// The property that matters most, because getting it wrong turns one bad read
// into permanent loss: a file that cannot be parsed must not be overwritten.
TEST_CASE("a corrupt file is preserved, not silently replaced", "[stats]") {
    TempDir dir;
    const auto path = dir.file("stats.bin");

    const std::vector<std::uint8_t> garbage{'n', 'o', 't', ' ', 's', 't', 'a', 't', 's'};
    write_bytes(path, garbage);

    game::StatsStore store = game::StatsStore::open(path);
    CHECK_FALSE(store.healthy());
    CHECK(store.size() == 0);

    // It still plays -- recording works, the server does not care.
    store.record_kill("alice");
    // ...but it refuses to write over what it could not read.
    CHECK_FALSE(store.save());

    std::ifstream check(path, std::ios::binary);
    const std::vector<std::uint8_t> still{std::istreambuf_iterator<char>(check),
                                          std::istreambuf_iterator<char>()};
    CHECK(still == garbage);
}

// Every field is attacker-influenced once this file is on a server someone
// else can reach, and a declared count is an allocation request.
TEST_CASE("hostile stats files are rejected rather than parsed", "[stats]") {
    game::StatsStore store;
    store.record_kill("alice");
    const std::vector<std::uint8_t> good = store.serialize();
    REQUIRE(game::StatsStore::deserialize(good).has_value());

    SECTION("empty") {
        CHECK_FALSE(game::StatsStore::deserialize({}).has_value());
    }

    SECTION("wrong magic") {
        std::vector<std::uint8_t> bad = good;
        bad[0] = 'X';
        CHECK_FALSE(game::StatsStore::deserialize(bad).has_value());
    }

    SECTION("wrong version") {
        std::vector<std::uint8_t> bad = good;
        bad[8] = 0xFF;
        CHECK_FALSE(game::StatsStore::deserialize(bad).has_value());
    }

    SECTION("a count far beyond the cap is not an allocation request") {
        std::vector<std::uint8_t> bad = good;
        for (std::size_t i = 10; i < 14; ++i) {
            bad[i] = 0xFF;  // ~4 billion records
        }
        CHECK_FALSE(game::StatsStore::deserialize(bad).has_value());
    }

    SECTION("truncated mid-record") {
        for (std::size_t cut = 1; cut < good.size(); ++cut) {
            std::vector<std::uint8_t> bad{good.begin(), good.begin() + static_cast<long>(cut)};
            CHECK_FALSE(game::StatsStore::deserialize(bad).has_value());
        }
    }

    SECTION("trailing junk") {
        std::vector<std::uint8_t> bad = good;
        bad.push_back(0);
        CHECK_FALSE(game::StatsStore::deserialize(bad).has_value());
    }
}

// Names are unauthenticated and free to invent, so without a bound the file
// grows for as long as someone keeps joining under new ones.
TEST_CASE("the store is bounded, and keeps the players who actually played", "[stats]") {
    game::StatsStore store;

    // One real player with a real record.
    for (int i = 0; i < 50; ++i) {
        store.record_kill("veteran");
    }
    // Then a flood of throwaway names, well past the cap.
    for (std::size_t i = 0; i < game::kMaxStatsRecords * 2; ++i) {
        store.record_death("spam" + std::to_string(i));
    }

    CHECK(store.size() <= game::kMaxStatsRecords);
    // The player with 50 kills outlived the flood; eviction is by activity,
    // not by arrival order.
    const game::PlayerRecord* veteran = store.find("veteran");
    REQUIRE(veteran != nullptr);
    CHECK(veteran->kills == 50);
}

TEST_CASE("the leaderboard is ordered and stable", "[stats]") {
    game::StatsStore store;
    store.record_kill("low");
    for (int i = 0; i < 5; ++i) {
        store.record_kill("high");
    }
    for (int i = 0; i < 5; ++i) {
        store.record_kill("tied_more_deaths");
    }
    store.record_death("tied_more_deaths");

    const std::vector<game::PlayerRecord> top = store.top(10);
    REQUIRE(top.size() == 3);
    CHECK(top[0].name == "high");              // most kills
    CHECK(top[1].name == "tied_more_deaths");  // same kills, more deaths
    CHECK(top[2].name == "low");

    // Truncation asks for the best, not an arbitrary slice.
    const std::vector<game::PlayerRecord> best = store.top(1);
    REQUIRE(best.size() == 1);
    CHECK(best[0].name == "high");

    // Serialization is order-independent: two stores built differently but
    // holding the same records must produce identical bytes, or a diff of the
    // file is noise and equality is untestable.
    game::StatsStore other;
    other.record_death("tied_more_deaths");
    for (int i = 0; i < 5; ++i) {
        other.record_kill("tied_more_deaths");
    }
    store.record_kill("zzz");
    other.record_kill("zzz");
    for (int i = 0; i < 5; ++i) {
        other.record_kill("high");
    }
    other.record_kill("low");
    CHECK(store.serialize() == other.serialize());
}
