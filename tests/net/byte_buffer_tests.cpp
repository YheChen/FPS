#include "engine/net/byte_buffer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

namespace {

TEST_CASE("writer/reader round-trips all primitive types", "[net]") {
    eng::ByteWriter w;
    w.u8(0xAB);
    w.u16(0x1234);
    w.u32(0xDEADBEEF);
    w.f32(-123.456f);
    w.str("hello");

    eng::ByteReader r{w.data()};
    CHECK(r.u8() == 0xAB);
    CHECK(r.u16() == 0x1234);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.f32() == -123.456f);
    CHECK(r.str(16) == "hello");
    CHECK(r.finished());
}

TEST_CASE("wire format is little-endian", "[net]") {
    eng::ByteWriter w;
    w.u16(0x1234);
    w.u32(0xAABBCCDD);
    const auto bytes = w.data();
    REQUIRE(bytes.size() == 6);
    CHECK(bytes[0] == 0x34);
    CHECK(bytes[1] == 0x12);
    CHECK(bytes[2] == 0xDD);
    CHECK(bytes[3] == 0xCC);
    CHECK(bytes[4] == 0xBB);
    CHECK(bytes[5] == 0xAA);
}

TEST_CASE("truncated reads fail and poison the reader", "[net]") {
    eng::ByteWriter w;
    w.u16(7);
    eng::ByteReader r{w.data()};
    CHECK(r.u32() == std::nullopt);  // needs 4, has 2
    CHECK_FALSE(r.ok());
    CHECK(r.u8() == std::nullopt);  // poisoned: even a valid-size read fails
    CHECK_FALSE(r.finished());
}

TEST_CASE("string reads enforce length rules", "[net]") {
    eng::ByteWriter w;
    w.str("abcdef");
    {
        eng::ByteReader r{w.data()};
        CHECK(r.str(4) == std::nullopt);  // longer than max
    }
    {
        eng::ByteWriter empty;
        empty.u8(0);  // zero-length string
        eng::ByteReader r{empty.data()};
        CHECK(r.str(8) == std::nullopt);
    }
    {
        eng::ByteWriter lying;
        lying.u8(10);  // claims 10 bytes, provides none
        eng::ByteReader r{lying.data()};
        CHECK(r.str(16) == std::nullopt);
        CHECK_FALSE(r.ok());
    }
}

TEST_CASE("non-finite floats are rejected on read", "[net]") {
    eng::ByteWriter w;
    w.f32(std::numeric_limits<float>::quiet_NaN());
    w.f32(std::numeric_limits<float>::infinity());
    eng::ByteReader r{w.data()};
    CHECK(r.f32() == std::nullopt);
    CHECK_FALSE(r.ok());
}

}  // namespace
