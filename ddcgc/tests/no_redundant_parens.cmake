# Fails if the generated C contains `if ((`.
#
# clang's -Wparentheses-equality reads `if ((a == b))` as an equality
# that was meant to be an assignment and rejects it under -Werror. gcc
# has no equivalent, so the -Werror e2e targets cannot catch it — a
# clang-built consumer is the first thing to break, which is exactly
# how this reached a downstream project twice.
#
# Driven off the emitted text so the check does not depend on which
# compiler CMake selected.

if(NOT DEFINED GENERATED)
    message(FATAL_ERROR "no_redundant_parens.cmake: -DGENERATED=<file> required")
endif()
if(NOT EXISTS "${GENERATED}")
    message(FATAL_ERROR "no_redundant_parens.cmake: ${GENERATED} does not exist")
endif()

# `if ((` alone is not enough: a JOIN legitimately parenthesises each
# operand, so `if ((!f(x)) && g(y))` opens with the same two characters
# and is correct. The redundant shape is a LONE condition, which has no
# `&&` or `||` — so those lines are excluded rather than matched more
# cleverly, since a regex cannot balance parentheses.
file(STRINGS "${GENERATED}" CANDIDATES REGEX "if \\(\\(")
set(OFFENDING "")
foreach(line IN LISTS CANDIDATES)
    if(NOT line MATCHES "&&|\\|\\|")
        list(APPEND OFFENDING "${line}")
    endif()
endforeach()
list(LENGTH OFFENDING N)
if(N GREATER 0)
    string(REPLACE ";" "\n  " PRETTY "${OFFENDING}")
    message(FATAL_ERROR
        "${GENERATED}: ${N} redundantly parenthesised condition(s).\n"
        "  ${PRETTY}\n"
        "A lone check already sits inside the `if (...)`; parenthesise "
        "only when joining several with &&.")
endif()
