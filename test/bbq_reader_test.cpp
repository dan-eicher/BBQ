// bbq_reader_test — the C++ ZCow parse context (bbq::reader).
//
// The view parser (step 5) is the compiled CEK that BUILDS the index: it advances
// the cursor and records spans into a CaptureBuilder. bbq::reader is its parse
// context — the self-contained C++ analogue of the C bbq_ctx / CEK parse state,
// depending only on backends/cpp/runtime. These tests pin its cursor/interval
// bounds and prove a hand-built index (what the generated parser will emit) is a
// correct, decodable FieldCapture tree — the same shape the CEK builds and the
// generated handle classes read.
#include <gtest/gtest.h>
#include "bbq_reader.h"
#include "bbq_node.h"   // bbq::decode_int over the built index

#include <cstdint>

using namespace bbq;

TEST(BbqReader, CursorBounds) {
    const uint8_t buf[4] = {1, 2, 3, 4};
    ParseArena arena;
    reader r(buf, sizeof buf, arena);
    EXPECT_EQ(r.pos, 0u);
    EXPECT_TRUE(r.advance(2));
    EXPECT_EQ(r.pos, 2u);
    EXPECT_TRUE(r.advance(2));
    EXPECT_EQ(r.pos, 4u);
    EXPECT_FALSE(r.advance(1));   // past end
    EXPECT_NE(r.error, nullptr);
}

TEST(BbqReader, IntervalBoundsConfine) {
    const uint8_t buf[8] = {0,1,2,3,4,5,6,7};
    ParseArena arena;
    reader r(buf, sizeof buf, arena);
    // A window [0,3): advancing past it fails even though the buffer is longer.
    r.interval_starts.push_back(0);
    r.interval_ends.push_back(3);
    EXPECT_EQ(r.effective_end(), 3u);
    EXPECT_TRUE(r.advance(3));
    EXPECT_FALSE(r.advance(1));   // past the window, not the buffer
    r.interval_starts.pop_back();
    r.interval_ends.pop_back();
    EXPECT_EQ(r.effective_end(), 8u);
}

TEST(BbqReader, BuildsDecodableStructIndex) {
    // What a flat-struct view parser for `S = struct { x: uint8, y: uint16le }`
    // emits: advance + record span per field. The result must be a FieldCapture
    // tree the shared decoders read — the same index the CEK builds.
    const uint8_t buf[3] = {0x12, 0x34, 0x12};  // x=0x12, y(le)=0x1234
    ParseArena arena;
    reader r(buf, sizeof buf, arena);

    size_t s = r.pos;
    ASSERT_TRUE(r.advance(1));
    r.builder.add_field("x", s, r.pos, r.int_capture(8, false, false, false));

    s = r.pos;
    ASSERT_TRUE(r.advance(2));
    r.builder.add_field("y", s, r.pos, r.int_capture(16, false, false, false));

    zcow::parse_result meta = r.finish(true);
    ASSERT_TRUE(meta.success);
    ASSERT_NE(meta.doc.root(), nullptr);
    ASSERT_EQ(meta.doc.root()->kids.size(), 2u);

    int64_t bits = 0; bool sgn = false;
    ASSERT_TRUE(zcow::decode_int(node_child(meta.doc.root(),"x"), buf, &bits, &sgn));
    EXPECT_EQ(bits, 0x12);
    ASSERT_TRUE(zcow::decode_int(node_child(meta.doc.root(),"y"), buf, &bits, &sgn));
    EXPECT_EQ(bits, 0x1234);
}

TEST(BbqReader, NativeEndianCaptureFollowsRegister) {
    const uint8_t buf[2] = {0};
    ParseArena arena;
    reader r(buf, sizeof buf, arena);
    r.little_endian = true;
    EXPECT_EQ(r.int_capture(16, false, false, /*native=*/true), CaptureType::UInt16LE);
    r.little_endian = false;
    EXPECT_EQ(r.int_capture(16, false, false, /*native=*/true), CaptureType::UInt16BE);
    // Suffixed (non-native) ignores the register.
    EXPECT_EQ(r.int_capture(16, false, /*be=*/false, false), CaptureType::UInt16LE);
}
