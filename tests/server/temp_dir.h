#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

// A scratch directory that cleans up after itself, so a failing assertion
// cannot leave a file behind that makes the NEXT run fail differently.
namespace test {

// The name is seeded from the process id plus a per-process counter, NOT from
// the object's address. Addresses are only unique within a process, and ctest
// -j runs many test binaries at once: two processes can easily place an
// object at the same address, and then each destructor's remove_all() deletes
// the other's fixture mid-test.
class TempDir {
public:
    explicit TempDir(const char* prefix = "fps_test_") {
        static std::atomic<std::uint64_t> counter{0};
#if defined(_WIN32)
        const auto pid = static_cast<std::uint64_t>(_getpid());
#else
        const auto pid = static_cast<std::uint64_t>(::getpid());
#endif
        base_ =
            std::filesystem::temp_directory_path() / (std::string{prefix} + std::to_string(pid) +
                                                      "_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(base_);
        std::filesystem::create_directories(base_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path file(const char* name) const { return base_ / name; }
    const std::filesystem::path& path() const { return base_; }

private:
    std::filesystem::path base_;
};

}  // namespace test
