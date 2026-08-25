#include "CTypeMapper.h"
#include "Names.h"

using namespace BBQ;

namespace bbqgen_c {

std::string CTypeMapper::to_snake(const std::string& name) {
    return bbqgen::to_snake_case(name);
}

std::string CTypeMapper::prefixed_name(const std::string& name) const {
    std::string s = bbqgen::to_snake_case(name);
    if (prefix_.empty()) return s;
    return prefix_ + "_" + s;
}

std::string CTypeMapper::type_name(const std::string& rule_name) const {
    return prefixed_name(rule_name) + "_t";
}

std::string CTypeMapper::read_func(const std::string& rule_name) const {
    return prefixed_name(rule_name) + "_read";
}

std::string CTypeMapper::write_func(const std::string& rule_name) const {
    return prefixed_name(rule_name) + "_write";
}

// ── Type mapping ──

std::string CTypeMapper::c_type(TypeExpr* type) const {
    if (auto* prim = dynamic_cast<Primitive*>(type)) {
        return primitive_c_type(prim->kind);
    } else if (auto* rr = dynamic_cast<RuleRef*>(type)) {
        return type_name(rr->name);
    } else if (auto* arr = dynamic_cast<Array*>(type)) {
        std::string elem_t = c_type(arr->element);
        return "struct { " + elem_t + "* items; size_t count; }";
    } else if (auto* opt = dynamic_cast<Optional*>(type)) {
        std::string elem_t = c_type(opt->element);
        return "struct { bool has_value; " + elem_t + " value; }";
    } else if (auto* comp = dynamic_cast<Compute*>(type)) {
        return primitive_c_type(comp->result_type);
    } else if (auto* st = dynamic_cast<Struct*>(type)) {
        std::string base = sema_.inline_struct_name(st);
        if (!base.empty())
            return prefixed_name(base) + "_t";
        return "/* anonymous struct */";
    } else if (auto* sw = dynamic_cast<Switch*>(type)) {
        std::string result = "struct { int tag; union { ";
        for (size_t i = 0; i < sw->cases.size(); i++)
            result += c_type(sw->cases[i]->target) + " case_" + std::to_string(i) + "; ";
        if (sw->default_ && !(*sw->default_)->is_reject)
            result += c_type(*(*sw->default_)->target) + " default_val; ";
        result += "} u; }";
        return result;
    } else if (auto* ext = dynamic_cast<Extern*>(type)) {
        return ext->cpp_type;
    } else if (auto* bf = dynamic_cast<Bitfield*>(type)) {
        std::string result = "struct { ";
        for (auto* entry : bf->entries)
            result += bitfield_entry_type(entry->width,
                                          entry->sign == Signedness::Signed)
                    + " " + entry->name + "; ";
        result += "}";
        return result;
    }
    return "/* unknown type */";
}

std::string CTypeMapper::primitive_c_type(PrimitiveKind* kind) const {
    if (auto* ik = dynamic_cast<IntegerKind*>(kind)) {
        std::string base = (ik->sign == Signedness::Unsigned) ? "uint" : "int";
        switch (ik->width) {
            case Width::W8:  return base + "8_t";
            case Width::W16: return base + "16_t";
            case Width::W32: return base + "32_t";
            case Width::W64: return base + "64_t";
        }
    } else if (auto* fk = dynamic_cast<FloatKind*>(kind)) {
        return (fk->width == Width::W64) ? "double" : "float";
    } else if (dynamic_cast<BytesKind*>(kind)) {
        return "bbq_bytes_t";
    } else if (dynamic_cast<StringKind*>(kind)) {
        return "bbq_string_t";
    } else if (dynamic_cast<BoolKind*>(kind)) {
        return "bool";
    }
    return "/* unknown primitive */";
}

// The smallest C integer the entry's bits fit in. A signed entry is read back
// sign-extended from its OWN width, so the field it lands in has to be signed —
// an unsigned spelling would report a 4-bit 0xD as 13 where the grammar said -3.
std::string CTypeMapper::bitfield_entry_type(int64_t width, bool is_signed) const {
    if (width <= 8)  return is_signed ? "int8_t"  : "uint8_t";
    if (width <= 16) return is_signed ? "int16_t" : "uint16_t";
    if (width <= 32) return is_signed ? "int32_t" : "uint32_t";
    return is_signed ? "int64_t" : "uint64_t";
}

}  // namespace bbqgen_c
