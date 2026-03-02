#include "string_utils.h"
#include <unordered_set>
#include <cctype>

static const std::unordered_set<std::string> reserved_keywords = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
    "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const", "consteval", "constexpr", "const_cast",
    "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
    "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
    "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
    "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator",
    "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
    "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
    "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
};

std::string CamelCase(const std::string& in) {
    if (in.empty()) {
        return in;
    }

    std::string str(1, std::toupper(in[0]));

    for (auto it = in.begin() + 1; it != in.end(); ) {
        if (*it == '_' && ++it != in.end()) {
            str += std::toupper(*it);
            it++;
        } else {
            str += *it;
            it++;
        }
    }

    return str;
}

std::string SnakeCase(const std::string& in) {
    if (in.empty()) {
        return in;
    }

    std::string result;
    result.reserve(in.length() * 2);

    result += std::tolower(in[0]);

    for (size_t i = 1; i < in.length(); ++i) {
        bool addUnderscore = false;

        if (std::isupper(in[i]) && std::islower(in[i-1])) {
            addUnderscore = true;
        } else if (std::isalpha(in[i]) && std::isdigit(in[i-1])) {
            addUnderscore = true;
        }

        if (addUnderscore) {
            result += '_';
        }
        result += std::tolower(in[i]);
    }

    return result;
}

std::string get_safe_name(const std::string& name) {
    if (reserved_keywords.count(name)) {
        return name + "_";
    }
    return name;
}
