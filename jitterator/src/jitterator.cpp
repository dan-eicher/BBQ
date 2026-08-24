// jitterator — Copy-and-Patch stencil extractor
//
// Usage: jitterator <stencils.o> [-o <stencil_table.h>]
//
// Reads an ELF64 relocatable object file, finds all global functions
// (stencils) and their _HOLE_ relocations, and generates stencil_table.h.

#include "elf64Types.h"
#include "elf64Reader.h"
#include "bbq_reader.h"   // bbq::ParseArena
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// ── ELF constants ──────────────────────────────────────────────

static constexpr uint32_t SHT_PROGBITS = 1;
static constexpr uint32_t SHT_SYMTAB   = 2;
static constexpr uint32_t SHT_RELA     = 4;

static constexpr uint8_t STB_LOCAL  = 0;
static constexpr uint8_t STB_GLOBAL = 1;

static constexpr uint8_t STT_FUNC = 2;

// x86-64 relocation types we care about
static constexpr uint32_t R_X86_64_64    = 1;   // 64-bit absolute
static constexpr uint32_t R_X86_64_PC32  = 2;   // 32-bit PC-relative
static constexpr uint32_t R_X86_64_32S   = 11;  // 32-bit signed absolute
static constexpr uint32_t R_X86_64_PLT32 = 4;   // 32-bit PLT-relative (treat as PC32)

// ── Data structures ────────────────────────────────────────────

struct RelaEntry {
    uint64_t offset;       // offset within .text
    uint32_t type;         // ELF relocation type
    std::string sym_name;  // referenced symbol name
    int64_t addend;
};

struct StencilInfo {
    std::string name;
    uint64_t offset;       // offset within .text
    uint64_t size;
    std::vector<const uint8_t*> code;  // pointer into file data
};

enum PatchType {
    PATCH_ABS64,       // 64-bit absolute (value written directly)
    PATCH_ABS32S,      // 32-bit signed absolute
    PATCH_REL_BRANCH,  // 32-bit PC-relative branch (value = target code address)
    PATCH_REL_DATA,    // 32-bit PC-relative data load (value stored in constant pool)
};

struct PatchInfo {
    uint32_t offset;       // byte offset within stencil
    PatchType type;
    uint16_t hole_index;   // index into per-stencil hole list
};

struct Stencil {
    std::string name;
    std::vector<uint8_t> code;
    std::vector<PatchInfo> patches;
    std::vector<std::string> hole_names;  // unique holes, indexed by hole_index
    uint16_t data_hole_count = 0;         // holes needing constant pool entries
};

// ── ELF parsing ────────────────────────────────────────────────

static const char* elf_string(const uint8_t* data, size_t data_size,
                              elf::Elf64 e, uint32_t sh_link, uint32_t name_idx) {
    if (sh_link >= e.shdrs().size()) return "";
    size_t off = (size_t)e.shdrs()[sh_link].sh_offset() + name_idx;
    if (off >= data_size) return "";
    return reinterpret_cast<const char*>(data + off);
}

struct ElfInfo {
    std::vector<StencilInfo> stencils;
    size_t text_offset = 0;
    size_t text_size = 0;
    size_t text_shndx = 0;
    std::vector<RelaEntry> text_relas;
    std::string error;
};

static ElfInfo parse_elf(const uint8_t* data, size_t size) {
    ElfInfo info;
    bbq::ParseArena arena;      // owns the capture index; outlives the handles below
    bbq::zcow::parse_result r = elf::Elf64_read(data, size, arena);
    if (!r.success) {
        info.error = "ELF64 parse failed";
        return info;
    }
    elf::Elf64 e = elf::Elf64_of(r.doc);   // zero-copy; the arena above holds the index
    auto ehdr = e.ehdr();
    auto shdrs = e.shdrs();

    // Find section name string table
    const char* shstrtab = nullptr;
    size_t shstrtab_size = 0;
    if (ehdr.e_shstrndx() < shdrs.size()) {
        auto ss = shdrs[ehdr.e_shstrndx()];
        if (ss.sh_offset() + ss.sh_size() <= size) {
            shstrtab = reinterpret_cast<const char*>(data + ss.sh_offset());
            shstrtab_size = ss.sh_size();
        }
    }

    // Find .text section
    for (size_t i = 0; i < shdrs.size(); i++) {
        auto s = shdrs[i];
        if (s.sh_type() == SHT_PROGBITS && shstrtab && s.sh_name() < shstrtab_size) {
            if (strcmp(shstrtab + s.sh_name(), ".text") == 0) {
                info.text_offset = s.sh_offset();
                info.text_size = s.sh_size();
                info.text_shndx = i;
                break;
            }
        }
    }

    if (info.text_size == 0) {
        info.error = "no .text section found";
        return info;
    }

    // Find all global function symbols in .text. Each symbol-table entry is
    // parsed as a standalone Elf64_Sym at its computed offset — the render
    // backend exposes a parse entry per rule, so no manual interval is needed.
    for (size_t si = 0; si < shdrs.size(); si++) {
        auto s = shdrs[si];
        if (s.sh_type() != SHT_SYMTAB) continue;

        uint64_t entsize = s.sh_entsize();
        size_t sym_count = entsize ? (size_t)(s.sh_size() / entsize) : 0;
        for (size_t j = 0; j < sym_count; j++) {
            size_t sym_off = (size_t)s.sh_offset() + j * entsize;
            if (sym_off + entsize > size) continue;

            bbq::ParseArena sym_arena;
            bbq::zcow::parse_result sr = elf::Elf64_Sym_read(data + sym_off, size - sym_off, sym_arena);
            if (!sr.success) continue;
            elf::Elf64_Sym sym = elf::Elf64_Sym_of(sr.doc);

            // Global function symbols in .text
            if (sym.st_bind() < STB_GLOBAL) continue;
            if (sym.st_type() != STT_FUNC) continue;
            if (sym.st_shndx() != info.text_shndx) continue;

            const char* name = elf_string(data, size, e, s.sh_link(), sym.st_name());
            if (name[0] == '\0') continue;

            StencilInfo si_info;
            si_info.name = name;
            si_info.offset = sym.st_value();
            si_info.size = sym.st_size();
            info.stencils.push_back(si_info);
        }
    }

    // Sort stencils by offset for deterministic output
    std::sort(info.stencils.begin(), info.stencils.end(),
              [](const StencilInfo& a, const StencilInfo& b) {
                  return a.offset < b.offset;
              });

    // Parse .rela.text relocations
    for (size_t si = 0; si < shdrs.size(); si++) {
        auto s = shdrs[si];
        if (s.sh_type() != SHT_RELA) continue;
        if (s.sh_info() != info.text_shndx) continue;

        if (s.sh_link() >= shdrs.size()) continue;
        auto symtab = shdrs[s.sh_link()];

        uint64_t entsize = s.sh_entsize();
        size_t rela_count = entsize ? (size_t)(s.sh_size() / entsize) : 0;
        for (size_t j = 0; j < rela_count; j++) {
            size_t rela_off = (size_t)s.sh_offset() + j * entsize;
            if (rela_off + entsize > size) continue;

            bbq::ParseArena rela_arena;
            bbq::zcow::parse_result rr = elf::Elf64_Rela_read(data + rela_off, size - rela_off, rela_arena);
            if (!rr.success) continue;
            elf::Elf64_Rela rela = elf::Elf64_Rela_of(rr.doc);

            // Resolve symbol name
            uint32_t sym_idx = rela.r_sym();
            uint64_t st_entsize = symtab.sh_entsize();
            size_t sym_count = st_entsize ? (size_t)(symtab.sh_size() / st_entsize) : 0;
            if (sym_idx >= sym_count) continue;

            size_t sym_off = (size_t)symtab.sh_offset() + sym_idx * st_entsize;
            if (sym_off + st_entsize > size) continue;

            bbq::ParseArena sym_arena;
            bbq::zcow::parse_result sr = elf::Elf64_Sym_read(data + sym_off, size - sym_off, sym_arena);
            if (!sr.success) continue;
            elf::Elf64_Sym sym = elf::Elf64_Sym_of(sr.doc);

            const char* name = elf_string(data, size, e, symtab.sh_link(), sym.st_name());

            RelaEntry re;
            re.offset = rela.r_offset();
            re.type = rela.r_type();
            re.sym_name = name;
            re.addend = rela.r_addend();
            info.text_relas.push_back(re);
        }
    }

    return info;
}

// ── Stencil extraction ─────────────────────────────────────────

static PatchType reloc_to_patch_type(uint32_t r_type) {
    switch (r_type) {
    case R_X86_64_64:    return PATCH_ABS64;
    case R_X86_64_32S:   return PATCH_ABS32S;
    case R_X86_64_PLT32: return PATCH_REL_BRANCH;  // jmp/call targets
    case R_X86_64_PC32:  return PATCH_REL_DATA;     // RIP-relative data loads
    default:             return PATCH_REL_DATA;
    }
}

static const char* patch_type_name(PatchType t) {
    switch (t) {
    case PATCH_ABS64:      return "PATCH_ABS64";
    case PATCH_ABS32S:     return "PATCH_ABS32S";
    case PATCH_REL_BRANCH: return "PATCH_REL_BRANCH";
    case PATCH_REL_DATA:   return "PATCH_REL_DATA";
    }
    return "PATCH_REL_DATA";
}

static std::vector<Stencil> extract_stencils(const uint8_t* data, const ElfInfo& info) {
    std::vector<Stencil> stencils;
    const uint8_t* text = data + info.text_offset;
    bool had_unnamed = false;

    for (auto& si : info.stencils) {
        Stencil s;
        s.name = si.name;

        // Copy code bytes
        if (si.offset + si.size <= info.text_size) {
            s.code.assign(text + si.offset, text + si.offset + si.size);
        }

        // Find relocations within this stencil's range that target _HOLE_ symbols
        for (auto& rela : info.text_relas) {
            if (rela.offset < si.offset) continue;
            if (rela.offset >= si.offset + si.size) continue;

            // A stencil may reference ONLY named _HOLE_ symbols (the driver plugs a
            // per-instance value: operand, continuation, native pointer). Any other
            // relocation — typically a compiler-emitted .rodata constant (a float /
            // vector literal) — cannot be copy-and-patched: its RIP-relative load
            // would dangle, and pooling it by hand reinvents the linker (alignment,
            // size). The fix is to move that op into a native, where clang and the
            // real linker lay the constant out correctly. Fail loudly here.
            if (rela.sym_name.rfind("_HOLE_", 0) != 0) {
                fprintf(stderr,
                    "jitterator: stencil '%s': non-_HOLE_ relocation against '%s' "
                    "at offset %u — a compiler constant. Move this op into a native.\n",
                    si.name.c_str(), rela.sym_name.c_str(),
                    (unsigned)(rela.offset - si.offset));
                had_unnamed = true;
                continue;
            }

            // Find or create hole index for this symbol
            uint16_t hole_idx = 0;
            auto it = std::find(s.hole_names.begin(), s.hole_names.end(), rela.sym_name);
            if (it != s.hole_names.end()) {
                hole_idx = static_cast<uint16_t>(it - s.hole_names.begin());
            } else {
                hole_idx = static_cast<uint16_t>(s.hole_names.size());
                s.hole_names.push_back(rela.sym_name);
            }

            PatchInfo p;
            p.offset = static_cast<uint32_t>(rela.offset - si.offset);
            p.type = reloc_to_patch_type(rela.type);
            p.hole_index = hole_idx;
            s.patches.push_back(p);
        }

        // Count data holes (need constant pool entries)
        // A data hole is any unique hole that has at least one PATCH_REL_DATA reference
        std::vector<bool> is_data(s.hole_names.size(), false);
        for (auto& p : s.patches) {
            if (p.type == PATCH_REL_DATA) {
                is_data[p.hole_index] = true;
            }
        }
        s.data_hole_count = 0;
        for (bool d : is_data) {
            if (d) s.data_hole_count++;
        }

        stencils.push_back(std::move(s));
    }

    if (had_unnamed) {
        fprintf(stderr, "jitterator: aborting — one or more stencils reference "
                        "non-_HOLE_ symbols (see above).\n");
        exit(1);
    }
    return stencils;
}

// ── Code generation ────────────────────────────────────────────

static std::string to_upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return r;
}

static std::string render_stencil_table(const std::vector<Stencil>& stencils) {
    std::string out;
    out += "// stencil_table.h — generated by jitterator. DO NOT EDIT.\n";
    out += "#pragma once\n";
    out += "#include <stdint.h>\n";
    out += "#include <stddef.h>  /* NULL */\n\n";

    // Stencil ID enum
    out += "typedef enum {\n";
    for (size_t i = 0; i < stencils.size(); i++) {
        out += "    STENCIL_" + to_upper(stencils[i].name) + " = " +
               std::to_string(i) + ",\n";
    }
    out += "    STENCIL_COUNT = " + std::to_string(stencils.size()) + "\n";
    out += "} StencilId;\n\n";

    // Patch type enum
    out += "typedef enum {\n";
    out += "    PATCH_ABS64,\n";
    out += "    PATCH_ABS32S,\n";
    out += "    PATCH_REL_BRANCH,  // 32-bit PC-relative branch (value = target address)\n";
    out += "    PATCH_REL_DATA,    // 32-bit PC-relative data load (needs constant pool)\n";
    out += "} PatchType;\n\n";

    // Patch entry struct
    // A patch names its hole by INDEX; the hole's name lives once in the
    // stencil's hole_names[]. A per-patch string here would be a pointer and
    // a load-time relocation per entry that no consumer reads — patching goes
    // by hole_index, and the name lookups go through hole_names[].
    out += "typedef struct {\n";
    out += "    uint32_t offset;\n";
    out += "    PatchType type;\n";
    out += "    uint16_t hole_index;\n";
    out += "} PatchEntry;\n\n";

    // Stencil def struct
    out += "typedef struct {\n";
    out += "    const uint8_t *code;\n";
    out += "    uint32_t code_size;\n";
    out += "    const PatchEntry *patches;\n";
    out += "    uint32_t patch_count;\n";
    out += "    uint16_t hole_count;\n";
    out += "    uint16_t data_hole_count;  // holes needing constant pool (8 bytes each)\n";
    out += "    const char **hole_names;\n";
    // The holes a stitcher resolves by NAME on every instruction it stamps. The
    // mapping is fixed when this table is generated, so it is emitted rather than
    // searched for: a driver asking `find_hole(def, \"_HOLE_ip\")` per instruction
    // is a string comparison per instruction, per function, per module load.
    // -1 means this stencil has no such hole.
    out += "    int16_t h_cont, h_ip, h_pc, h_trap, h_resync, h_memidx, h_offmap, h_codelen;\n";
    out += "} StencilDef;\n\n";

    // Per-stencil code bytes and patch arrays
    for (auto& s : stencils) {
        std::string upper = to_upper(s.name);

        // Code bytes
        out += "static const uint8_t code_" + s.name + "[] = {";
        for (size_t i = 0; i < s.code.size(); i++) {
            if (i % 16 == 0) out += "\n    ";
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%02x,", s.code[i]);
            out += buf;
        }
        out += "\n};\n\n";

        // Patch entries
        if (!s.patches.empty()) {
            out += "static const PatchEntry patches_" + s.name + "[] = {\n";
            for (auto& p : s.patches) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "    {%u, %s, %u},\n",
                         p.offset, patch_type_name(p.type),
                         p.hole_index);
                out += buf;
            }
            out += "};\n\n";
        }

        // Hole names
        if (!s.hole_names.empty()) {
            out += "static const char *holes_" + s.name + "[] = {";
            for (auto& h : s.hole_names) {
                out += "\"" + h + "\", ";
            }
            out += "};\n\n";
        }
    }

    // Stencil table
    auto hole_index = [](const Stencil& s, const char* want) -> int {
        for (size_t i = 0; i < s.hole_names.size(); i++)
            if (s.hole_names[i] == want) return (int)i;
        return -1;
    };
    out += "static const StencilDef stencil_table[STENCIL_COUNT] = {\n";
    for (auto& s : stencils) {
        std::string upper = to_upper(s.name);
        char buf[640];
        snprintf(buf, sizeof(buf),
                 "    [STENCIL_%s] = {code_%s, %zu, %s, %zu, %zu, %u, %s, %d,%d,%d,%d,%d,%d,%d,%d},\n",
                 upper.c_str(), s.name.c_str(), s.code.size(),
                 s.patches.empty() ? "NULL" : ("patches_" + s.name).c_str(),
                 s.patches.size(),
                 s.hole_names.size(),
                 s.data_hole_count,
                 s.hole_names.empty() ? "NULL" : ("holes_" + s.name).c_str(),
                 hole_index(s, "_HOLE_cont"),   hole_index(s, "_HOLE_ip"),
                 hole_index(s, "_HOLE_pc"),     hole_index(s, "_HOLE_trap"),
                 hole_index(s, "_HOLE_resync"), hole_index(s, "_HOLE_memidx"),
                 hole_index(s, "_HOLE_offmap"), hole_index(s, "_HOLE_codelen"));
        out += buf;
    }
    out += "};\n";

    return out;
}

// ── Main ───────────────────────────────────────────────────────

static std::vector<uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    const char* input = nullptr;
    const char* output = nullptr;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (!input) {
            input = argv[i];
        }
    }

    if (!input) {
        fprintf(stderr, "Usage: jitterator <stencils.o> [-o stencil_table.h]\n");
        return 1;
    }

    auto data = read_file(input);
    if (data.empty()) {
        fprintf(stderr, "Cannot read %s\n", input);
        return 1;
    }

    auto info = parse_elf(data.data(), data.size());
    if (!info.error.empty()) {
        fprintf(stderr, "ELF parse error: %s\n", info.error.c_str());
        return 1;
    }

    if (info.stencils.empty()) {
        fprintf(stderr, "No global function symbols found in %s\n", input);
        return 1;
    }

    auto stencils = extract_stencils(data.data(), info);

    fprintf(stderr, "Extracted %zu stencils:\n", stencils.size());
    for (auto& s : stencils) {
        fprintf(stderr, "  %-24s %4zu bytes, %zu patches, %zu holes\n",
                s.name.c_str(), s.code.size(), s.patches.size(), s.hole_names.size());
    }

    auto table = render_stencil_table(stencils);

    if (output) {
        std::ofstream out(output);
        if (!out) { fprintf(stderr, "Cannot write %s\n", output); return 1; }
        out << table;
        fprintf(stderr, "Wrote %s\n", output);
    } else {
        printf("%s", table.c_str());
    }

    return 0;
}
