#include "engine/net/byte_buffer.h"

#include <bit>
#include <cmath>

#include "engine/core/assert.h"

namespace eng {

void ByteWriter::u8(std::uint8_t value) {
    buffer_.push_back(value);
}

void ByteWriter::u16(std::uint16_t value) {
    buffer_.push_back(static_cast<std::uint8_t>(value & 0xffu));
    buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void ByteWriter::u32(std::uint32_t value) {
    buffer_.push_back(static_cast<std::uint8_t>(value & 0xffu));
    buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    buffer_.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
    buffer_.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
}

void ByteWriter::f32(float value) {
    u32(std::bit_cast<std::uint32_t>(value));
}

void ByteWriter::str(std::string_view value) {
    ENG_ASSERT(value.size() <= 255, "wire strings are u8-length-prefixed");
    u8(static_cast<std::uint8_t>(value.size()));
    buffer_.insert(buffer_.end(), value.begin(), value.end());
}

bool ByteReader::take(std::size_t count, const std::uint8_t** out) {
    if (failed_ || data_.size() - position_ < count) {
        failed_ = true;
        return false;
    }
    *out = data_.data() + position_;
    position_ += count;
    return true;
}

std::optional<std::uint8_t> ByteReader::u8() {
    const std::uint8_t* p = nullptr;
    if (!take(1, &p)) {
        return std::nullopt;
    }
    return *p;
}

std::optional<std::uint16_t> ByteReader::u16() {
    const std::uint8_t* p = nullptr;
    if (!take(2, &p)) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::optional<std::uint32_t> ByteReader::u32() {
    const std::uint8_t* p = nullptr;
    if (!take(4, &p)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::optional<float> ByteReader::f32() {
    const auto bits = u32();
    if (!bits) {
        return std::nullopt;
    }
    const float value = std::bit_cast<float>(*bits);
    if (!std::isfinite(value)) {
        failed_ = true;
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> ByteReader::str(std::size_t max_length) {
    const auto length = u8();
    if (!length || *length == 0 || *length > max_length) {
        failed_ = true;
        return std::nullopt;
    }
    const std::uint8_t* p = nullptr;
    if (!take(*length, &p)) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(p), *length);
}

}  // namespace eng
