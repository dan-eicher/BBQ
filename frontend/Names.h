#pragma once
#include <string>

namespace bbqgen {

// CamelCase/PascalCase rule name -> snake_case C identifier base. THE one
// canonical name conversion, lives in the frontend so every backend (the C
// type/reader/writer emitters, the render backend, the future type renderer)
// produces identical names — and so it survives any one backend's retirement.
//
// Known wart: acronym+digit runs aren't grouped ("IPv4Header" -> "i_pv4_header"
// rather than "ipv4_header"). Fixing it here fixes every backend at once.
std::string to_snake_case(const std::string& name);

}  // namespace bbqgen
