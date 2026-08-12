#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Explicit little-endian wire serialization. NEVER memcpy structs onto the
// network (docs/packet-format.md). ByteReader is hostile-input safe: any
// out-of-bounds or invalid read poisons the reader and every subsequent
// read fails.
namespace eng {

class ByteWriter {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void f32(float value);
    // u8 length prefix + UTF-8 bytes. `value` must be <= 255 bytes.
    void str(std::string_view value);
    // u16 length prefix + UTF-8 bytes, for the one thing on this wire that is
    // genuinely kilobytes: an SDP session description. Kept separate from
    // str() rather than widening it, so every existing field keeps its
    // one-byte prefix and a 300-byte player name stays impossible.
    void long_str(std::string_view value);

    std::span<const std::uint8_t> data() const { return buffer_; }
    std::size_t size() const { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

    std::optional<std::uint8_t> u8();
    std::optional<std::uint16_t> u16();
    std::optional<std::uint32_t> u32();
    // Rejects NaN/Inf (network floats must be finite).
    std::optional<float> f32();
    // Rejects length 0 or length > max_length.
    std::optional<std::string> str(std::size_t max_length);
    // The u16-prefixed counterpart. `max_length` is still enforced by the
    // caller's own limit, so a hostile peer cannot make us allocate 64 KB
    // just by claiming to.
    std::optional<std::string> long_str(std::size_t max_length);

    bool ok() const { return !failed_; }
    std::size_t remaining() const { return data_.size() - position_; }
    // True when the whole buffer was consumed with no failures.
    bool finished() const { return ok() && remaining() == 0; }

private:
    bool take(std::size_t count, const std::uint8_t** out);

    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

}  // namespace eng
