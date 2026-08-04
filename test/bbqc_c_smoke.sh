#!/usr/bin/env bash
# bbqc C-backend smoke test: run the production CLI on a spec, then compile and
# run the generated C. Gates the bbqc cutover to render_types_c + the
# CTypeMapper-backed reader/writer (the CLI is otherwise unit-test-free).
#
# Args: $1 = bbqc binary, $2 = repo source dir.
set -euo pipefail

BBQC="$1"
SRC="$2"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/smoke.bbq" <<'EOF'
@endian big
# `ext` reaches out to the ENCLOSING rule's `tag`: a cross-rule scope
# reference, which the reader/writer spell as a cast through the scope
# pointer. Under -prefix that cast must name the PREFIXED struct type.
Inner = struct {
    a:   uint8,
    b:   uint16,
    ext: optional<uint8> where tag >= 5
}
Blob  = struct { len: uint8, data: bytes[len] }
Msg = struct {
    tag:   uint8,
    # A `where` on a bitfield entry names the entry, but the field lives under the
    # container: the constraint must read flags.hi, not a non-existent top-level hi.
    flags: bitfield<uint8> { hi: 4 where hi == 3, lo: 4 },
    inner: Inner,
    blob:  Blob,
    # An optional whose element is an ARRAY: presence is `tail.has_value` and the
    # array itself lives at `tail.value`, not at `tail`.
    tail:  optional<array<uint8>[2]> where tag >= 5
}
EOF

"$BBQC" -backend c -o "$WORK" \
    -frames "$SRC/backends/c/frames" \
    -templates "$SRC/backends/render/templates" \
    -prefix Smk "$WORK/smoke.bbq"

cat > "$WORK/main.c" <<'EOF'
#include "smk_reader.h"
#include "smk_writer.h"
#include <string.h>
#include <assert.h>

int main(void) {
    /* tag=9, flags=0x35 (hi=3 lo=5), inner.a=1 inner.b=0x0203,
     * inner.ext=0x7E (present, tag>=5), blob.len=2 data=0xAA,0xBB,
     * tail present = 0xC1,0xC2 */
    const uint8_t buf[] = { 9, 0x35, 1, 0x02, 0x03, 0x7E, 2, 0xAA, 0xBB, 0xC1, 0xC2 };
    bbq_ctx_t ctx;
    bbq_ctx_init(&ctx, buf, sizeof buf);
    smk_msg_t m;
    assert(smk_msg_read(&ctx, &m));
    assert(m.tag == 9);
    assert(m.flags.hi == 3);
    assert(m.flags.lo == 5);
    assert(m.inner.a == 1);
    assert(m.inner.b == 0x0203);
    assert(m.inner.ext.has_value);
    assert(m.inner.ext.value == 0x7E);
    assert(m.blob.len == 2);
    assert(m.tail.has_value);
    assert(m.tail.value.count == 2);
    assert(m.tail.value.items[0] == 0xC1);
    assert(m.tail.value.items[1] == 0xC2);

    uint8_t out[64];
    bbq_write_ctx_t w;
    bbq_write_ctx_init(&w, out, sizeof out);
    assert(smk_msg_write(&w, &m));
    assert(w.pos == sizeof buf);
    assert(memcmp(out, buf, sizeof buf) == 0);

    smk_msg_free(&m);
    return 0;
}
EOF

cc -Wall -Wextra -Werror -std=c11 -I"$WORK" -I"$SRC/backends/c/runtime" -I"$SRC/crt" \
    "$WORK/smk_reader.c" "$WORK/smk_writer.c" "$WORK/main.c" -o "$WORK/smoke"
"$WORK/smoke"
echo "bbqc C smoke OK"
