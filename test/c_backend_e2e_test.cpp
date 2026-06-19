#include <gtest/gtest.h>
#include "Parser.h"
#include "Sema.h"
#include "CompilerCtx.h"
#include "RenderEmit.h"
#include "RenderTypes.h"
#include "bbq_compile.h"
#include <fstream>
#include <cstdlib>
#include <cstdio>

using namespace BBQ;
using namespace bbqgen;

// Generate C code from a BBQ spec, compile it with a test harness,
// run the binary, and check the exit code.
// The test harness reads binary data, parses it, writes it back,
// and compares the round-trip output to the original.
static bool run_c_e2e(const char* bbq_spec,
                       const char* test_harness_c,
                       const uint8_t* test_data, size_t test_data_len,
                       std::string* error_out = nullptr) {
    // Parse + analyze
    auto* parser = new Parser();
    parser->init(bbq_spec, static_cast<int>(strlen(bbq_spec)));
    if (!parser->parse()) {
        if (error_out) *error_out = "parse failed";
        return false;
    }
    auto* errors = new ErrorReporter();
    auto* sema = new Sema(*errors);
    if (!sema->analyze(parser->ast)) {
        if (error_out) *error_out = "sema failed";
        return false;
    }

    // Generate C files
    std::string tmpdir = "/tmp/bbq_e2e_test_" + std::to_string(getpid());
    std::string mkdir_cmd = "mkdir -p " + tmpdir;
    system(mkdir_cmd.c_str());

    // The compiler context — the shared home the backend phases consume.
    bbq::render::CompilerCtx ctx{parser->ast, sema, nullptr, ""};

    // Types — render backend (flat type model -> inja).
    std::string types_tmpl = std::string(SOURCE_DIR) + "/backends/render/templates/types_c.inja";
    { std::ofstream f(tmpdir + "/testTypes.h"); f << bbq::render::render_types_c(ctx, types_tmpl); }

    // Reader — the destination-driven render backend: kont graph -> op-list -> inja.
    ::bbq::Compiler compiler;
    auto* g = compiler.compile_grammar(parser->ast);
    ctx.ir = g;  // accumulate the IR for the reader phases
    std::string reader_tmpl = std::string(SOURCE_DIR) + "/backends/render/templates";
    {
        std::ofstream f(tmpdir + "/testReader.h");
        f << "#ifndef TEST_READER_H\n#define TEST_READER_H\n";
        f << "#include \"testTypes.h\"\n#include \"bbq_runtime.h\"\n";
        f << bbq::render::render_reader_decls(ctx);
        f << "\n#endif\n";
    }
    {
        std::ofstream f(tmpdir + "/testReader.c");
        f << "#include \"testReader.h\"\n#include <stdlib.h>\n#include <string.h>\n";
        f << bbq::render::render_reader_c(ctx, reader_tmpl);
    }

    // Writer — the burg-driven OwningCWriter (dual of the reader over the same lowering).
    std::string writer_tmpl = std::string(SOURCE_DIR) + "/backends/render/templates";
    {
        std::ofstream f(tmpdir + "/testWriter.h");
        f << "#ifndef TEST_WRITER_H\n#define TEST_WRITER_H\n";
        f << "#include \"testTypes.h\"\n#include \"bbq_runtime.h\"\n";
        f << bbq::render::render_writer_decls(ctx);
        f << "\n#endif\n";
    }
    {
        std::ofstream f(tmpdir + "/testWriter.c");
        f << "#include \"testWriter.h\"\n#include <string.h>\n";
        f << bbq::render::render_writer_c(ctx, writer_tmpl);
    }

    // Test harness
    { std::ofstream f(tmpdir + "/harness.c"); f << test_harness_c; }

    // Test data
    {
        std::ofstream f(tmpdir + "/testdata.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(test_data), test_data_len);
    }

    // Compile
    std::string runtime_dir = std::string(SOURCE_DIR) + "/backends/c/runtime";
    std::string compile_cmd = "cc -Wall -Wextra -Werror -std=c11 -g "
        "-I" + tmpdir + " -I" + runtime_dir + " "
        + tmpdir + "/testReader.c "
        + tmpdir + "/testWriter.c "
        + tmpdir + "/harness.c "
        "-o " + tmpdir + "/test_bin 2>&1";
    FILE* compile_pipe = popen(compile_cmd.c_str(), "r");
    char compile_out[4096] = {};
    if (compile_pipe) {
        fread(compile_out, 1, sizeof(compile_out) - 1, compile_pipe);
        int rc = pclose(compile_pipe);
        if (rc != 0) {
            if (error_out) *error_out = std::string("compile failed:\n") + compile_out;
            return false;
        }
    }

    // Run
    std::string run_cmd = tmpdir + "/test_bin " + tmpdir + "/testdata.bin 2>&1";
    FILE* run_pipe = popen(run_cmd.c_str(), "r");
    char run_out[4096] = {};
    if (run_pipe) {
        fread(run_out, 1, sizeof(run_out) - 1, run_pipe);
        int rc = pclose(run_pipe);
        if (rc != 0) {
            if (error_out) *error_out = std::string("run failed (exit ") +
                std::to_string(WEXITSTATUS(rc)) + "):\n" + run_out;
            return false;
        }
    }

    // Cleanup
    system(("rm -rf " + tmpdir).c_str());
    return true;
}

// ── Simple struct: read + write round-trip ──

TEST(CBackendE2E, SimpleStructRoundTrip) {
    const char* spec =
        "@endian big\n"
        "Header = struct {\n"
        "    magic: uint32be,\n"
        "    version: uint8,\n"
        "    flags: uint8,\n"
        "    length: uint16be\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    // Read
    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    header_t hdr;
    assert(header_read(&ctx, &hdr));
    assert(hdr.magic == 0xDEADBEEF);
    assert(hdr.version == 1);
    assert(hdr.flags == 0x42);
    assert(hdr.length == 256);

    // Write
    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(header_write(&wctx, &hdr));

    // Round-trip: output matches input
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);

    printf("OK: round-trip matched (%ld bytes)\n", sz);
    return 0;
}
)";

    // Binary: magic=0xDEADBEEF, version=1, flags=0x42, length=256 (big-endian)
    uint8_t data[] = {
        0xDE, 0xAD, 0xBE, 0xEF,  // magic
        0x01,                     // version
        0x42,                     // flags
        0x01, 0x00                // length = 256
    };

    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Nested struct ──

TEST(CBackendE2E, NestedStructRoundTrip) {
    const char* spec =
        "@endian big\n"
        "Inner = struct { a: uint8, b: uint8 }\n"
        "Outer = struct { tag: uint16be, inner: Inner, trailer: uint8 }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    outer_t o;
    assert(outer_read(&ctx, &o));
    assert(o.tag == 0x1234);
    assert(o.inner.a == 0xAA);
    assert(o.inner.b == 0xBB);
    assert(o.trailer == 0xFF);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(outer_write(&wctx, &o));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);

    printf("OK: nested struct round-trip (%ld bytes)\n", sz);
    return 0;
}
)";

    uint8_t data[] = { 0x12, 0x34, 0xAA, 0xBB, 0xFF };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Array with count field ──

TEST(CBackendE2E, ArrayRoundTrip) {
    const char* spec =
        "@endian big\n"
        "Item = struct { val: uint16be }\n"
        "List = struct { count: uint8, items: array<Item>[count] }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    list_t list;
    assert(list_read(&ctx, &list));
    assert(list.count == 3);
    assert(list.items.count == 3);
    assert(list.items.items[0].val == 0x0001);
    assert(list.items.items[1].val == 0x0002);
    assert(list.items.items[2].val == 0x0003);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    // Write count field manually first, then items
    assert(list_write(&wctx, &list));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);

    free(list.items.items);
    printf("OK: array round-trip (%ld bytes)\n", sz);
    return 0;
}
)";

    uint8_t data[] = {
        0x03,                     // count = 3
        0x00, 0x01,               // items[0].val = 1
        0x00, 0x02,               // items[1].val = 2
        0x00, 0x03                // items[2].val = 3
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Bytes field (owned copy) ──

TEST(CBackendE2E, BytesFieldRoundTrip) {
    const char* spec =
        "@endian big\n"
        "Packet = struct {\n"
        "    header: uint8,\n"
        "    length: uint16be,\n"
        "    payload: bytes[length]\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    packet_t pkt;
    assert(packet_read(&ctx, &pkt));
    assert(pkt.header == 0xAA);
    assert(pkt.length == 4);
    assert(pkt.payload.length == 4);
    assert(pkt.payload.data[0] == 0x01);
    assert(pkt.payload.data[3] == 0x04);
    // Owned copy: payload.data is the struct's own storage, not a borrow of buf,
    // so the struct outlives the input buffer and packet_free releases it.
    assert(pkt.payload.data != buf + 3);
    memset(buf + 3, 0xFF, 4);                  // clobber the input: the copy is unaffected
    assert(pkt.payload.data[0] == 0x01);
    assert(pkt.payload.data[3] == 0x04);
    memcpy(buf + 3, pkt.payload.data, 4);      // restore for the byte-compare below

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(packet_write(&wctx, &pkt));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);
    packet_free(&pkt);

    printf("OK: bytes round-trip (%ld bytes)\n", sz);
    return 0;
}
)";

    uint8_t data[] = {
        0xAA,                     // header
        0x00, 0x04,               // length = 4
        0x01, 0x02, 0x03, 0x04   // payload
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Bitfield ──

TEST(CBackendE2E, BitfieldRoundTrip) {
    const char* spec =
        "@endian big\n"
        "Flags = bitfield<uint8> { high: 4, low: 4 }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    flags_t flags;
    assert(flags_read(&ctx, &flags));
    assert(flags.high == 0x0A);
    assert(flags.low == 0x05);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(flags_write(&wctx, &flags));
    assert(wctx.pos == 1);
    assert(out[0] == 0xA5);

    printf("OK: bitfield round-trip\n");
    return 0;
}
)";

    uint8_t data[] = { 0xA5 };  // high=0xA, low=0x5
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Compute field ──

TEST(CBackendE2E, ComputeField) {
    const char* spec =
        "@endian big\n"
        "Hdr = struct {\n"
        "    raw: uint8,\n"
        "    version: compute((raw >> 4) & 0x0F : uint8),\n"
        "    ihl: compute(raw & 0x0F : uint8)\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include <stdio.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    hdr_t hdr;
    assert(hdr_read(&ctx, &hdr));
    assert(hdr.raw == 0x45);
    assert(hdr.version == 4);
    assert(hdr.ihl == 5);

    printf("OK: compute fields\n");
    return 0;
}
)";

    uint8_t data[] = { 0x45 };  // version=4, ihl=5
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Constraint ──

TEST(CBackendE2E, ConstraintEnforced) {
    const char* spec =
        "@endian big\n"
        "Magic = struct {\n"
        "    value: uint32be where (value == 0xDEADBEEF)\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include <stdio.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    // Should succeed with correct magic
    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    magic_t m;
    assert(magic_read(&ctx, &m));
    assert(m.value == 0xDEADBEEF);

    // Should fail with wrong magic
    uint8_t bad[] = {0x00, 0x00, 0x00, 0x00};
    bbq_ctx_init(&ctx, bad, 4);
    assert(!magic_read(&ctx, &m));
    assert(ctx.has_error);

    printf("OK: constraint enforced\n");
    return 0;
}
)";

    uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Switch (integer discriminator) ──

TEST(CBackendE2E, SwitchRoundTrip) {
    const char* spec =
        "@endian big\n"
        "TypeA = struct { a: uint8 }\n"
        "TypeB = struct { b: uint16be }\n"
        "Msg = struct {\n"
        "    tag: uint8,\n"
        "    body: switch(tag) {\n"
        "        1: TypeA;\n"
        "        2: TypeB;\n"
        "    }\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    msg_t msg;
    assert(msg_read(&ctx, &msg));
    assert(msg.tag == 2);
    assert(msg.body.tag == 2);  // actual discriminator value
    assert(msg.body.u.case_1.b == 0x1234);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(msg_write(&wctx, &msg));
    assert(memcmp(buf, out, wctx.pos) == 0);

    printf("OK: switch round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0x02,                     // tag = 2 (TypeB)
        0x12, 0x34               // body.b = 0x1234
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Switch as inline field with trailing fields ──
//
// Regression: inline switch inside a struct must not return early —
// fields after the switch must still be parsed.

TEST(CBackendE2E, InlineSwitchWithTrailingFields) {
    const char* spec =
        "@endian big\n"
        "HeaderA = struct { a: uint8 }\n"
        "HeaderB = struct { b: uint16be }\n"
        "Packet = struct {\n"
        "    tag: uint8,\n"
        "    header: switch(tag) {\n"
        "        1: HeaderA;\n"
        "        2: HeaderB;\n"
        "    },\n"
        "    payload: uint32be\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    packet_t pkt;
    assert(packet_read(&ctx, &pkt));
    assert(pkt.tag == 1);
    assert(pkt.header.tag == 1);
    assert(pkt.header.u.case_0.a == 0x42);
    // This field comes AFTER the switch — must not be skipped
    assert(pkt.payload == 0xDEADBEEF);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(packet_write(&wctx, &pkt));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);

    printf("OK: inline switch with trailing fields round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0x01,                       // tag = 1 (HeaderA)
        0x42,                       // header.a = 0x42
        0xDE, 0xAD, 0xBE, 0xEF     // payload = 0xDEADBEEF
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Error: truncated input ──

TEST(CBackendE2E, TruncatedInput) {
    const char* spec =
        "@endian big\n"
        "Hdr = struct { a: uint32be, b: uint32be }";

    const char* harness = R"(
#include "testReader.h"
#include <stdio.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    // Only 4 bytes but struct needs 8
    uint8_t buf[] = {0xDE, 0xAD, 0xBE, 0xEF};
    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sizeof(buf));
    hdr_t hdr;
    assert(!hdr_read(&ctx, &hdr));
    assert(ctx.has_error);
    printf("OK: truncated input detected\n");
    return 0;
}
)";

    uint8_t data[] = {0}; // dummy, harness uses inline data
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, 0, &err)) << err;
}

// ── Error: write buffer too small ──

TEST(CBackendE2E, WriteBufferOverflow) {
    const char* spec =
        "@endian big\n"
        "Hdr = struct { a: uint32be, b: uint32be }";

    const char* harness = R"(
#include "testWriter.h"
#include <stdio.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    hdr_t hdr = { .a = 1, .b = 2 };
    uint8_t buf[4]; // too small for 8 bytes
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, buf, sizeof(buf));
    assert(!hdr_write(&wctx, &hdr));
    printf("OK: write overflow detected\n");
    return 0;
}
)";

    uint8_t data[] = {0};
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, 0, &err)) << err;
}

// ── Empty array ──

TEST(CBackendE2E, EmptyArray) {
    const char* spec =
        "@endian big\n"
        "Item = struct { v: uint8 }\n"
        "List = struct { count: uint8, items: array<Item>[count] }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    list_t list;
    assert(list_read(&ctx, &list));
    assert(list.count == 0);
    assert(list.items.count == 0);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(list_write(&wctx, &list));
    assert(wctx.pos == 1); // just the count byte
    assert(out[0] == 0);
    printf("OK: empty array round-trip\n");
    return 0;
}
)";

    uint8_t data[] = { 0x00 }; // count = 0
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Max/min values ──

TEST(CBackendE2E, MaxMinValues) {
    const char* spec =
        "@endian big\n"
        "Vals = struct { u8max: uint8, u16max: uint16be, i8neg: int8, i16neg: int16be }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    vals_t v;
    assert(vals_read(&ctx, &v));
    assert(v.u8max == 255);
    assert(v.u16max == 65535);
    assert(v.i8neg == -128);
    assert(v.i16neg == -32768);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(vals_write(&wctx, &v));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);
    printf("OK: max/min values round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0xFF,                     // u8max = 255
        0xFF, 0xFF,               // u16max = 65535
        0x80,                     // i8neg = -128
        0x80, 0x00                // i16neg = -32768
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Optional: present and absent ──

TEST(CBackendE2E, OptionalPresentAbsent) {
    const char* spec =
        "@endian big\n"
        "Msg = struct {\n"
        "    has_extra: uint8,\n"
        "    extra: optional<uint16be> where (has_extra != 0),\n"
        "    trailer: uint8\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    // Parse: has_extra=1, so extra is present
    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    msg_t msg;
    assert(msg_read(&ctx, &msg));
    assert(msg.has_extra == 1);
    assert(msg.extra.has_value == true);
    assert(msg.extra.value == 0xABCD);
    assert(msg.trailer == 0xFF);

    // Round-trip
    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(msg_write(&wctx, &msg));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);

    // Absent case
    uint8_t absent[] = {0x00, 0xEE}; // has_extra=0, trailer=0xEE
    bbq_ctx_init(&ctx, absent, sizeof(absent));
    msg_t msg2;
    assert(msg_read(&ctx, &msg2));
    assert(msg2.has_extra == 0);
    assert(msg2.extra.has_value == false);
    assert(msg2.trailer == 0xEE);

    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(msg_write(&wctx, &msg2));
    assert(wctx.pos == 2);
    assert(memcmp(absent, out, 2) == 0);

    printf("OK: optional present+absent round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0x01,                     // has_extra = 1 (present)
        0xAB, 0xCD,               // extra = 0xABCD
        0xFF                      // trailer
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Nested arrays: array of structs containing arrays ──

TEST(CBackendE2E, NestedArrays) {
    const char* spec =
        "@endian big\n"
        "Inner = struct { len: uint8, data: array<uint8>[len] }\n"
        "Outer = struct { count: uint8, items: array<Inner>[count] }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    outer_t o;
    assert(outer_read(&ctx, &o));
    assert(o.count == 2);
    assert(o.items.count == 2);
    assert(o.items.items[0].len == 2);
    assert(o.items.items[0].data.count == 2);
    assert(o.items.items[0].data.items[0] == 0xAA);
    assert(o.items.items[0].data.items[1] == 0xBB);
    assert(o.items.items[1].len == 1);
    assert(o.items.items[1].data.items[0] == 0xCC);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(outer_write(&wctx, &o));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);

    outer_free(&o);
    printf("OK: nested arrays round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0x02,                     // outer count = 2
        0x02, 0xAA, 0xBB,        // inner[0]: len=2, data=[AA,BB]
        0x01, 0xCC                // inner[1]: len=1, data=[CC]
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Zero-length bytes field ──

TEST(CBackendE2E, ZeroLengthBytes) {
    const char* spec =
        "@endian big\n"
        "Pkt = struct { len: uint16be, payload: bytes[len] }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    pkt_t pkt;
    assert(pkt_read(&ctx, &pkt));
    assert(pkt.len == 0);
    assert(pkt.payload.length == 0);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(pkt_write(&wctx, &pkt));
    assert(wctx.pos == 2);
    assert(memcmp(buf, out, 2) == 0);
    printf("OK: zero-length bytes round-trip\n");
    return 0;
}
)";

    uint8_t data[] = { 0x00, 0x00 }; // len = 0
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── 64-bit values ──

TEST(CBackendE2E, Uint64RoundTrip) {
    const char* spec =
        "@endian big\n"
        "Wide = struct { val: uint64be }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    wide_t w;
    assert(wide_read(&ctx, &w));
    assert(w.val == 0x0102030405060708ULL);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(wide_write(&wctx, &w));
    assert(wctx.pos == 8);
    assert(memcmp(buf, out, 8) == 0);
    printf("OK: uint64 round-trip\n");
    return 0;
}
)";

    uint8_t data[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Float round-trip ──

TEST(CBackendE2E, FloatRoundTrip) {
    const char* spec =
        "@endian big\n"
        "F = struct { val: float32be }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    f_t fv;
    assert(f_read(&ctx, &fv));
    assert(fabsf(fv.val - 3.14f) < 0.001f);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(f_write(&wctx, &fv));
    assert(wctx.pos == 4);
    assert(memcmp(buf, out, 4) == 0);
    printf("OK: float round-trip\n");
    return 0;
}
)";

    // IEEE 754 big-endian for 3.14f = 0x4048F5C3
    uint8_t data[] = { 0x40, 0x48, 0xF5, 0xC3 };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Multiple rules referencing each other ──

TEST(CBackendE2E, MultiRuleRefs) {
    const char* spec =
        "@endian big\n"
        "Header = struct { version: uint8, flags: uint8 }\n"
        "Packet = struct {\n"
        "    hdr: Header,\n"
        "    payload_size: uint16be,\n"
        "    payload: bytes[payload_size]\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    packet_t pkt;
    assert(packet_read(&ctx, &pkt));
    assert(pkt.hdr.version == 1);
    assert(pkt.hdr.flags == 0x42);
    assert(pkt.payload_size == 3);
    assert(pkt.payload.length == 3);
    assert(pkt.payload.data[0] == 0xAA);
    assert(pkt.payload.data[2] == 0xCC);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(packet_write(&wctx, &pkt));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);
    printf("OK: multi-rule refs round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0x01,                     // hdr.version = 1
        0x42,                     // hdr.flags = 0x42
        0x00, 0x03,               // payload_size = 3
        0xAA, 0xBB, 0xCC         // payload data
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Cross-rule field reference (child reads parent's field) ──

TEST(CBackendE2E, CrossRuleFieldRef) {
    const char* spec =
        "@endian big\n"
        "Header = struct { version: uint8, payload_size: uint16be }\n"
        "Payload = struct { data: bytes[payload_size] }\n"
        "Packet = struct { hdr: Header, body: Payload }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    packet_t pkt;
    assert(packet_read(&ctx, &pkt));
    assert(pkt.hdr.version == 1);
    assert(pkt.hdr.payload_size == 3);
    assert(pkt.body.data.length == 3);
    assert(pkt.body.data.data[0] == 0xAA);
    assert(pkt.body.data.data[2] == 0xCC);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(packet_write(&wctx, &pkt));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);
    printf("OK: cross-rule field ref round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0x01,                     // hdr.version = 1
        0x00, 0x03,               // hdr.payload_size = 3
        0xAA, 0xBB, 0xCC         // body.data
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Little-endian ──

TEST(CBackendE2E, LittleEndianRoundTrip) {
    const char* spec =
        "@endian little\n"
        "LEStruct = struct { a: uint16le, b: uint32le }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    le_struct_t s;
    assert(le_struct_read(&ctx, &s));
    assert(s.a == 0x0102);
    assert(s.b == 0x03040506);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(le_struct_write(&wctx, &s));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);

    printf("OK: little-endian round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0x02, 0x01,               // a = 0x0102 (LE)
        0x06, 0x05, 0x04, 0x03   // b = 0x03040506 (LE)
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Union with backtracking ──

TEST(CBackendE2E, UnionBacktracking) {
    const char* spec =
        "@endian big\n"
        "SmallMsg = struct { tag: uint8 where (tag == 0x01), val: uint8 }\n"
        "LargeMsg = struct { tag: uint8 where (tag == 0x02), val: uint16be }\n"
        "Msg = union { small: SmallMsg, large: LargeMsg }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    msg_t msg;
    assert(msg_read(&ctx, &msg));
    // tag=0x02 should match LargeMsg (second variant, index 1)
    assert(msg.tag == 1);
    assert(msg.u.large.tag == 0x02);
    assert(msg.u.large.val == 0xABCD);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(msg_write(&wctx, &msg));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);

    printf("OK: union backtracking round-trip\n");
    return 0;
}
)";

    uint8_t data[] = { 0x02, 0xAB, 0xCD }; // tag=2 → Long variant
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── LE bitfield ──

TEST(CBackendE2E, BitfieldLERoundTrip) {
    const char* spec =
        "@endian little\n"
        "Flags = bitfield<uint16le> { low: 8, high: 8 }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    flags_t flags;
    assert(flags_read(&ctx, &flags));
    assert(flags.low == 0x34);
    assert(flags.high == 0x12);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(flags_write(&wctx, &flags));
    assert(wctx.pos == 2);
    assert(memcmp(buf, out, 2) == 0);

    printf("OK: LE bitfield round-trip\n");
    return 0;
}
)";

    // 0x1234 in LE = bytes 0x34, 0x12. LE bitfield: low=0x34 (bits 0-7), high=0x12 (bits 8-15)
    uint8_t data[] = { 0x34, 0x12 };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Inline bitfield (inside struct) ──

TEST(CBackendE2E, InlineBitfieldRoundTrip) {
    const char* spec =
        "@endian big\n"
        "Pkt = struct {\n"
        "    flags: bitfield<uint8> { high: 4, low: 4 },\n"
        "    payload: uint8\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    pkt_t pkt;
    assert(pkt_read(&ctx, &pkt));
    assert(pkt.flags.high == 0x0A);
    assert(pkt.flags.low == 0x05);
    assert(pkt.payload == 0xFF);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(pkt_write(&wctx, &pkt));
    assert(wctx.pos == 2);
    assert(out[0] == 0xA5);
    assert(out[1] == 0xFF);

    printf("OK: inline bitfield round-trip\n");
    return 0;
}
)";

    uint8_t data[] = { 0xA5, 0xFF };  // flags: high=0xA, low=0x5; payload=0xFF
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Bool field ──

TEST(CBackendE2E, BoolRoundTrip) {
    const char* spec =
        "Flags = struct { a: bool, b: bool }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    flags_t flags;
    assert(flags_read(&ctx, &flags));
    assert(flags.a == true);
    assert(flags.b == false);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(flags_write(&wctx, &flags));
    assert(wctx.pos == 2);
    assert(memcmp(buf, out, 2) == 0);
    printf("OK: bool round-trip\n");
    return 0;
}
)";

    uint8_t data[] = { 0x01, 0x00 }; // true, false
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Signed integer round-trip ──

TEST(CBackendE2E, SignedIntRoundTrip) {
    const char* spec =
        "@endian big\n"
        "Vals = struct { a: int16be, b: int32be }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    vals_t v;
    assert(vals_read(&ctx, &v));
    assert(v.a == -1);
    assert(v.b == -100000);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(vals_write(&wctx, &v));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);
    printf("OK: signed int round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0xFF, 0xFF,                         // a = -1 (int16be)
        0xFF, 0xFE, 0x79, 0x60              // b = -100000 (int32be)
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── String field (zero-copy) ──

TEST(CBackendE2E, StringRoundTrip) {
    const char* spec =
        "@endian big\n"
        "Msg = struct { len: uint8, text: string[len] }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    msg_t msg;
    assert(msg_read(&ctx, &msg));
    assert(msg.len == 5);
    assert(msg.text.length == 5);
    assert(memcmp(msg.text.data, "hello", 5) == 0);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(msg_write(&wctx, &msg));
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, sz) == 0);
    printf("OK: string round-trip\n");
    return 0;
}
)";

    uint8_t data[] = { 0x05, 'h', 'e', 'l', 'l', 'o' };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Generated free functions ──

TEST(CBackendE2E, FreeFunctionGenerated) {
    const char* spec =
        "@endian big\n"
        "Item = struct { val: uint16be }\n"
        "Container = struct {\n"
        "    count: uint8,\n"
        "    items: array<Item>[count],\n"
        "    name_len: uint8,\n"
        "    name: bytes[name_len]\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include <stdio.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    container_t c;
    assert(container_read(&ctx, &c));
    assert(c.count == 2);
    assert(c.items.count == 2);
    assert(c.items.items[0].val == 0x0001);
    assert(c.items.items[1].val == 0x0002);

    // Generated free function handles nested arrays
    container_free(&c);

    // Verify items pointer was freed (can't dereference, but no crash)
    printf("OK: free function works\n");
    return 0;
}
)";

    uint8_t data[] = {
        0x02,                     // count = 2
        0x00, 0x01,               // items[0].val = 1
        0x00, 0x02,               // items[1].val = 2
        0x03,                     // name_len = 3
        'a', 'b', 'c'            // name
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

TEST(CBackendE2E, NestedFreeFunction) {
    const char* spec =
        "@endian big\n"
        "Inner = struct { len: uint8, data: array<uint8>[len] }\n"
        "Outer = struct { count: uint8, items: array<Inner>[count] }";

    const char* harness = R"(
#include "testReader.h"
#include <stdio.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    outer_t o;
    assert(outer_read(&ctx, &o));
    assert(o.count == 2);
    assert(o.items.items[0].data.count == 2);
    assert(o.items.items[1].data.count == 1);

    // One call frees everything recursively
    outer_free(&o);

    printf("OK: nested free works\n");
    return 0;
}
)";

    uint8_t data[] = {
        0x02,                     // outer count = 2
        0x02, 0xAA, 0xBB,        // inner[0]: len=2, data=[AA,BB]
        0x01, 0xCC                // inner[1]: len=1, data=[CC]
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── LEB128 varints ──

TEST(CBackendE2E, Leb128RoundTrip) {
    const char* spec =
        "Msg = struct {\n"
        "    id: uleb128,\n"
        "    delta: sleb128,\n"
        "    wide: uleb64,\n"
        "    tail: uint8\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    msg_t msg;
    assert(msg_read(&ctx, &msg));
    assert(msg.id == 300);          // uleb 0xAC 0x02
    assert(msg.delta == -1);        // sleb 0x7F
    assert(msg.wide == 624485u);    // uleb 0xE5 0x8E 0x26
    assert(msg.tail == 0x2A);
    assert(ctx.pos == (size_t)sz);  // consumed exactly the variable spans

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(msg_write(&wctx, &msg));
    // Canonical re-encode reproduces the (canonical) input byte-for-byte.
    assert(wctx.pos == (size_t)sz);
    assert(memcmp(buf, out, wctx.pos) == 0);

    printf("OK: leb128 round-trip\n");
    return 0;
}
)";

    uint8_t data[] = {
        0xAC, 0x02,              // id    = 300  (uleb128)
        0x7F,                    // delta = -1   (sleb128)
        0xE5, 0x8E, 0x26,        // wide  = 624485 (uleb64)
        0x2A                     // tail  = 42
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Switch range cases (lo..hi) ──

TEST(CBackendE2E, SwitchRangeRoundTrip) {
    const char* spec =
        "@endian big\n"
        "Small = struct { v: uint8 }\n"
        "Wide  = struct { v: uint16be }\n"
        "Msg = struct {\n"
        "    tag: uint8,\n"
        "    body: switch(tag) {\n"
        "        1..3: Small;\n"
        "        10:   Wide;\n"
        "    }\n"
        "}";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    msg_t msg;
    assert(msg_read(&ctx, &msg));
    assert(msg.tag == 2);            // tag 2 falls in range 1..3
    assert(msg.body.tag == 2);       // tag holds the actual matched discriminant
    assert(msg.body.u.case_0.v == 0x2A);

    uint8_t out[256];
    bbq_write_ctx_t wctx;
    bbq_write_ctx_init(&wctx, out, sizeof(out));
    assert(msg_write(&wctx, &msg));  // writer dispatches via expanded range labels
    assert(memcmp(buf, out, wctx.pos) == 0);

    printf("OK: switch range round-trip\n");
    return 0;
}
)";

    uint8_t data[] = { 0x02, 0x2A };   // tag=2 (range 1..3) → Small{v=42}
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Verbatim @header / @source / @writer code blocks ──

TEST(CBackendE2E, UserCodeBlocks) {
    // @header → types header (visible to all TUs); @source → reader .c;
    // @writer → writer .c. Declaring in @header and defining in @source/@writer
    // proves each block landed in the right translation unit (cross-TU linkage).
    const char* spec =
        "@header (. int reader_side(void); int writer_side(void); .)\n"
        "@source (. int reader_side(void) { return 42; } .)\n"
        "@writer (. int writer_side(void) { return 99; } .)\n"
        "Header = struct { x: uint8 }";

    const char* harness = R"(
#include "testReader.h"
#include "testWriter.h"
#include <stdio.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[256]; fread(buf, 1, sz, f); fclose(f);

    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    header_t h;
    assert(header_read(&ctx, &h));
    assert(h.x == 0x11);
    assert(reader_side() == 42);     // defined in @source (testReader.c)
    assert(writer_side() == 99);     // defined in @writer (testWriter.c)

    printf("OK: user code blocks\n");
    return 0;
}
)";

    uint8_t data[] = { 0x11 };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}

// ── Struct-level `where` calling an external verification function ──
//
// The IPv4 header checksum is a property of the whole header, so it is a
// struct-level `where` that calls a user function. The function is supplied
// via code blocks; `buffer` resolves to the header's raw bytes and `checksum`
// to the parsed field — both figured out by the where environment.

TEST(CBackendE2E, IPv4ChecksumWhere) {
    const char* spec =
        "@endian big\n"
        "@header (.\n"
        "/* One's-complement IPv4 header checksum: recompute over `buf` with the\n"
        "   checksum field (bytes 10-11) treated as zero, and compare to the parsed\n"
        "   `stored` value. Returns true iff the header checksum is valid. */\n"
        "static inline bool ipv4_checksum_ok(bbq_bytes_t buf, uint16_t stored) {\n"
        "    uint32_t sum = 0;\n"
        "    for (size_t i = 0; i + 1 < buf.length; i += 2) {\n"
        "        uint32_t w = ((uint32_t)buf.data[i] << 8) | buf.data[i+1];\n"
        "        if (i == 10) w = 0;            /* skip the checksum field itself */\n"
        "        sum += w;\n"
        "    }\n"
        "    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);\n"
        "    return (uint16_t)(~sum) == stored;\n"
        "} .)\n"
        "IPv4Header = struct {\n"
        "    ver_ihl:        uint8,\n"
        "    version:        compute((ver_ihl >> 4) & 0x0F : uint8),\n"
        "    ihl:            compute(ver_ihl & 0x0F : uint8),\n"
        "    dscp_ecn:       uint8,\n"
        "    total_length:   uint16,\n"
        "    identification: uint16,\n"
        "    flags_offset:   uint16,\n"
        "    ttl:            uint8,\n"
        "    protocol:       uint8,\n"
        "    checksum:       uint16,\n"
        "    src_addr:       uint32,\n"
        "    dst_addr:       uint32\n"
        "} where ipv4_checksum_ok(@buffer, checksum)";

    const char* harness = R"(
#include "testReader.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(int argc, char** argv) {
    (void)argc;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t buf[64]; fread(buf, 1, sz, f); fclose(f);

    // Valid header → the struct-level where (checksum) passes.
    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sz);
    i_pv4_header_t h;
    assert(i_pv4_header_read(&ctx, &h));
    assert(h.ver_ihl == 0x45);
    assert(h.checksum == 0xb861);

    // Corrupt a header byte → checksum no longer matches → the where rejects it.
    uint8_t bad[64]; memcpy(bad, buf, sz);
    bad[12] ^= 0xFF;                 // flip part of src_addr
    bbq_ctx_t ctx2;
    bbq_ctx_init(&ctx2, bad, sz);
    i_pv4_header_t h2;
    assert(!i_pv4_header_read(&ctx2, &h2));   // constraint failure

    printf("OK: ipv4 checksum where-clause\n");
    return 0;
}
)";

    // Canonical valid IPv4 header (checksum 0xb861).
    uint8_t data[] = {
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0xb8, 0x61, 0xc0, 0xa8, 0x00, 0x01,
        0xc0, 0xa8, 0x00, 0xc7
    };
    std::string err;
    ASSERT_TRUE(run_c_e2e(spec, harness, data, sizeof(data), &err)) << err;
}
