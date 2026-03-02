#include <gtest/gtest.h>
#include "BBQContext.h"

using namespace bbq;

TEST(Context, ReadUint8) {
    uint8_t data[] = {0x42, 0xFF};
    BBQContext ctx(data, sizeof(data));
    uint8_t val;
    EXPECT_TRUE(ctx.read_uint8(val));
    EXPECT_EQ(val, 0x42);
    EXPECT_TRUE(ctx.read_uint8(val));
    EXPECT_EQ(val, 0xFF);
    EXPECT_FALSE(ctx.read_uint8(val));
}

TEST(Context, ReadUint16LE) {
    uint8_t data[] = {0x01, 0x02};
    BBQContext ctx(data, sizeof(data));
    uint16_t val;
    EXPECT_TRUE(ctx.read_uint16le(val));
    EXPECT_EQ(val, 0x0201);
}

TEST(Context, ReadUint16BE) {
    uint8_t data[] = {0x01, 0x02};
    BBQContext ctx(data, sizeof(data));
    uint16_t val;
    EXPECT_TRUE(ctx.read_uint16be(val));
    EXPECT_EQ(val, 0x0102);
}

TEST(Context, ReadUint32LE) {
    uint8_t data[] = {0x04, 0x03, 0x02, 0x01};
    BBQContext ctx(data, sizeof(data));
    uint32_t val;
    EXPECT_TRUE(ctx.read_uint32le(val));
    EXPECT_EQ(val, 0x01020304u);
}

TEST(Context, ReadUint32BE) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    BBQContext ctx(data, sizeof(data));
    uint32_t val;
    EXPECT_TRUE(ctx.read_uint32be(val));
    EXPECT_EQ(val, 0x01020304u);
}

TEST(Context, ReadInt8) {
    uint8_t data[] = {0xFF};
    BBQContext ctx(data, sizeof(data));
    int8_t val;
    EXPECT_TRUE(ctx.read_int8(val));
    EXPECT_EQ(val, -1);
}

TEST(Context, ReadBool) {
    uint8_t data[] = {0x00, 0x01, 0x42};
    BBQContext ctx(data, sizeof(data));
    bool val;
    EXPECT_TRUE(ctx.read_bool(val));
    EXPECT_FALSE(val);
    EXPECT_TRUE(ctx.read_bool(val));
    EXPECT_TRUE(val);
    EXPECT_TRUE(ctx.read_bool(val));
    EXPECT_TRUE(val);
}

TEST(Context, BoundsCheck) {
    uint8_t data[] = {0x01, 0x02};
    BBQContext ctx(data, sizeof(data));
    uint32_t val;
    EXPECT_FALSE(ctx.read_uint32le(val));
}

TEST(Context, Position) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    BBQContext ctx(data, sizeof(data));
    EXPECT_EQ(ctx.pos(), 0u);
    EXPECT_EQ(ctx.remaining(), 4u);
    EXPECT_EQ(ctx.total_size(), 4u);
    EXPECT_FALSE(ctx.at_end());

    uint8_t v;
    ctx.read_uint8(v);
    ctx.read_uint8(v);
    EXPECT_EQ(ctx.pos(), 2u);
    EXPECT_EQ(ctx.remaining(), 2u);

    ctx.read_uint8(v);
    ctx.read_uint8(v);
    EXPECT_TRUE(ctx.at_end());
}

TEST(Context, Interval) {
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x02, 0x03, 0x04};
    BBQContext ctx(data, sizeof(data));

    {
        auto scope = ctx.push_interval(4, 8);
        EXPECT_EQ(ctx.pos(), 4u);
        EXPECT_EQ(ctx.remaining(), 4u);
        uint32_t val;
        EXPECT_TRUE(ctx.read_uint32le(val));
        EXPECT_EQ(val, 0x04030201u);
        EXPECT_TRUE(ctx.at_end());
    }
    // After scope, depth restored; pos is 8 and length is 8, so remaining is 0
    EXPECT_EQ(ctx.pos(), 8u);
    EXPECT_TRUE(ctx.at_end());
}

TEST(Context, NestedInterval) {
    uint8_t data[16];
    for (int i = 0; i < 16; i++) data[i] = (uint8_t)i;
    BBQContext ctx(data, sizeof(data));

    {
        auto outer = ctx.push_interval(0, 8);
        EXPECT_EQ(ctx.remaining(), 8u);
        {
            auto inner = ctx.push_interval(2, 4);
            EXPECT_EQ(ctx.pos(), 2u);
            EXPECT_EQ(ctx.remaining(), 2u);
            uint8_t v;
            EXPECT_TRUE(ctx.read_uint8(v));
            EXPECT_EQ(v, 2);
        }
    }
}

TEST(Context, SaveRestore) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    BBQContext ctx(data, sizeof(data));

    auto cp = ctx.save();
    uint8_t v;
    ctx.read_uint8(v);
    ctx.read_uint8(v);
    EXPECT_EQ(ctx.pos(), 2u);

    ctx.restore(cp);
    EXPECT_EQ(ctx.pos(), 0u);
}

TEST(Context, ErrorAccumulation) {
    uint8_t data[] = {0x01};
    BBQContext ctx(data, sizeof(data));

    EXPECT_FALSE(ctx.has_errors());
    ctx.fail("first error at %zu", ctx.pos());
    ctx.fail("second error");
    EXPECT_TRUE(ctx.has_errors());
    EXPECT_EQ(ctx.errors().size(), 2u);

    std::string fmt = ctx.format_errors();
    EXPECT_NE(fmt.find("first error"), std::string::npos);
    EXPECT_NE(fmt.find("second error"), std::string::npos);
}

TEST(Context, ReadBytes) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    BBQContext ctx(data, sizeof(data));
    std::vector<uint8_t> out;
    EXPECT_TRUE(ctx.read_bytes(out, 3));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], 0x01);
    EXPECT_EQ(out[2], 0x03);
    EXPECT_EQ(ctx.pos(), 3u);
}

TEST(Context, ReadString) {
    const char* text = "Hello!XX";
    BBQContext ctx(reinterpret_cast<const uint8_t*>(text), strlen(text));
    std::string out;
    EXPECT_TRUE(ctx.read_string(out, 6));
    EXPECT_EQ(out, "Hello!");
}

TEST(Context, ReadFloat32LE) {
    float original = 3.14f;
    uint8_t data[4];
    memcpy(data, &original, 4);
    BBQContext ctx(data, sizeof(data));
    float out;
    EXPECT_TRUE(ctx.read_float32le(out));
    EXPECT_FLOAT_EQ(out, original);
}

TEST(Context, ReadUint64BE) {
    uint8_t data[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    BBQContext ctx(data, sizeof(data));
    uint64_t val;
    EXPECT_TRUE(ctx.read_uint64be(val));
    EXPECT_EQ(val, 256u);
}

TEST(Context, Crc32) {
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint32_t c = bbq::crc32(data, sizeof(data));
    EXPECT_EQ(c, 0xCBF43926u);  // Known CRC-32 of "123456789"
}

TEST(Context, ChecksumXor) {
    uint8_t data[] = {0xFF, 0x0F, 0xF0};
    uint8_t result = bbq::checksum_xor(data, sizeof(data));
    EXPECT_EQ(result, 0xFF ^ 0x0F ^ 0xF0);
}

TEST(Context, IntervalBoundsCheck) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    BBQContext ctx(data, sizeof(data));
    // Push interval with end beyond data length — should clamp
    {
        auto scope = ctx.push_interval(0, 100);
        // Effective end should be clamped to data length (4)
        EXPECT_EQ(ctx.remaining(), 4u);
    }
}

TEST(Context, IntervalStackOverflow) {
    uint8_t data[256];
    memset(data, 0, sizeof(data));
    BBQContext ctx(data, sizeof(data));
    // Push intervals up to the max
    std::vector<IntervalScope> scopes;
    for (size_t i = 0; i < 64; i++) {
        scopes.push_back(ctx.push_interval(0, sizeof(data)));
    }
    // One more should throw
    EXPECT_THROW(ctx.push_interval(0, sizeof(data)), std::runtime_error);
}

TEST(Context, BacktrackingClearsErrors) {
    uint8_t data[] = {0x01, 0x02};
    BBQContext ctx(data, sizeof(data));

    auto cp = ctx.save();
    ctx.fail("branch error 1");
    ctx.fail("branch error 2");
    EXPECT_EQ(ctx.errors().size(), 2u);

    ctx.restore(cp);
    // Errors from the abandoned branch should be cleaned up
    EXPECT_EQ(ctx.errors().size(), 0u);
}

TEST(Context, SaveRestoreErrorCount) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    BBQContext ctx(data, sizeof(data));

    ctx.fail("real error");
    EXPECT_EQ(ctx.errors().size(), 1u);

    auto cp = ctx.save();
    ctx.fail("branch error");
    EXPECT_EQ(ctx.errors().size(), 2u);

    ctx.restore(cp);
    // Only the real error survives
    EXPECT_EQ(ctx.errors().size(), 1u);
    EXPECT_NE(ctx.errors()[0].message.find("real error"), std::string::npos);
}
