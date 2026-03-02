#ifndef ASDL_STRING_UTILS_H
#define ASDL_STRING_UTILS_H

#include <string>

std::string CamelCase(const std::string& in);
std::string SnakeCase(const std::string& in);
std::string get_safe_name(const std::string& name);

#endif // ASDL_STRING_UTILS_H
