#pragma once
// Generated from asdl.asdl — do not edit

#include <string>
#include <vector>

namespace asdl_ast
{

struct SourceLoc {
    const char* file = nullptr;
    int line = 0;
    int col = 0;
};

// ── Forward declarations ──────────────────────────────────────
struct Module;
struct TypeAlias;
struct Definition;
struct TypeBody;
struct Product;
struct Sum;
struct Constructor;
struct Field;

// ── Enum types ────────────────────────────────────────────────
enum class Flag {
    None,
    Optional,
    Sequence
};

// ── Base node ─────────────────────────────────────────────────
struct ASTNode {
    SourceLoc loc;
    virtual ~ASTNode() = default;
};

// ── Sum type bases ────────────────────────────────────────────
struct TypeBody : public ASTNode {
    virtual ~TypeBody() = default;
};


// ── Concrete types ────────────────────────────────────────────
struct Module : public ASTNode {
    Module(std::string name, std::vector<TypeAlias*> aliases, std::vector<Definition*> definitions)
        : name(std::move(name)), aliases(std::move(aliases)), definitions(std::move(definitions)) {}

    std::string name;
    std::vector<TypeAlias*> aliases;
    std::vector<Definition*> definitions;
};

struct TypeAlias : public ASTNode {
    TypeAlias(std::string name, std::string ctype)
        : name(std::move(name)), ctype(std::move(ctype)) {}

    std::string name;
    std::string ctype;
};

struct Definition : public ASTNode {
    Definition(std::string name, TypeBody* body)
        : name(std::move(name)), body(std::move(body)) {}

    std::string name;
    TypeBody* body;
};

struct Product : public TypeBody {
    Product(std::vector<Field*> fields)
        : fields(std::move(fields)) {}

    std::vector<Field*> fields;
};

struct Sum : public TypeBody {
    Sum(std::vector<Constructor*> types, std::vector<Field*> attributes)
        : types(std::move(types)), attributes(std::move(attributes)) {}

    std::vector<Constructor*> types;
    std::vector<Field*> attributes;
};

struct Constructor : public ASTNode {
    Constructor(std::string name, std::vector<Field*> fields)
        : name(std::move(name)), fields(std::move(fields)) {}

    std::string name;
    std::vector<Field*> fields;
};

struct Field : public ASTNode {
    Field(std::string type_name, Flag flag, std::string name)
        : type_name(std::move(type_name)), flag(std::move(flag)), name(std::move(name)) {}

    std::string type_name;
    Flag flag;
    std::string name;
};


} // namespace asdl_ast
