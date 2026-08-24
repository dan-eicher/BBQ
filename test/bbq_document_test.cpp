// ZCow — the parsed document, tested on its own.
//
// Nothing here involves a grammar, a machine or generated code: trees are built
// through the same builder a parser drives, so what is under test is the document
// itself — zero-copy reads, path copying, structural sharing, transience,
// serialization, and the build-phase surface a parser needs (backtracking and
// mid-parse lookup).
//
// The contract comes from L'orange 2014: §2.4/Listing 2.3 (UPDATE by path copying),
// Figure 2.2a (untouched subtrees are SHARED, not copied), §4.1 Def 4.1/4.5
// (invalidation; reads happen on the transient), §4.2/Listing 4.1 (owner id: a node
// this transient already made is not cloned again), §7.2/§8.1.2 (a transient is
// checked live and thread-owned on every operation).

#include <gtest/gtest.h>

#include <cstring>
#include <future>
#include <string>
#include <type_traits>
#include <vector>

#include "CaptureCow.h"

using namespace bbq;
using namespace bbq::zcow;

namespace {

// Doc = { hdr: { magic: u32be, n: u8 }, xs: u8[2] }  over 7 bytes.
//
//   00 01 02 03 | 04 | 05 06
//   magic         n    xs[0] xs[1]
const uint8_t kBytes[7] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x0A, 0x14 };

document make_doc() {
    builder b;
    b.begin_struct("hdr", 0);
      b.add_field("magic", 0, 4, CaptureType::UInt32BE);
      b.add_field("n",     4, 5, CaptureType::UInt8);
    b.end_struct(5);
    b.begin_array("xs", 5);
      b.add_field(nullptr, 5, 6, CaptureType::UInt8);
      b.add_field(nullptr, 6, 7, CaptureType::UInt8);
    b.end_array(7);
    return b.finish(true, 7, kBytes, sizeof kBytes).doc;
}

const node* field(const node* n, const char* a, const char* b = nullptr) {
    const node* c = child_named(n, a);
    return b ? child_named(c, b) : c;
}

std::vector<uint8_t> bytes_of(std::initializer_list<uint8_t> v) { return std::vector<uint8_t>(v); }

ComputedValue* int_cv(int64_t v) {
    static ComputedValue cv;
    cv.kind = ComputedValue::Kind::Int;
    cv.i = v;
    return &cv;
}

}  // namespace

// ── Zero copy: a document nobody wrote to owns nothing ──────────────────────

TEST(ZCow, ReadsDecodeStraightFromTheInput) {
    document d = make_doc();
    EXPECT_EQ(read_int(field(d.root(), "hdr", "magic"), d.src()), 0xDEADBEEF);
    EXPECT_EQ(read_int(field(d.root(), "hdr", "n"), d.src()), 2);
    EXPECT_EQ(read_int(child_at(field(d.root(), "xs"), 0), d.src()), 0x0A);
    EXPECT_EQ(read_int(child_at(field(d.root(), "xs"), 1), d.src()), 0x14);
}

TEST(ZCow, EveryNodeOfAFreshParseIsSpanBacked) {
    document d = make_doc();
    EXPECT_TRUE(d.root()->parsed);
    EXPECT_TRUE(field(d.root(), "hdr")->parsed);
    EXPECT_TRUE(field(d.root(), "hdr", "magic")->parsed);
    EXPECT_TRUE(field(d.root(), "xs")->parsed);
}

TEST(ZCow, UnmodifiedSerializeIsByteIdentical) {
    document d = make_doc();
    EXPECT_EQ(d.serialize(), std::vector<uint8_t>(kBytes, kBytes + sizeof kBytes));
}

TEST(ZCow, ReadingBytesPointsIntoTheInputNotACopy) {
    document d = make_doc();
    auto [p, len] = read_bytes(field(d.root(), "hdr", "magic"), d.src());
    EXPECT_EQ(p, kBytes + 0);          // the mapping itself, not a copy
    EXPECT_EQ(len, 4u);
}

// ── Writes ──────────────────────────────────────────────────────────────────

TEST(ZCow, ReadYourWritesOnTheTransient) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr", "magic"};
    node* m = t.own_path(p, nullptr, 2);
    ASSERT_NE(m, nullptr);
    set_int(m, 99);
    // Def 4.5: the read counterpart is ON the transient.
    EXPECT_EQ(read_int(field(t.root(), "hdr", "magic"), t.src()), 99);
}

TEST(ZCow, TheSourceDocumentIsUntouched) {
    document d = make_doc();
    {
        transient t = d.begin_edit();
        const char* p[] = {"hdr", "magic"};
        set_int(t.own_path(p, nullptr, 2), 99);
        (void)std::move(t).commit();
    }
    EXPECT_EQ(read_int(field(d.root(), "hdr", "magic"), d.src()), 0xDEADBEEF);
    EXPECT_EQ(d.serialize(), std::vector<uint8_t>(kBytes, kBytes + sizeof kBytes));
}

// Figure 2.2a: the new version copies the path and SHARES everything else. This is
// the test that separates copy-on-write from a deep copy — pointer identity, not
// value equality.
TEST(ZCow, UntouchedSubtreesAreSharedByPointer) {
    document d = make_doc();
    const node* xs_before = field(d.root(), "xs");
    const node* n_before  = field(d.root(), "hdr", "n");

    transient t = d.begin_edit();
    const char* p[] = {"hdr", "magic"};
    set_int(t.own_path(p, nullptr, 2), 99);
    document e = std::move(t).commit();

    // On the path: new nodes.
    EXPECT_NE(e.root(), d.root());
    EXPECT_NE(field(e.root(), "hdr"), field(d.root(), "hdr"));
    EXPECT_NE(field(e.root(), "hdr", "magic"), field(d.root(), "hdr", "magic"));
    // Off the path: the SAME nodes.
    EXPECT_EQ(field(e.root(), "xs"), xs_before);
    EXPECT_EQ(field(e.root(), "hdr", "n"), n_before);
}

TEST(ZCow, AWriteMarksOnlyThePathAsNoLongerSpanBacked) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr", "magic"};
    set_int(t.own_path(p, nullptr, 2), 99);
    document e = std::move(t).commit();

    EXPECT_FALSE(e.root()->parsed);
    EXPECT_FALSE(field(e.root(), "hdr")->parsed);
    EXPECT_FALSE(field(e.root(), "hdr", "magic")->parsed);
    EXPECT_TRUE(field(e.root(), "hdr", "n")->parsed);   // untouched sibling
    EXPECT_TRUE(field(e.root(), "xs")->parsed);
}

TEST(ZCow, EditedDocumentSerializesTheEditAndBlitsTheRest) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr", "magic"};
    set_int(t.own_path(p, nullptr, 2), 0x11223344);
    document e = std::move(t).commit();

    EXPECT_EQ(e.serialize(), bytes_of({0x11, 0x22, 0x33, 0x44, 0x02, 0x0A, 0x14}));
}

TEST(ZCow, WritingAnArrayElementByIndex) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    ASSERT_NE(xs, nullptr);
    set_int(t.own_child(xs, 1), 0x77);
    document e = std::move(t).commit();

    EXPECT_EQ(e.serialize(), bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x0A, 0x77}));
    EXPECT_EQ(read_int(child_at(field(e.root(), "xs"), 0), e.src()), 0x0A);
}

// ── Listing 4.1: the owner id means one copy per path, not one per write ────

TEST(ZCow, RepeatedWritesDownOnePathCopyItOnce) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr", "magic"};

    node* first = t.own_path(p, nullptr, 2);
    set_int(first, 1);
    node* second = t.own_path(p, nullptr, 2);
    set_int(second, 2);

    EXPECT_EQ(first, second);       // same node, cloned once
    EXPECT_EQ(read_int(field(t.root(), "hdr", "magic"), t.src()), 2);
}

TEST(ZCow, ADerivedTransientDoesNotDisturbTheOneItCameFrom) {
    document d = make_doc();
    const char* p[] = {"hdr", "magic"};

    transient t1 = d.begin_edit();
    set_int(t1.own_path(p, nullptr, 2), 111);
    document v1 = std::move(t1).commit();

    transient t2 = v1.begin_edit();
    set_int(t2.own_path(p, nullptr, 2), 222);
    document v2 = std::move(t2).commit();

    EXPECT_EQ(read_int(field(d.root(),  "hdr", "magic"), d.src()),  0xDEADBEEF);
    EXPECT_EQ(read_int(field(v1.root(), "hdr", "magic"), v1.src()), 111);
    EXPECT_EQ(read_int(field(v2.root(), "hdr", "magic"), v2.src()), 222);
}

// ── Structural edits ────────────────────────────────────────────────────────

TEST(ZCow, AppendReturnsTheNewElementAndItIsAddressable) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    node* e = t.append(xs, CaptureType::UInt8);
    ASSERT_NE(e, nullptr);
    set_int(e, 0x63);

    // Visible through the tree, not just through the pointer append handed back.
    EXPECT_EQ(size_of(field(t.root(), "xs")), 3u);
    EXPECT_EQ(read_int(child_at(field(t.root(), "xs"), 2), t.src()), 0x63);

    document v = std::move(t).commit();
    EXPECT_EQ(v.serialize(), bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x0A, 0x14, 0x63}));
}

TEST(ZCow, AppendedElementsHaveNoSpanAndDoNotReachBack) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    node* e = t.append(xs, CaptureType::UInt8);
    set_int(e, 0x63);
    EXPECT_FALSE(e->parsed);
    EXPECT_EQ(e->start_offset, e->end_offset);   // names no bytes in the input
    EXPECT_EQ(size_of(field(d.root(), "xs")), 2u);
}

// A subtree built from nothing joins the document as STRUCTURE, not as the bytes it
// would have serialized to — so it can be navigated and written afterwards. This is what
// a constructed value is: nodes, the same ones a parse makes, minus the spans.
static node_ptr built_pair(uint8_t v, uint8_t w) {
    auto s = std::make_shared<node>();
    s->type = CaptureType::Struct; s->parsed = false;
    for (auto [nm, val] : {std::pair<const char*, uint8_t>{"v", v}, {"w", w}}) {
        auto k = std::make_shared<node>();
        k->name = nm; k->type = CaptureType::UInt8;
        set_int(k.get(), val);
        s->kids.push_back(std::move(k));
    }
    return s;
}

TEST(ZCow, AnExistingSubtreeCanBeAppendedAndIsStillStructure) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    ASSERT_NE(t.append(xs, built_pair(0x63, 0x64)), nullptr);

    document v = std::move(t).commit();
    const node* added = child_at(field(v.root(), "xs"), 2);
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(size_of(added), 2u);                                   // not flattened
    EXPECT_EQ(read_int(child_named(added, "w"), v.src()), 0x64);     // and navigable
    EXPECT_EQ(v.serialize(), bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x0A, 0x14, 0x63, 0x64}));
}

// An @rest window's size is MEASURED by the emitter, not by a second walk that knows how
// wide each shape is. This is what that buys: for every node, of every shape, measuring
// and writing give the same answer — because they are the same code. A separate length
// function could pass its own tests and still drift from this one.
static void measuring_agrees(const node& n, const source& src) {
    std::vector<uint8_t> written;
    detail::emit_node(n, src, std::string(), written);
    EXPECT_EQ(detail::emitted_size(n, src), written.size())
        << "measured and written disagree for a " << (int)n.type;
    for (const auto& k : n.kids) measuring_agrees(*k, src);
}

TEST(ZCowLaw, MeasuringAgreesWithWritingForEveryShape) {
    // Span-backed, and every owned shape an edit can produce: a resized array, a
    // rewritten leaf, a byte string of a different length, and both varints — whose
    // width is the one thing that cannot be read off a type.
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    set_int(t.append(xs, CaptureType::UInt8), 0x1E);

    node* free_bits = t.append(xs, CaptureType::Bytes);
    set_bytes(free_bits, (const uint8_t*)"abcd", 4);

    for (auto e : {Enc::Uleb, Enc::Sleb})
        for (int64_t v : {(int64_t)0, (int64_t)1, (int64_t)127, (int64_t)128,
                          (int64_t)-1, (int64_t)300000}) {
            node* c = t.append(xs, CaptureType::Computed);
            set_int(c, v, e);
        }

    document v = std::move(t).commit();
    measuring_agrees(*v.root(), v.src());
    measuring_agrees(*d.root(), d.src());     // and the untouched version too
}

// A varint built from nothing has no span, and what says it occupies bytes is its
// ENCODING. Reading that off the span instead emits it as nothing — which is what a
// constructed leb would silently do.
TEST(ZCow, AConstructedVarintEmitsEvenThoughItHasNoSpan) {
    auto root = std::make_shared<node>();
    root->type = CaptureType::Struct; root->parsed = false;
    for (int64_t v : {(int64_t)300, (int64_t)1}) {
        auto k = std::make_shared<node>();
        k->type = CaptureType::Computed;
        set_int(k.get(), v, Enc::Uleb);
        root->kids.push_back(std::move(k));
    }
    EXPECT_EQ(document(root, nullptr).serialize(), bytes_of({0xAC, 0x02, 0x01}));

    // ...and a compute(), which really does occupy nothing, still emits nothing.
    auto c = std::make_shared<node>();
    c->type = CaptureType::Computed;
    set_int(c.get(), 99);
    root->kids.push_back(c);
    EXPECT_EQ(document(root, nullptr).serialize(), bytes_of({0xAC, 0x02, 0x01}));
}

TEST(ZCow, AnExistingSubtreeCanReplaceAnElement) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    ASSERT_TRUE(t.replace(xs, 0, built_pair(0x11, 0x22)));

    document v = std::move(t).commit();
    EXPECT_EQ(size_of(field(v.root(), "xs")), 2u);   // still two elements
    EXPECT_EQ(v.serialize(), bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x11, 0x22, 0x14}));
}

// An attached subtree is owned by nobody, so writing into it copies it first — the
// document it was attached to is the only one that changes.
TEST(ZCow, WritingIntoAnAttachedSubtreeCopiesIt) {
    node_ptr shared = built_pair(1, 2);
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    t.append(t.own_path(p, nullptr, 1), shared);

    node* in_tree = t.own_child(t.own_child(t.own_path(p, nullptr, 1), 2), 0);
    set_int(in_tree, 0x7F);
    document v = std::move(t).commit();

    EXPECT_EQ(read_int(child_at(child_at(field(v.root(), "xs"), 2), 0), v.src()), 0x7F);
    EXPECT_EQ(shared->kids[0]->ival, 1);   // the subtree the caller still holds
}

// Splicing replaces a subtree with bytes the caller supplies — the shape it had is gone,
// so its children go too, and the neighbours around it are untouched.
TEST(ZCow, SpliceReplacesASubtreeWithBytes) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    ASSERT_NE(t.splice(t.own_child(xs, 0), (const uint8_t*)"\x77", 1), nullptr);
    EXPECT_EQ(size_of(field(t.root(), "xs")), 2u);   // the neighbour is still there
    document v = std::move(t).commit();
    EXPECT_EQ(v.serialize(), bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x77, 0x14}));
}

// A splice that changes the length has to say so: a container's length is otherwise read
// off its children, and a spliced node has none left to read.
TEST(ZCow, ASpliceOfADifferentLengthResizes) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    t.splice(t.own_child(xs, 0), (const uint8_t*)"\x77\x88\x99", 3);
    document v = std::move(t).commit();
    EXPECT_EQ(v.serialize(), bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x77, 0x88, 0x99, 0x14}));
}

TEST(ZCow, RemoveDropsTheElementAndShortensTheOutput) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    ASSERT_TRUE(t.remove(xs, 0));
    EXPECT_EQ(size_of(field(t.root(), "xs")), 1u);
    document v = std::move(t).commit();
    EXPECT_EQ(v.serialize(), bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x14}));
    EXPECT_EQ(size_of(field(d.root(), "xs")), 2u);   // source untouched
}

// The sequence the plan measured: append, write, delete — and the view must agree
// with what serializing produces, BEFORE serializing.
TEST(ZCow, AppendWriteDeleteReadsBackWhatItWillSerialize) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);

    set_int(t.append(xs, CaptureType::UInt8), 0x63);   // xs = [0A, 14, 63]
    set_int(t.own_child(xs, 0), 0x4D);                 // xs = [4D, 14, 63]
    ASSERT_TRUE(t.remove(xs, 1));                      // xs = [4D, 63]

    const node* v = field(t.root(), "xs");
    ASSERT_EQ(size_of(v), 2u);
    EXPECT_EQ(read_int(child_at(v, 0), t.src()), 0x4D);
    EXPECT_EQ(read_int(child_at(v, 1), t.src()), 0x63);

    EXPECT_EQ(std::move(t).commit().serialize(),
              bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x4D, 0x63}));
}

// ── Values ──────────────────────────────────────────────────────────────────

TEST(ZCow, BytesLeafResizes) {
    const uint8_t raw[4] = {'a', 'b', 'c', 'd'};
    builder b;
    b.add_field("s", 0, 4, CaptureType::Bytes);
    document d = b.finish(true, 4, raw, sizeof raw).doc;

    EXPECT_EQ(read_str(field(d.root(), "s"), d.src()), "abcd");

    transient t = d.begin_edit();
    const char* p[] = {"s"};
    set_str(t.own_path(p, nullptr, 1), "wxyz!");
    document v = std::move(t).commit();
    EXPECT_EQ(read_str(field(v.root(), "s"), v.src()), "wxyz!");
    EXPECT_EQ(v.serialize(), bytes_of({'w', 'x', 'y', 'z', '!'}));
    EXPECT_EQ(read_str(field(d.root(), "s"), d.src()), "abcd");
}

TEST(ZCow, ComputedValuesHaveNoBytes) {
    const uint8_t raw[1] = {7};
    builder b;
    b.add_field("a", 0, 1, CaptureType::UInt8);
    b.add_computed("total", int_cv(42), 1);
    document d = b.finish(true, 1, raw, sizeof raw).doc;

    EXPECT_EQ(read_int(field(d.root(), "a"), d.src()), 7);
    const node* c = field(d.root(), "total");
    ASSERT_NE(c, nullptr);
    ASSERT_NE(c->computed_value, nullptr);
    EXPECT_EQ(c->computed_value->i, 42);
    EXPECT_EQ(d.serialize(), bytes_of({7}));   // contributes no bytes
}

TEST(ZCow, VarintLeafEncodesAtItsWidth) {
    const uint8_t raw[1] = {0x01};
    builder b;
    b.add_field("v", 0, 1, CaptureType::UInt32LE);
    document d = b.finish(true, 1, raw, sizeof raw).doc;

    transient t = d.begin_edit();
    const char* p[] = {"v"};
    set_int(t.own_path(p, nullptr, 1), 300, Enc::Uleb);
    EXPECT_EQ(std::move(t).commit().serialize(), bytes_of({0xAC, 0x02}));
}

TEST(ZCow, TypeMismatchIsRefused) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr", "magic"};
    node* m = t.own_path(p, nullptr, 2);
    EXPECT_THROW(set_float(m, 1.5), type_error);
}

// ── Transience: Def 4.1, §4.2, §7.2 ─────────────────────────────────────────

TEST(ZCow, UsingATransientAfterCommitThrows) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr"};
    document v = std::move(t).commit();
    EXPECT_THROW((void)t.root(), invalidated);
    EXPECT_THROW((void)t.serialize(), invalidated);
    EXPECT_THROW((void)t.own_path(p, nullptr, 1), invalidated);
}

TEST(ZCow, ATransientIsRefusedOnAnotherThread) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr"};
    // §7.2 / §8.1.2: only the thread that created the transient may operate on it.
    auto fut = std::async(std::launch::async, [&t, &p] {
        try { (void)t.own_path(p, nullptr, 1); return false; }
        catch (const invalidated&) { return true; }
    });
    EXPECT_TRUE(fut.get());
    EXPECT_NE(t.own_path(p, nullptr, 1), nullptr);   // still fine on its own thread
}

// ...and the ownership MOVES when a caller says so. The thread check approximates single
// use; a caller that enforces single use itself — a mutex around every operation, which
// is what the Python binding has — supplies the real property in place of the proxy.
// What it does not do is weaken the proxy for anyone who has not said this.
TEST(ZCow, AnAdoptedTransientMovesToTheAdoptingThread) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr"};

    auto fut = std::async(std::launch::async, [&t, &p] {
        try { (void)t.own_path(p, nullptr, 1); return false; }   // proxy stands...
        catch (const invalidated&) {}
        t.adopt();                                                // ...until told
        return t.own_path(p, nullptr, 1) != nullptr;
    });
    EXPECT_TRUE(fut.get());

    // It really moved: the thread that created it is now the stranger.
    bool refused = false;
    try { (void)t.own_path(p, nullptr, 1); }
    catch (const invalidated&) { refused = true; }
    EXPECT_TRUE(refused);

    t.adopt();
    EXPECT_NE(t.own_path(p, nullptr, 1), nullptr);
}

TEST(ZCow, TransientIsMoveOnly) {
    static_assert(!std::is_copy_constructible_v<transient>, "a transient has one owner");
    static_assert(!std::is_copy_assignable_v<transient>,    "a transient has one owner");
    static_assert(std::is_move_constructible_v<transient>,  "a transient can be handed on");
    SUCCEED();
}

TEST(ZCow, APersistentDocumentCopiesInConstantTimeAndSharesEverything) {
    document d = make_doc();
    document copy = d;                       // O(1)
    EXPECT_EQ(copy.root(), d.root());        // the same tree
    EXPECT_EQ(copy.serialize(), d.serialize());
}

TEST(ZCow, MissingFieldsResolveToNothingRatherThanCrashing) {
    document d = make_doc();
    const char* p[] = {"nope"};
    EXPECT_EQ(child_named(d.root(), "nope"), nullptr);
    EXPECT_EQ(child_at(field(d.root(), "xs"), 99), nullptr);
    EXPECT_THROW(read_int(nullptr, d.src()), type_error);

    transient t = d.begin_edit();
    EXPECT_EQ(t.own_path(p, nullptr, 1), nullptr);
}

// ── Laws ────────────────────────────────────────────────────────────────────
//
// Taken from the sources rather than invented, and quantified over every leaf type
// the grammar has rather than the three I would otherwise have picked.
//
//   L'orange 2014, Improving RRB-Tree Performance through Transience
//     Persistence     §1.2      an update preserves the original version
//     Get-after-put   §2.4      read(update(d,p,v), p) = v
//     Put-elsewhere   §2.4      read(update(d,p,v), q) = read(d,q) for q ≠ p
//     Sharing         Fig 2.2a  nothing off the updated path is copied
//     Append          §2.5      size(conj(d,x)) = size(d)+1, last = x
//     Pop             §2.6      size(pop(d)) = size(d)-1
//     Isolation       §4.1      "the mutations inside a transient are not
//                               externally visible when used correctly"
//     Round trip      Def 4.5   persistent!(transient(d)) = d
//     Invalidation    Def 4.1   an invalidated transient is not a legal argument
//     Single copy     List 4.1  a node this transient owns is not cloned again
//
//   BBQ's own — not in the paper, and labelled as such:
//     Zero-copy identity        serialize(parse(b)) = b
//     GetPut (Foster et al.)    writing back what you read changes nothing
//     Dependent fields (Nail)   after commit, a derived field equals its computation

namespace {

// Every leaf type the grammar has, with a value to write into it. The laws below
// run over this table, so adding a type to the grammar makes them cover it.
struct LeafCase {
    const char* name;
    CaptureType type;
    size_t width;
    int64_t iv;
    double  fv;
    const char* sv;
};

const std::vector<LeafCase>& leaf_cases() {
    static const std::vector<LeafCase> cases = {
        {"uint8",    CaptureType::UInt8,     1, 0xA5, 0, nullptr},
        {"uint16le", CaptureType::UInt16LE,  2, 0x1234, 0, nullptr},
        {"uint16be", CaptureType::UInt16BE,  2, 0x1234, 0, nullptr},
        {"uint32le", CaptureType::UInt32LE,  4, 0x12345678, 0, nullptr},
        {"uint32be", CaptureType::UInt32BE,  4, 0x12345678, 0, nullptr},
        {"uint64le", CaptureType::UInt64LE,  8, 0x0123456789ABCDEFLL, 0, nullptr},
        {"uint64be", CaptureType::UInt64BE,  8, 0x0123456789ABCDEFLL, 0, nullptr},
        {"int8",     CaptureType::Int8,      1, -3, 0, nullptr},
        {"int16le",  CaptureType::Int16LE,   2, -300, 0, nullptr},
        {"int16be",  CaptureType::Int16BE,   2, -300, 0, nullptr},
        {"int32le",  CaptureType::Int32LE,   4, -70000, 0, nullptr},
        {"int32be",  CaptureType::Int32BE,   4, -70000, 0, nullptr},
        {"int64le",  CaptureType::Int64LE,   8, -5000000000LL, 0, nullptr},
        {"int64be",  CaptureType::Int64BE,   8, -5000000000LL, 0, nullptr},
        // false, because the fixture byte 0x11 already reads as true — a write that
        // changes nothing is a no-op by design (see GetPutIsIdentity).
        {"bool",     CaptureType::Bool,      1, 0, 0, nullptr},
        {"float32le",CaptureType::Float32LE, 4, 0, 0.5, nullptr},
        {"float32be",CaptureType::Float32BE, 4, 0, 0.5, nullptr},
        {"float64le",CaptureType::Float64LE, 8, 0, 2.25, nullptr},
        {"float64be",CaptureType::Float64BE, 8, 0, 2.25, nullptr},
        {"bytes",    CaptureType::Bytes,     3, 0, 0, "xyz"},
        {"string",   CaptureType::String,    3, 0, 0, "abc"},
    };
    return cases;
}

bool is_span_valued(const LeafCase& c) { return c.sv != nullptr; }
bool is_float_valued(const LeafCase& c) { return is_float_type(c.type); }

// A two-field document { probe: <case type>, guard: uint16le } over zeroed bytes,
// so every law has both a field to write and a neighbour that must not move.
struct Fixture {
    std::vector<uint8_t> raw;
    document doc;
};

Fixture make_leaf_fixture(const LeafCase& c) {
    Fixture f;
    f.raw.assign(c.width + 2, 0);
    for (size_t i = 0; i < c.width; i++) f.raw[i] = (uint8_t)(0x11 * (i + 1));
    f.raw[c.width] = 0xBE; f.raw[c.width + 1] = 0xEF;

    builder b;
    b.add_field("probe", 0, c.width, c.type);
    b.add_field("guard", c.width, c.width + 2, CaptureType::UInt16LE);
    f.doc = b.finish(true, c.width + 2, f.raw.data(), f.raw.size()).doc;
    return f;
}

// Apply the case's value at "probe".
void write_probe(transient& t, const LeafCase& c) {
    const char* p[] = {"probe"};
    node* n = t.own_path(p, nullptr, 1);
    ASSERT_NE(n, nullptr);
    if (is_span_valued(c))       set_str(n, c.sv);
    else if (is_float_valued(c)) set_float(n, c.fv);
    else                         set_int(n, c.iv);
}

void expect_probe_reads_back(const document& d, const LeafCase& c) {
    const node* n = child_named(d.root(), "probe");
    ASSERT_NE(n, nullptr);
    if (is_span_valued(c))       EXPECT_EQ(read_str(n, d.src()), c.sv) << c.name;
    else if (is_float_valued(c)) EXPECT_DOUBLE_EQ(read_float(n, d.src()), c.fv) << c.name;
    else                         EXPECT_EQ(read_int(n, d.src()), c.iv) << c.name;
}

}  // namespace

// §2.4 — read(update(d, p, v), p) = v, for every leaf type.
TEST(ZCowLaw, GetAfterPut) {
    for (const auto& c : leaf_cases()) {
        Fixture f = make_leaf_fixture(c);
        transient t = f.doc.begin_edit();
        write_probe(t, c);
        // Def 4.5: the read counterpart is on the transient itself...
        {
            const node* n = child_named(t.root(), "probe");
            if (is_span_valued(c))       EXPECT_EQ(read_str(n, t.src()), c.sv) << c.name;
            else if (is_float_valued(c)) EXPECT_DOUBLE_EQ(read_float(n, t.src()), c.fv) << c.name;
            else                         EXPECT_EQ(read_int(n, t.src()), c.iv) << c.name;
        }
        // ...and on the document it commits to.
        expect_probe_reads_back(std::move(t).commit(), c);
    }
}

// §2.4 — a write at p leaves every q ≠ p reading what it read before.
TEST(ZCowLaw, PutElsewhereChangesNothing) {
    for (const auto& c : leaf_cases()) {
        Fixture f = make_leaf_fixture(c);
        int64_t guard_before = read_int(child_named(f.doc.root(), "guard"), f.doc.src());

        transient t = f.doc.begin_edit();
        write_probe(t, c);
        document v = std::move(t).commit();

        EXPECT_EQ(read_int(child_named(v.root(), "guard"), v.src()), guard_before) << c.name;
    }
}

// §1.2 / §4.1 — the version an edit was derived from is preserved, and the
// transient's mutations are not visible in it.
TEST(ZCowLaw, PersistenceAndIsolation) {
    for (const auto& c : leaf_cases()) {
        Fixture f = make_leaf_fixture(c);
        std::vector<uint8_t> before = f.doc.serialize();

        transient t = f.doc.begin_edit();
        write_probe(t, c);
        // Before committing: the source document must already be unaffected.
        EXPECT_EQ(f.doc.serialize(), before) << c.name;
        document v = std::move(t).commit();
        EXPECT_EQ(f.doc.serialize(), before) << c.name;
        EXPECT_NE(v.serialize(), before) << c.name;
    }
}

// Def 4.5 — persistent!(transient(d)) = d.
TEST(ZCowLaw, TransientRoundTripIsIdentity) {
    for (const auto& c : leaf_cases()) {
        Fixture f = make_leaf_fixture(c);
        std::vector<uint8_t> before = f.doc.serialize();
        document v = f.doc.begin_edit().commit();
        EXPECT_EQ(v.serialize(), before) << c.name;
        EXPECT_EQ(v.root(), f.doc.root()) << c.name;   // and nothing was copied
    }
}

// Foster et al. GetPut — writing back the value you just read changes nothing.
// BBQ's own law, not the transience paper's.
TEST(ZCowLaw, GetPutIsIdentity) {
    for (const auto& c : leaf_cases()) {
        Fixture f = make_leaf_fixture(c);
        std::vector<uint8_t> before = f.doc.serialize();

        const node* p = child_named(f.doc.root(), "probe");
        transient t = f.doc.begin_edit();
        const char* path[] = {"probe"};
        node* w = t.own_path(path, nullptr, 1);
        if (is_span_valued(c)) {
            auto b = read_bytes(p, f.doc.src());
            set_bytes(w, b.first, b.second);
        } else if (is_float_valued(c)) {
            set_float(w, read_float(p, f.doc.src()));
        } else {
            set_int(w, read_int(p, f.doc.src()));
        }
        EXPECT_EQ(std::move(t).commit().serialize(), before) << c.name;
    }
}

// Figure 2.2a — an update copies the path and shares everything else. Pointer
// identity, since value equality would pass for a deep copy too.
TEST(ZCowLaw, NothingOffThePathIsCopied) {
    for (const auto& c : leaf_cases()) {
        Fixture f = make_leaf_fixture(c);
        const node* guard_before = child_named(f.doc.root(), "guard");

        transient t = f.doc.begin_edit();
        write_probe(t, c);
        document v = std::move(t).commit();

        EXPECT_NE(v.root(), f.doc.root()) << c.name;                     // on the path
        EXPECT_NE(child_named(v.root(), "probe"),
                  child_named(f.doc.root(), "probe")) << c.name;         // on the path
        EXPECT_EQ(child_named(v.root(), "guard"), guard_before) << c.name; // shared
    }
}

// Listing 4.1 — a node this transient already owns is not cloned again.
TEST(ZCowLaw, RepeatedWritesCopyThePathOnce) {
    for (const auto& c : leaf_cases()) {
        Fixture f = make_leaf_fixture(c);
        transient t = f.doc.begin_edit();
        const char* p[] = {"probe"};
        node* first = t.own_path(p, nullptr, 1);
        node* second = t.own_path(p, nullptr, 1);
        EXPECT_EQ(first, second) << c.name;
    }
}

// Def 4.1 — an invalidated transient cannot be used as an argument to any function.
TEST(ZCowLaw, EveryOperationRefusesAnInvalidatedTransient) {
    for (const auto& c : leaf_cases()) {
        Fixture f = make_leaf_fixture(c);
        transient t = f.doc.begin_edit();
        const char* p[] = {"probe"};
        (void)std::move(t).commit();

        EXPECT_THROW((void)t.root(), invalidated) << c.name;
        EXPECT_THROW((void)t.serialize(), invalidated) << c.name;
        EXPECT_THROW((void)t.own_path(p, nullptr, 1), invalidated) << c.name;
        EXPECT_THROW((void)t.own_child(nullptr, 0), invalidated) << c.name;
        EXPECT_THROW((void)t.append(nullptr, c.type), invalidated) << c.name;
        EXPECT_THROW((void)t.remove(nullptr, 0), invalidated) << c.name;
    }
}

// BBQ's own: a parse that nobody wrote to serializes to the bytes it read.
TEST(ZCowLaw, ZeroCopyIdentity) {
    for (const auto& c : leaf_cases()) {
        Fixture f = make_leaf_fixture(c);
        EXPECT_EQ(f.doc.serialize(), f.raw) << c.name;
    }
}

// A file holds bytes no field describes — separators, padding, alignment, regions a
// seek jumped over. They belong to the document as much as the fields do, and an
// edit must not consume them. This is why an edit that resizes nothing patches the
// input in place instead of rebuilding from the tree: rebuilding only knows about
// nodes, so everything between them would vanish.
TEST(ZCowLaw, BytesNoFieldCoversSurviveAnEdit) {
    const uint8_t raw[5] = {0x0A, 0xFF, 0xEE, 0x0B, 0xDD};   // [1,3) and [4,5) are gaps
    builder b;
    b.add_field("a", 0, 1, CaptureType::UInt8);
    b.add_field("b", 3, 4, CaptureType::UInt8);
    document d = b.finish(true, 5, raw, sizeof raw).doc;
    EXPECT_EQ(d.serialize(), std::vector<uint8_t>(raw, raw + 5));

    transient t = d.begin_edit();
    const char* p[] = {"a"};
    set_int(t.own_path(p, nullptr, 1), 0x77);
    EXPECT_EQ(std::move(t).commit().serialize(),
              bytes_of({0x77, 0xFF, 0xEE, 0x0B, 0xDD}));
}

// The same for a layout whose fields are not in file order — patching writes each
// leaf where it belongs, so the order is preserved rather than normalised away.
TEST(ZCowLaw, AScatteredLayoutKeepsItsOrder) {
    const uint8_t raw[4] = {0x0B, 0x00, 0x0A, 0x00};   // b at [0,2), a at [2,4)
    builder b;
    b.add_field("a", 2, 4, CaptureType::UInt16LE);
    b.add_field("b", 0, 2, CaptureType::UInt16LE);
    document d = b.finish(true, 4, raw, sizeof raw).doc;
    EXPECT_EQ(d.serialize(), std::vector<uint8_t>(raw, raw + 4));

    transient t = d.begin_edit();
    const char* p[] = {"a"};
    set_int(t.own_path(p, nullptr, 1), 0x0C);
    EXPECT_EQ(std::move(t).commit().serialize(), bytes_of({0x0B, 0x00, 0x0C, 0x00}));
}

// KNOWN LIMITATION, pinned rather than left to be discovered: once something
// resizes, the document has to be rebuilt from the tree, and a rebuild only knows
// about nodes — so bytes no field covers are lost. Patching cannot be used because
// the offsets after the resize no longer hold.
TEST(ZCowLaw, AResizeDropsBytesNoFieldCovers) {
    const uint8_t raw[4] = {0x0A, 0xFF, 0x0B, 0x0C};   // [1,2) is a gap
    builder b;
    b.add_field("a", 0, 1, CaptureType::UInt8);
    b.begin_array("xs", 2);
      b.add_field(nullptr, 2, 3, CaptureType::UInt8);
      b.add_field(nullptr, 3, 4, CaptureType::UInt8);
    b.end_array(4);
    document d = b.finish(true, 4, raw, sizeof raw).doc;
    EXPECT_EQ(d.serialize(), std::vector<uint8_t>(raw, raw + 4));

    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    set_int(t.append(t.own_path(p, nullptr, 1), CaptureType::UInt8), 0x0D);
    // The 0xFF is gone. Documented so the behaviour is a decision, not a surprise.
    EXPECT_EQ(std::move(t).commit().serialize(), bytes_of({0x0A, 0x0B, 0x0C, 0x0D}));
}

// §2.5 / §2.6 — the append and pop laws, over every element type.
TEST(ZCowLaw, AppendAndPopSizeLaws) {
    for (const auto& c : leaf_cases()) {
        if (is_span_valued(c)) continue;   // an appended span has no declared width

        builder b;
        b.begin_array("xs", 0);
          b.add_field(nullptr, 0, c.width, c.type);
        b.end_array(c.width);
        std::vector<uint8_t> raw(c.width, 0x11);
        document d = b.finish(true, c.width, raw.data(), raw.size()).doc;

        const size_t n0 = size_of(child_named(d.root(), "xs"));

        transient t = d.begin_edit();
        const char* p[] = {"xs"};
        node* xs = t.own_path(p, nullptr, 1);
        node* added = t.append(xs, c.type);
        if (is_float_valued(c)) set_float(added, c.fv); else set_int(added, c.iv);

        // conj: one longer, and the new element is last.
        EXPECT_EQ(size_of(child_named(t.root(), "xs")), n0 + 1) << c.name;
        const node* last = child_at(child_named(t.root(), "xs"), n0);
        if (is_float_valued(c)) EXPECT_DOUBLE_EQ(read_float(last, t.src()), c.fv) << c.name;
        else                    EXPECT_EQ(read_int(last, t.src()), c.iv) << c.name;

        // pop: back to where it started.
        ASSERT_TRUE(t.remove(xs, n0));
        EXPECT_EQ(size_of(child_named(t.root(), "xs")), n0) << c.name;
        EXPECT_EQ(std::move(t).commit().serialize(), raw) << c.name;
    }
}

// Reading addresses a struct field by name (`child_named`) as well as by position,
// because a name is how the grammar talks about it. Writing has to do the same, or
// every caller reimplements the name→index scan.
TEST(ZCow, OwningAChildByName) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr"};
    node* hdr = t.own_path(p, nullptr, 1);
    ASSERT_NE(hdr, nullptr);

    node* magic = t.own_child(hdr, "magic");
    ASSERT_NE(magic, nullptr);
    set_int(magic, 0x11223344);

    EXPECT_EQ(read_int(field(t.root(), "hdr", "magic"), t.src()), 0x11223344);
    EXPECT_EQ(read_int(field(t.root(), "hdr", "n"), t.src()), 2);   // sibling untouched
    EXPECT_EQ(std::move(t).commit().serialize(),
              bytes_of({0x11, 0x22, 0x33, 0x44, 0x02, 0x0A, 0x14}));
}

TEST(ZCow, OwningAChildByANameThatIsNotThereYieldsNothing) {
    document d = make_doc();
    transient t = d.begin_edit();
    const char* p[] = {"hdr"};
    EXPECT_EQ(t.own_child(t.own_path(p, nullptr, 1), "nope"), nullptr);
}

// ── Bitfields ───────────────────────────────────────────────────────────────
//
// An entry has no storage of its own: it is a run of bits inside a containing
// scalar, and only the grammar knows where. So both directions go through the
// CONTAINER, with the layout supplied by the caller who has it from the grammar.
//
// Reading through the container rather than through the entry's own recorded value
// is the point: that value is what the parse decoded, and it does not move when a
// neighbouring entry is written.

namespace {

// (bits [lo unsigned 4] [hi unsigned 4]) over one byte 0xA5 — lo = 5, hi = A.
// Ftypes lists entries by their bit range, so `lo` is [0,4) and `hi` is [4,8).
const uint8_t kBits[1] = {0xA5};

document make_bits() {
    builder b;
    node* f = nullptr;
    b.begin_struct("f", 0);
      node* lo = b.add_computed("lo", int_cv(0x5), 0, 1);
      lo->bits = {0, 4, false, true};
      node* hi = b.add_computed("hi", int_cv(0xA), 0, 1);
      hi->bits = {4, 8, false, true};
    f = b.end_struct(1);
    f->bits_container = true;
    return b.finish(true, 1, kBits, sizeof kBits).doc;
}

}  // namespace

TEST(ZCowBits, EntriesReadFromTheContainersBytes) {
    document d = make_bits();
    const node* f = field(d.root(), "f");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(read_bits(f, "lo", d.src()), 0x5);
    EXPECT_EQ(read_bits(f, "hi", d.src()), 0xA);
}

// Ftypes §3.4 records a signed flag per entry, and its own example round-trips
// `[x signed 4]` as -3 — a 4-bit field holding 0xD. An unsigned read gives 13.
TEST(ZCowBits, ASignedEntryIsSignExtended) {
    const uint8_t raw[1] = {0x0D};
    builder b;
    b.begin_struct("f", 0);
      node* x = b.add_computed("x", int_cv(-3), 0, 1);
      x->bits = {0, 4, /*is_signed=*/true, true};
      node* u = b.add_computed("u", int_cv(0), 0, 1);
      u->bits = {0, 4, /*is_signed=*/false, true};
    node* f = b.end_struct(1);
    f->bits_container = true;
    document d = b.finish(true, 1, raw, sizeof raw).doc;

    const node* fn = field(d.root(), "f");
    EXPECT_EQ(read_bits(fn, "x", d.src()), -3);
    EXPECT_EQ(read_bits(fn, "u", d.src()), 13);
}

TEST(ZCowBits, WritingOneEntryLeavesItsNeighbourAlone) {
    document d = make_bits();
    transient t = d.begin_edit();
    const char* p[] = {"f"};
    node* f = t.own_path(p, nullptr, 1);
    ASSERT_NE(f, nullptr);
    write_bits(f, "lo", t.src(), 0x3);

    // Read back through the container. The entries' own recorded values are what the
    // parse decoded and do not move, so reading those would give the old answer —
    // which is why a bit entry is derived, never stored.
    EXPECT_EQ(read_bits(f, "lo", t.src()), 0x3);
    EXPECT_EQ(read_bits(f, "hi", t.src()), 0xA);

    document v = std::move(t).commit();
    EXPECT_EQ(v.serialize(), bytes_of({0xA3}));
    EXPECT_EQ(d.serialize(), bytes_of({0xA5}));   // the source is untouched
}

TEST(ZCowBits, WritingBothEntriesComposes) {
    document d = make_bits();
    transient t = d.begin_edit();
    const char* p[] = {"f"};
    node* f = t.own_path(p, nullptr, 1);
    write_bits(f, "lo", t.src(), 0x3);
    write_bits(f, "hi", t.src(), 0xC);
    EXPECT_EQ(read_bits(f, "lo", t.src()), 0x3);
    EXPECT_EQ(read_bits(f, "hi", t.src()), 0xC);
    EXPECT_EQ(std::move(t).commit().serialize(), bytes_of({0xC3}));
}

TEST(ZCowBits, ASignedEntryRoundTripsANegativeValue) {
    const uint8_t raw[1] = {0x00};
    builder b;
    b.begin_struct("f", 0);
      node* x = b.add_computed("x", int_cv(0), 0, 1);
      x->bits = {0, 4, true, true};
    node* f = b.end_struct(1);
    f->bits_container = true;
    document d = b.finish(true, 1, raw, sizeof raw).doc;

    transient t = d.begin_edit();
    const char* p[] = {"f"};
    node* fn = t.own_path(p, nullptr, 1);
    write_bits(fn, "x", t.src(), -3);
    EXPECT_EQ(read_bits(fn, "x", t.src()), -3);
    EXPECT_EQ(std::move(t).commit().serialize(), bytes_of({0x0D}));
}

// Ftypes §3.4 puts `swap?` on the CONTAINER, not on each access.
TEST(ZCowBits, AContainerLaidOutMsbFirstPacksTheOtherWay) {
    const uint8_t raw[2] = {0x12, 0x34};        // msb-first: 0x1234
    builder b;
    b.begin_struct("f", 0);
      node* top = b.add_computed("top", int_cv(0x12), 0, 2);
      top->bits = {8, 16, false, true};
      node* bot = b.add_computed("bot", int_cv(0x34), 0, 2);
      bot->bits = {0, 8, false, true};
    node* f = b.end_struct(2);
    f->bits_container = true;
    f->bits_msb_first = true;
    document d = b.finish(true, 2, raw, sizeof raw).doc;

    const node* fn = field(d.root(), "f");
    EXPECT_EQ(read_bits(fn, "top", d.src()), 0x12);
    EXPECT_EQ(read_bits(fn, "bot", d.src()), 0x34);

    transient t = d.begin_edit();
    const char* p[] = {"f"};
    write_bits(t.own_path(p, nullptr, 1), "top", t.src(), 0xAB);
    EXPECT_EQ(std::move(t).commit().serialize(), bytes_of({0xAB, 0x34}));
}

TEST(ZCowBits, PuttingBackWhatYouReadIsIdentity) {
    document d = make_bits();
    transient t = d.begin_edit();
    const char* p[] = {"f"};
    node* f = t.own_path(p, nullptr, 1);
    write_bits(f, "lo", t.src(), read_bits(f, "lo", t.src()));
    EXPECT_EQ(std::move(t).commit().serialize(), bytes_of({0xA5}));
}

TEST(ZCowBits, ABitWriteDoesNotResizeSoEverythingAroundItSurvives) {
    // The container sits between two ordinary fields, with a byte no field covers.
    // A bit write never changes width, so serializing patches in place and the gap
    // is preserved.
    const uint8_t raw[4] = {0x11, 0xA5, 0xFF, 0x22};   // [2,3) is a gap
    builder b;
    b.add_field("a", 0, 1, CaptureType::UInt8);
    b.begin_struct("f", 1);
      node* lo = b.add_computed("lo", int_cv(0x5), 1, 2);
      lo->bits = {0, 4, false, true};
      node* hi = b.add_computed("hi", int_cv(0xA), 1, 2);
      hi->bits = {4, 8, false, true};
    node* f = b.end_struct(2);
    f->bits_container = true;
    b.add_field("z", 3, 4, CaptureType::UInt8);
    document d = b.finish(true, 4, raw, sizeof raw).doc;
    EXPECT_EQ(d.serialize(), std::vector<uint8_t>(raw, raw + 4));

    transient t = d.begin_edit();
    const char* p[] = {"f"};
    write_bits(t.own_path(p, nullptr, 1), "hi", t.src(), 0x3);
    EXPECT_EQ(std::move(t).commit().serialize(), bytes_of({0x11, 0x35, 0xFF, 0x22}));
}

TEST(ZCowBits, NamingAnEntryTheRunDoesNotHaveIsRefused) {
    document d = make_bits();
    const node* f = field(d.root(), "f");
    EXPECT_THROW((void)read_bits(f, "nope", d.src()), type_error);

    transient t = d.begin_edit();
    const char* p[] = {"f"};
    EXPECT_THROW(write_bits(t.own_path(p, nullptr, 1), "nope", t.src(), 1), type_error);
}

// ── Dependent fields ────────────────────────────────────────────────────────
//
// A count, or an @rest window's size, is the grammar's to compute — Nail's
// dependent field, "not exposed in the data model, but instead transparently
// computed". The parser records what a field determines when it knows; committing
// makes it true. A caller never sets one, and cannot leave one wrong.

namespace {

// Doc = { n: uint8 = |xs|, xs: uint8[n] }  over {02, 0A, 14}
const uint8_t kCounted[3] = {0x02, 0x0A, 0x14};

document make_counted() {
    builder b;
    b.add_field("n", 0, 1, CaptureType::UInt8);
    b.begin_array("xs", 1);
      b.add_field(nullptr, 1, 2, CaptureType::UInt8);
      b.add_field(nullptr, 2, 3, CaptureType::UInt8);
    node* xs = b.end_array(3);
    xs->derives = node::Derives::HasCount;
    xs->determines = "n";
    return b.finish(true, 3, kCounted, sizeof kCounted).doc;
}

}  // namespace

TEST(ZCowDerived, AnUntouchedDocumentIsStillByteIdentical) {
    document d = make_counted();
    EXPECT_EQ(d.serialize(), std::vector<uint8_t>(kCounted, kCounted + 3));
    // Committing without writing must not disturb a dependent field either.
    document same = d.begin_edit().commit();
    EXPECT_EQ(same.serialize(), std::vector<uint8_t>(kCounted, kCounted + 3));
    EXPECT_TRUE(same.root()->parsed);
}

TEST(ZCowDerived, AppendingUpdatesTheCountWithoutBeingAsked) {
    document d = make_counted();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    set_int(t.append(t.own_path(p, nullptr, 1), CaptureType::UInt8), 0x1E);
    document v = std::move(t).commit();

    EXPECT_EQ(read_int(field(v.root(), "n"), v.src()), 3);
    EXPECT_EQ(v.serialize(), bytes_of({0x03, 0x0A, 0x14, 0x1E}));
    // The document it came from is untouched, count included.
    EXPECT_EQ(read_int(field(d.root(), "n"), d.src()), 2);
}

// Serializing a TRANSIENT is what a caller that goes on editing does — the Python
// binding hands out bytes without ever giving up the ability to edit — so it has to
// settle the dependent fields exactly as commit does. Every other serialize test here
// runs on a committed document, which is how transient::serialize came to emit the
// count the PARSE recorded rather than the one the edits produced.
TEST(ZCowDerived, SerializingATransientSettlesTheCountToo) {
    document d = make_counted();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    set_int(t.append(t.own_path(p, nullptr, 1), CaptureType::UInt8), 0x1E);

    EXPECT_EQ(t.serialize(), bytes_of({0x03, 0x0A, 0x14, 0x1E}));

    // Still alive and still editable afterwards — the reason not to make a caller
    // commit just to look at the bytes — and the walk repeats without drifting.
    set_int(t.append(t.own_path(p, nullptr, 1), CaptureType::UInt8), 0x28);
    EXPECT_EQ(t.serialize(), bytes_of({0x04, 0x0A, 0x14, 0x1E, 0x28}));
    EXPECT_EQ(std::move(t).commit().serialize(), bytes_of({0x04, 0x0A, 0x14, 0x1E, 0x28}));
}

TEST(ZCowDerived, RemovingUpdatesTheCount) {
    document d = make_counted();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    ASSERT_TRUE(t.remove(t.own_path(p, nullptr, 1), 0));
    document v = std::move(t).commit();

    EXPECT_EQ(read_int(field(v.root(), "n"), v.src()), 1);
    EXPECT_EQ(v.serialize(), bytes_of({0x01, 0x14}));
}

TEST(ZCowDerived, ACountThatDidNotChangeStaysSpanBacked) {
    document d = make_counted();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    // Write an element: the array's contents changed but its length did not.
    set_int(t.own_child(t.own_path(p, nullptr, 1), 0), 0x77);
    document v = std::move(t).commit();

    EXPECT_TRUE(field(v.root(), "n")->parsed);   // not rewritten for no reason
    EXPECT_EQ(v.serialize(), bytes_of({0x02, 0x77, 0x14}));
}

TEST(ZCowDerived, ARestWindowSizeIsRecomputedWhenItsContentResizes) {
    // Doc = { sz: uint8 = bytes after me, body: bytes }
    const uint8_t raw[4] = {0x03, 'a', 'b', 'c'};
    builder b;
    b.add_field("sz", 0, 1, CaptureType::UInt8);
    b.add_field("body", 1, 4, CaptureType::Bytes);
    b.root_determines(node::Derives::HasRestSize, "sz");
    document d = b.finish(true, 4, raw, sizeof raw).doc;

    EXPECT_EQ(d.serialize(), std::vector<uint8_t>(raw, raw + 4));

    transient t = d.begin_edit();
    const char* p[] = {"body"};
    set_str(t.own_path(p, nullptr, 1), "wxyz!");
    document v = std::move(t).commit();

    EXPECT_EQ(read_int(field(v.root(), "sz"), v.src()), 5);
    EXPECT_EQ(v.serialize(), bytes_of({0x05, 'w', 'x', 'y', 'z', '!'}));
}

TEST(ZCowDerived, NestedCountsAreFixedInnermostFirst) {
    // Doc = { sz: uint8 = bytes after me, n: uint8 = |xs|, xs: uint8[n] }
    // Appending grows xs, which grows n's window, which grows sz.
    const uint8_t raw[4] = {0x03, 0x02, 0x0A, 0x14};
    builder b;
    b.add_field("sz", 0, 1, CaptureType::UInt8);
    b.add_field("n", 1, 2, CaptureType::UInt8);
    b.begin_array("xs", 2);
      b.add_field(nullptr, 2, 3, CaptureType::UInt8);
      b.add_field(nullptr, 3, 4, CaptureType::UInt8);
    node* xs = b.end_array(4);
    xs->derives = node::Derives::HasCount;
    xs->determines = "n";
    b.root_determines(node::Derives::HasRestSize, "sz");
    document d = b.finish(true, 4, raw, sizeof raw).doc;
    EXPECT_EQ(d.serialize(), std::vector<uint8_t>(raw, raw + 4));

    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    set_int(t.append(t.own_path(p, nullptr, 1), CaptureType::UInt8), 0x1E);
    document v = std::move(t).commit();

    EXPECT_EQ(read_int(field(v.root(), "n"), v.src()), 3);   // count first
    EXPECT_EQ(read_int(field(v.root(), "sz"), v.src()), 4);  // then the window over it
    EXPECT_EQ(v.serialize(), bytes_of({0x04, 0x03, 0x0A, 0x14, 0x1E}));
}

TEST(ZCowDerived, AnEditElsewhereLeavesAnUntouchedSubtreesCountAlone) {
    // Two counted arrays; editing one must not rewrite the other's count, and must
    // not walk into it at all — the clean subtree is shared, not copied.
    const uint8_t raw[6] = {0x01, 0x0A, 0x02, 0x14, 0x1E, 0x99};
    builder b;
    b.begin_struct("l", 0);
      b.add_field("n", 0, 1, CaptureType::UInt8);
      b.begin_array("xs", 1);
        b.add_field(nullptr, 1, 2, CaptureType::UInt8);
      node* lxs = b.end_array(2);
      lxs->derives = node::Derives::HasCount; lxs->determines = "n";
    b.end_struct(2);
    b.begin_struct("r", 2);
      b.add_field("n", 2, 3, CaptureType::UInt8);
      b.begin_array("xs", 3);
        b.add_field(nullptr, 3, 4, CaptureType::UInt8);
        b.add_field(nullptr, 4, 5, CaptureType::UInt8);
      node* rxs = b.end_array(5);
      rxs->derives = node::Derives::HasCount; rxs->determines = "n";
    b.end_struct(5);
    b.add_field("tail", 5, 6, CaptureType::UInt8);
    document d = b.finish(true, 6, raw, sizeof raw).doc;
    const node* right_before = field(d.root(), "r");

    transient t = d.begin_edit();
    const char* p[] = {"l", "xs"};
    set_int(t.append(t.own_path(p, nullptr, 2), CaptureType::UInt8), 0x0B);
    document v = std::move(t).commit();

    EXPECT_EQ(read_int(child_named(field(v.root(), "l"), "n"), v.src()), 2);
    EXPECT_EQ(read_int(child_named(field(v.root(), "r"), "n"), v.src()), 2);
    EXPECT_EQ(field(v.root(), "r"), right_before);   // shared, never visited
    EXPECT_EQ(v.serialize(), bytes_of({0x02, 0x0A, 0x0B, 0x02, 0x14, 0x1E, 0x99}));
}

TEST(ZCowDerived, ACountOverAnArrayThatWasEmptiedGoesToZero) {
    document d = make_counted();
    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    node* xs = t.own_path(p, nullptr, 1);
    ASSERT_TRUE(t.remove(xs, 0));
    ASSERT_TRUE(t.remove(xs, 0));
    document v = std::move(t).commit();

    EXPECT_EQ(read_int(field(v.root(), "n"), v.src()), 0);
    EXPECT_EQ(v.serialize(), bytes_of({0x00}));
}

TEST(ZCowDerived, ADanglingDeterminesNameIsIgnoredRatherThanCrashing) {
    const uint8_t raw[2] = {0x01, 0x0A};
    builder b;
    b.add_field("n", 0, 1, CaptureType::UInt8);
    b.begin_array("xs", 1);
      b.add_field(nullptr, 1, 2, CaptureType::UInt8);
    node* xs = b.end_array(2);
    xs->derives = node::Derives::HasCount;
    xs->determines = "nothing_by_that_name";
    document d = b.finish(true, 2, raw, sizeof raw).doc;

    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    set_int(t.append(t.own_path(p, nullptr, 1), CaptureType::UInt8), 0x14);
    document v = std::move(t).commit();
    EXPECT_EQ(v.serialize(), bytes_of({0x01, 0x0A, 0x14}));   // count left as it was
}

// The count a change invalidates can live in a subtree the change never touched —
// `struct { h: struct { n }, xs: array[h.n] }`. A dotted path finds it, and the
// nodes on the way to it are owned so it can be written.
TEST(ZCowDerived, ACountInANestedStructIsStillFound) {
    const uint8_t raw[3] = {0x02, 0x0A, 0x14};
    builder b;
    b.begin_struct("h", 0);
      b.add_field("n", 0, 1, CaptureType::UInt8);
    b.end_struct(1);
    b.begin_array("xs", 1);
      b.add_field(nullptr, 1, 2, CaptureType::UInt8);
      b.add_field(nullptr, 2, 3, CaptureType::UInt8);
    node* xs = b.end_array(3);
    xs->derives = node::Derives::HasCount;
    xs->determines = "h.n";
    document d = b.finish(true, 3, raw, sizeof raw).doc;

    transient t = d.begin_edit();
    const char* p[] = {"xs"};
    set_int(t.append(t.own_path(p, nullptr, 1), CaptureType::UInt8), 0x1E);
    document v = std::move(t).commit();

    EXPECT_EQ(read_int(child_named(field(v.root(), "h"), "n"), v.src()), 3);
    EXPECT_EQ(v.serialize(), bytes_of({0x03, 0x0A, 0x14, 0x1E}));
    EXPECT_EQ(read_int(child_named(field(d.root(), "h"), "n"), d.src()), 2);  // source intact
}

// ── Build phase: what a parser needs ────────────────────────────────────────

// A speculative parse that fails must leave no trace in the index. The cursor
// rewinds; so does this.
TEST(ZCowBuild, RestoreRewindsAFailedAttempt) {
    builder b;
    b.add_field("keep", 0, 1, CaptureType::UInt8);

    builder::Mark m = b.mark();
    b.begin_struct("attempt", 1);
      b.add_field("a", 1, 2, CaptureType::UInt8);
      b.add_field("bee", 2, 3, CaptureType::UInt8);
    b.end_struct(3);
    b.restore(m);

    b.add_field("after", 1, 2, CaptureType::UInt8);

    const uint8_t raw[2] = {1, 2};
    document d = b.finish(true, 2, raw, sizeof raw).doc;
    EXPECT_EQ(size_of(d.root()), 2u);
    EXPECT_NE(child_named(d.root(), "keep"), nullptr);
    EXPECT_NE(child_named(d.root(), "after"), nullptr);
    EXPECT_EQ(child_named(d.root(), "attempt"), nullptr);
}

TEST(ZCowBuild, RestoreRewindsAnAttemptThatLeftScopesOpen) {
    builder b;
    b.begin_struct("outer", 0);
      b.add_field("kept", 0, 1, CaptureType::UInt8);
      builder::Mark m = b.mark();
      b.begin_struct("half", 1);
        b.add_field("gone", 1, 2, CaptureType::UInt8);
      // deliberately not closed — the attempt died mid-scope
      b.restore(m);
      b.add_field("next", 1, 2, CaptureType::UInt8);
    b.end_struct(2);

    const uint8_t raw[2] = {1, 2};
    document d = b.finish(true, 2, raw, sizeof raw).doc;
    const node* outer = child_named(d.root(), "outer");
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(size_of(outer), 2u);
    EXPECT_NE(child_named(outer, "kept"), nullptr);
    EXPECT_NE(child_named(outer, "next"), nullptr);
    EXPECT_EQ(child_named(outer, "half"), nullptr);
}

TEST(ZCowBuild, CurrentScopeStartTracksTheInnermostOpenScope) {
    builder b;
    EXPECT_EQ(b.current_scope_start(), 0u);
    b.begin_struct("outer", 4);
    EXPECT_EQ(b.current_scope_start(), 4u);
    b.begin_array("inner", 9);
    EXPECT_EQ(b.current_scope_start(), 9u);
    b.end_array(11);
    EXPECT_EQ(b.current_scope_start(), 4u);
    b.end_struct(11);
    EXPECT_EQ(b.current_scope_start(), 0u);
}

// An expression may name a field in ANY enclosing scope — an element's interval
// referencing a header field parsed before the array opened.
TEST(ZCowBuild, LookupWalksOutwardThroughEnclosingScopes) {
    builder b;
    b.add_field("top", 0, 1, CaptureType::UInt8);
    b.begin_struct("hdr", 1);
      b.add_field("off", 1, 2, CaptureType::UInt8);
      b.begin_array("xs", 2);
        b.add_field("elem", 2, 3, CaptureType::UInt8);

        EXPECT_NE(b.find_field_str("elem"), nullptr);   // innermost
        EXPECT_NE(b.find_field_str("off"), nullptr);    // enclosing
        EXPECT_NE(b.find_field_str("top"), nullptr);    // top level
        EXPECT_EQ(b.find_field_str("absent"), nullptr);
      b.end_array(3);
    b.end_struct(3);
    SUCCEED();
}

TEST(ZCowBuild, LookupPrefersTheInnermostShadowingName) {
    builder b;
    b.add_field("n", 0, 1, CaptureType::UInt8);
    b.begin_struct("s", 1);
      node* inner = b.add_field("n", 1, 2, CaptureType::UInt8);
      EXPECT_EQ(b.find_field_str("n"), inner);
    b.end_struct(2);
    SUCCEED();
}

TEST(ZCowBuild, InternedLookupComparesPointersAndContentLookupComparesText) {
    static const char kName[] = "field";
    builder b;
    node* n = b.add_field(kName, 0, 1, CaptureType::UInt8);

    EXPECT_EQ(b.find_field(kName), n);                       // same pointer
    std::string copy = "field";
    EXPECT_EQ(b.find_field(copy.c_str()), nullptr);          // different pointer
    EXPECT_EQ(b.find_field_str(copy.c_str()), n);            // same content
}

TEST(ZCowBuild, ChildLookupByNameAndIndex) {
    builder b;
    b.begin_struct("s", 0);
      b.add_field("a", 0, 1, CaptureType::UInt8);
      b.add_field("b", 1, 2, CaptureType::UInt8);
    b.end_struct(2);
    b.begin_array("xs", 2);
      b.add_field(nullptr, 2, 3, CaptureType::UInt8);
      b.add_field(nullptr, 3, 4, CaptureType::UInt8);
    b.end_array(4);

    const uint8_t raw[4] = {1, 2, 3, 4};
    document d = b.finish(true, 4, raw, sizeof raw).doc;
    const node* s = child_named(d.root(), "s");
    const node* xs = child_named(d.root(), "xs");

    EXPECT_NE(b.find_child_str(s, "b"), nullptr);
    EXPECT_EQ(b.find_child_str(s, "zz"), nullptr);
    EXPECT_EQ(b.find_child_at(xs, 1), child_at(xs, 1));
    EXPECT_EQ(b.find_child_at(xs, 5), nullptr);
    EXPECT_EQ(b.find_child_at(nullptr, 0), nullptr);
}

// Armed at the switch dispatch, consumed by the arm it selects — and by that arm
// only, so a following scope does not inherit it.
TEST(ZCowBuild, PendingVariantTagIsConsumedByTheNextScope) {
    builder b;
    b.set_pending_variant_tag(3);
    b.begin_struct("arm", 0);
    b.end_struct(1);
    b.begin_struct("after", 1);
    b.end_struct(2);

    const uint8_t raw[2] = {1, 2};
    document d = b.finish(true, 2, raw, sizeof raw).doc;
    EXPECT_EQ(child_named(d.root(), "arm")->variant_tag, 3);
    EXPECT_EQ(child_named(d.root(), "after")->variant_tag, -1);
}

TEST(ZCowBuild, AFailedParseYieldsNoDocumentButKeepsTheReason) {
    builder b;
    b.add_field("a", 0, 1, CaptureType::UInt8);
    const uint8_t raw[1] = {1};
    parse_result r = b.finish(false, 3, raw, sizeof raw, "unexpected byte", 3);
    EXPECT_FALSE(r);
    EXPECT_FALSE(r.success);
    EXPECT_STREQ(r.error_message, "unexpected byte");
    EXPECT_EQ(r.error_offset, 3u);
    EXPECT_FALSE(r.doc.valid());
    EXPECT_TRUE(r.doc.serialize().empty());
}

TEST(ZCowBuild, ASuccessfulParseReportsWhatItConsumed) {
    builder b;
    b.add_field("a", 0, 1, CaptureType::UInt8);
    const uint8_t raw[2] = {1, 2};
    parse_result r = b.finish(true, 1, raw, sizeof raw);
    EXPECT_TRUE(r);
    EXPECT_EQ(r.bytes_consumed, 1u);
    EXPECT_EQ(r.error_message, nullptr);
    EXPECT_TRUE(r.doc.valid());
}

// A parser carries a produced value — span, type, computed value — before a later
// step records it. The carrier is a node that is not in the tree.
TEST(ZCowBuild, FreeNodesAreCarriedButNotInTheTree) {
    builder b;
    node* carried = b.make_free();
    ASSERT_NE(carried, nullptr);
    carried->type = CaptureType::Computed;
    carried->start_offset = carried->end_offset = 3;
    carried->computed_value = int_cv(5);

    // Recording it is a separate step; until then the index does not contain it.
    const uint8_t raw[1] = {1};
    builder::Mark m = b.mark();
    EXPECT_EQ(m.kids, 0u);

    b.add_field("recorded", carried->start_offset, carried->end_offset,
                carried->type, carried->computed_value);
    document d = b.finish(true, 1, raw, sizeof raw).doc;
    const node* rec = child_named(d.root(), "recorded");
    ASSERT_NE(rec, nullptr);
    EXPECT_NE(rec, carried);                        // a distinct index node
    EXPECT_EQ(rec->computed_value->i, 5);
}

TEST(ZCowBuild, ClearResetsEverything) {
    builder b;
    b.set_pending_variant_tag(7);
    b.begin_struct("s", 0);
    b.add_field("a", 0, 1, CaptureType::UInt8);
    b.clear();
    EXPECT_EQ(b.current_scope_start(), 0u);
    EXPECT_EQ(b.find_field_str("a"), nullptr);

    b.add_field("fresh", 0, 1, CaptureType::UInt8);
    const uint8_t raw[1] = {1};
    document d = b.finish(true, 1, raw, sizeof raw).doc;
    EXPECT_EQ(size_of(d.root()), 1u);
    EXPECT_EQ(child_named(d.root(), "fresh")->variant_tag, -1);   // tag cleared too
}
