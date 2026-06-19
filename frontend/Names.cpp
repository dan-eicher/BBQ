#include "Names.h"
#include <cctype>

namespace bbqgen {

std::string to_snake_case(const std::string& name) {
    std::string result;
    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        if (std::isupper((unsigned char)c)) {
            // Insert _ before an uppercase that starts a new word:
            //  - preceded by lowercase ("fooBar" -> "foo_bar") or a digit,
            //  - or an acronym's last capital before a lowercase ("ABCDef" -> "abc_def").
            if (i > 0) {
                char prev = name[i - 1];
                if (std::islower((unsigned char)prev) || std::isdigit((unsigned char)prev))
                    result += '_';
                else if (std::isupper((unsigned char)prev) && i + 1 < name.size() &&
                         std::islower((unsigned char)name[i + 1]))
                    result += '_';
            }
            result += (char)std::tolower((unsigned char)c);
        } else {
            result += c;
        }
    }
    return result;
}

}  // namespace bbqgen
