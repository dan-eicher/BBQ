#ifndef ASDL_TYPE_REGISTRY_H
#define ASDL_TYPE_REGISTRY_H

#include <map>
#include <set>
#include <string>

class TypeRegistry {
public:
    TypeRegistry();

    void set_lang_c();   // Remap base types for C output
    void register_type(const std::string& asdl_type, const std::string& target_type);
    void register_alias(const std::string& asdl_type, const std::string& target_type);
    std::string get_type(const std::string& asdl_type) const;
    bool is_base_type(const std::string& type) const;
    bool has_type(const std::string& type) const;

private:
    std::map<std::string, std::string> types_;
    std::set<std::string> aliases_;  // Types that are aliases (treated as base types)
};

#endif // ASDL_TYPE_REGISTRY_H
