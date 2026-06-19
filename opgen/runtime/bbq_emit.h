/*
 * bbq_emit.h — generic codegen emit buffer + post-layout peephole for
 * opgen-VM bytecode. The writer half of the opgen bytecode ecosystem
 * (bbq_runtime.h is the reader/cursor).
 *
 * A backend's codegen — e.g. a burgc-generated matcher executing Dybvig
 * cg_jump — emits opcodes + sleb/u8 operands and branches to label
 * anchors keyed by opaque node pointers; branches resolve to absolute
 * byte PCs. finalize() runs the classic control-flow peephole suite
 * (Tanenbaum, van Staveren, Stevenson, TOPLAS 4(1):21-36, 1982) to a
 * fixpoint, then writes the final targets:
 *
 *   - jump threading       (branch chaining: goto→goto→T  ⇒  →T)
 *   - tail-jump merging     (goto whose target is a 1-byte terminator
 *                            ⇒ the terminator itself)
 *   - fallthrough elision   (goto to the next instruction ⇒ «»; this is
 *                            the DDCG paper's CG_jump L_next ⇒ «»)
 *   - branch reversal       (cond T; goto F; T:  ⇒  !cond F)
 *
 * Generic over the opcode set via the Peephole config: the GOTO opcode,
 * an optional conditional-inversion table, and an optional 1-byte-
 * terminator predicate. (Operand-width narrowing and stack scheduling
 * are opcode-set-specific and live with their consumers, not here.)
 *
 * Branch placeholders are a fixed BRANCH_WIDTH-byte padded sleb so PCs
 * stay stable through emission; the peephole edits the stream and
 * re-floats every label/fixup PC, then finalize() writes absolute sleb
 * targets.
 */
#ifndef BBQ_EMIT_H
#define BBQ_EMIT_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>

namespace bbq {

// Peephole configuration: the opcode-set knowledge the generic passes
// need. `invert`/`is_terminator` may be null to disable their pass.
struct Peephole {
    uint8_t        goto_op;                      // unconditional jump opcode
    const uint8_t* invert = nullptr;             // 256-entry op→inverted-op, or null
    bool         (*is_terminator)(uint8_t) = nullptr;  // 1-byte BB-ender, or null
};

class Emit {
public:
    // 5 bytes of padded sleb cover any non-negative PC at calc scale;
    // fixed width keeps PCs stable until the peephole edits them.
    static constexpr int BRANCH_WIDTH = 5;

    std::vector<uint8_t> code;

    size_t pc() const { return code.size(); }
    void op(uint8_t o) { code.push_back(o); }
    void u8(uint8_t v) { code.push_back(v); }

    // signed LEB128 — the immediate form the interp reads + the JIT bakes.
    void sleb(int64_t v) {
        for (;;) {
            uint8_t b = v & 0x7F; v >>= 7;
            bool last = (v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40));
            if (!last) b |= 0x80;
            code.push_back(b);
            if (last) break;
        }
    }

    // Record that label anchor `key` resolves to the current PC.
    void label(const void* key) { labels_[key] = code.size(); }
    bool has_label(const void* key) const { return labels_.count(key) != 0; }

    // Emit `o` + a BRANCH_WIDTH-byte placeholder targeting anchor `target`.
    void branch(uint8_t o, const void* target) {
        code.push_back(o);
        fixups_.push_back({code.size(), target, false});
        for (int i = 0; i < BRANCH_WIDTH; i++) code.push_back(0x80);
    }

    // Run the peephole to a fixpoint, then write absolute sleb targets.
    // Pristine destination-driven codegen rarely needs more than
    // fallthrough; the other passes are the safety net for IR that an
    // optimization pass (constant fold / DCE / GVN / code motion) has
    // perturbed before lowering — those reintroduce goto-chains,
    // goto-to-terminator, and conditional-over-goto redundancies.
    void finalize(const Peephole& cfg) {
        bool changed = true;
        for (int round = 0; changed && round < ROUND_LIMIT; round++) {
            changed = false;
            changed |= pass_thread(cfg.goto_op);
            if (cfg.is_terminator) changed |= pass_tail_jump(cfg.goto_op, cfg.is_terminator);
            changed |= pass_fallthrough(cfg.goto_op);
            if (cfg.invert) changed |= pass_invert(cfg.goto_op, cfg.invert);
        }
        backpatch();
    }

    // Resolve branches with no peephole (for tests / debugging).
    void backpatch_only() { backpatch(); }

private:
    struct Fixup { size_t pos; const void* target; bool dead; };
    std::vector<Fixup> fixups_;
    std::unordered_map<const void*, size_t> labels_;

    static constexpr int ROUND_LIMIT = 16;

    // pos(operand-start) → live-fixup index, for O(1) "what branches at PC?".
    std::unordered_map<size_t, size_t> fixup_index() const {
        std::unordered_map<size_t, size_t> m;
        for (size_t i = 0; i < fixups_.size(); i++)
            if (!fixups_[i].dead) m[fixups_[i].pos] = i;
        return m;
    }

    // ── Branch chaining: retarget a branch through goto→goto chains. ──
    bool pass_thread(uint8_t goto_op) {
        bool changed = false;
        auto idx = fixup_index();
        for (auto& f : fixups_) {
            if (f.dead) continue;
            const void* nt = f.target;
            for (int hops = 0; hops < 8; hops++) {
                auto lit = labels_.find(nt);
                if (lit == labels_.end()) break;
                size_t tpc = lit->second;
                if (tpc >= code.size() || code[tpc] != goto_op) break;
                auto git = idx.find(tpc + 1);       // the goto's operand pos
                if (git == idx.end()) break;
                const void* hop = fixups_[git->second].target;
                if (hop == nt) break;               // self-loop guard
                nt = hop;
            }
            if (nt != f.target) { f.target = nt; changed = true; }
        }
        return changed;
    }

    // ── Tail-jump merging: goto → (1-byte terminator) ⇒ the terminator. ──
    bool pass_tail_jump(uint8_t goto_op, bool (*is_term)(uint8_t)) {
        bool changed = false;
        for (int i = (int)fixups_.size() - 1; i >= 0; i--) {
            Fixup& f = fixups_[i];
            if (f.dead) continue;
            size_t op_pc = f.pos - 1;
            if (code[op_pc] != goto_op) continue;
            size_t tpc = labels_.at(f.target);
            if (tpc >= code.size() || !is_term(code[tpc])) continue;
            code[op_pc] = code[tpc];                // become the terminator
            remove_bytes(f.pos, BRANCH_WIDTH);      // drop the operand
            f.dead = true; changed = true;
        }
        return changed;
    }

    // ── Fallthrough elision: goto to the next instruction ⇒ nothing. ──
    bool pass_fallthrough(uint8_t goto_op) {
        bool changed = false;
        for (int i = (int)fixups_.size() - 1; i >= 0; i--) {
            Fixup& f = fixups_[i];
            if (f.dead) continue;
            size_t op_pc = f.pos - 1;
            if (code[op_pc] != goto_op) continue;
            if (labels_.at(f.target) != f.pos + BRANCH_WIDTH) continue;
            remove_bytes(op_pc, 1 + BRANCH_WIDTH);
            f.dead = true; changed = true;
        }
        return changed;
    }

    // ── Branch reversal: `cond T; goto F; T:` ⇒ `!cond F`. ──
    bool pass_invert(uint8_t goto_op, const uint8_t* invert) {
        bool changed = false;
        auto idx = fixup_index();
        for (size_t i = 0; i < fixups_.size(); i++) {
            Fixup& f = fixups_[i];
            if (f.dead) continue;
            size_t cond_pc = f.pos - 1;
            uint8_t cond_op = code[cond_pc];
            if (!invert[cond_op]) continue;
            size_t goto_op_pc = f.pos + BRANCH_WIDTH;       // right after cond's operand
            if (goto_op_pc >= code.size() || code[goto_op_pc] != goto_op) continue;
            auto git = idx.find(goto_op_pc + 1);
            if (git == idx.end()) continue;
            Fixup& gf = fixups_[git->second];
            // cond's target must be the instruction right after the goto.
            if (labels_.at(f.target) != goto_op_pc + 1 + BRANCH_WIDTH) continue;
            code[cond_pc] = invert[cond_op];                // invert
            f.target = gf.target;                           // take the goto's target
            remove_bytes(goto_op_pc, 1 + BRANCH_WIDTH);     // delete the goto
            gf.dead = true; changed = true;
            idx = fixup_index();                            // positions shifted
        }
        return changed;
    }

    // Erase n bytes at `at` and re-float every label/fixup PC past it.
    void remove_bytes(size_t at, size_t n) {
        code.erase(code.begin() + at, code.begin() + at + n);
        for (auto& kv : labels_) if (kv.second > at) kv.second -= n;
        for (auto& f : fixups_) if (!f.dead && f.pos > at) f.pos -= n;
    }

    void backpatch() {
        for (auto& f : fixups_) {
            if (f.dead) continue;
            uint64_t v = (uint64_t)labels_.at(f.target);
            for (int i = 0; i < BRANCH_WIDTH; i++) {
                uint8_t b = (uint8_t)((v >> (7 * i)) & 0x7F);
                if (i < BRANCH_WIDTH - 1) b |= 0x80;        // canonical padded sleb
                code[f.pos + i] = b;
            }
        }
    }
};

} // namespace bbq

#endif
