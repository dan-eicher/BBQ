# Jitterator — Copy-and-Patch stencil extractor

add_executable(jitterator
    src/jitterator.cpp
    extract/elf64Parser.cpp
)
target_include_directories(jitterator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extract
    ${CMAKE_SOURCE_DIR}/backends/cpp/runtime  # BBQContext.h
)

# --- Calculator proof-of-life example ---
add_executable(calc_example
    example/calc.cpp
)
target_include_directories(calc_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/example
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
)

# --- Calculator JIT (full toolchain demo: pegc + asdl + burgc + jitterator) ---

# Compile calc_stencils.c → calc_stencils.o with the preserve_none /
# musttail attributes intact, then run jitterator on it to extract
# bytecode + patch info into calc_stencil_table.h. The header is
# checked into source so calc_jit.cpp / compiler.h can include it.
set(CALC_STENCILS_O ${CMAKE_CURRENT_BINARY_DIR}/calc_stencils.o)
set(CALC_STENCIL_TABLE ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_stencil_table.h)
find_program(CLANG_C_COMPILER NAMES clang)
if(NOT CLANG_C_COMPILER)
    message(FATAL_ERROR "clang required to compile calc_stencils.c "
                        "(preserve_none + musttail attributes are clang-specific)")
endif()
add_custom_command(
    OUTPUT ${CALC_STENCILS_O}
    COMMAND ${CLANG_C_COMPILER} -O2 -fno-stack-protector -fno-asynchronous-unwind-tables
            -c ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_stencils.c
            -o ${CALC_STENCILS_O}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/example/calc_stencils.c
    COMMENT "compile calc_stencils.c -> calc_stencils.o"
    VERBATIM
)
add_custom_command(
    OUTPUT ${CALC_STENCIL_TABLE}
    COMMAND jitterator ${CALC_STENCILS_O} -o ${CALC_STENCIL_TABLE}
    DEPENDS jitterator ${CALC_STENCILS_O}
    COMMENT "jitterator: calc_stencils.o -> calc_stencil_table.h"
    VERBATIM
)

# ASDL JSON sidecars (consumed by ddcgc + burgc) + asdl-generated
# C++ headers (CalcAST.h, CalcIR.h). The headers are checked into
# source so calc_jit.cpp can include them directly; CMake regens them
# whenever the .asdl changes.
set(CALC_IR_JSON ${CMAKE_CURRENT_BINARY_DIR}/calc_ir.json)
set(CALC_IR_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/CalcIR.h)
add_custom_command(
    OUTPUT ${CALC_IR_JSON} ${CALC_IR_HEADER}
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc_ir.asdl
                 --json ${CALC_IR_JSON}
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc_ir.asdl
                 -t ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/templates/calc_ast.inja
                 -o ${CALC_IR_HEADER}
    DEPENDS asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc_ir.asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/templates/calc_ast.inja
    COMMENT "asdl: calc_ir.asdl -> calc_ir.json + CalcIR.h"
    VERBATIM
)
set(CALC_AST_JSON ${CMAKE_CURRENT_BINARY_DIR}/calc.json)
set(CALC_AST_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/CalcAST.h)
add_custom_command(
    OUTPUT ${CALC_AST_JSON} ${CALC_AST_HEADER}
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.asdl
                 --json ${CALC_AST_JSON}
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.asdl
                 -t ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/templates/calc_ast.inja
                 -o ${CALC_AST_HEADER}
    DEPENDS asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/templates/calc_ast.inja
    COMMENT "asdl: calc.asdl -> calc.json + CalcAST.h"
    VERBATIM
)

# ddcgc generates calc_compile.h from calc.ddcg + the two ASDL JSONs.
# Replaces the hand-written `Compiler` class that used to live in
# example/compiler.h. Output is `class calc::Compiler` with inline
# method bodies.
set(CALC_COMPILE_H ${CMAKE_CURRENT_BINARY_DIR}/calc_compile.h)
set(DDCGC_LIB_DIR ${CMAKE_SOURCE_DIR}/ddcgc/lib)
add_custom_command(
    OUTPUT ${CALC_COMPILE_H}
    COMMAND ddcgc -i ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.ddcg
                  -ast ${CALC_AST_JSON}
                  -ir  ${CALC_IR_JSON}
                  -L   ${DDCGC_LIB_DIR}
                  -o   ${CALC_COMPILE_H}
    DEPENDS ddcgc
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.ddcg
            ${DDCGC_LIB_DIR}/dybvig.ddcg
            ${CALC_AST_JSON}
            ${CALC_IR_JSON}
    COMMENT "Running ddcgc on calc.ddcg"
    VERBATIM
)

# End-to-end pipeline check: run burgc completeness analysis on
# calc.burg with the ASDL JSON. Demonstrates the asdl→burgc flow.
# calc is slot-based, so the schema falls back to heuristic for
# multi-arity terminals; we don't pass --strict here for that
# reason. AST-shaped IRs (yoctojc/sir.asdl) get precise filtering.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/CalcMatcher.h
    COMMAND burgc -i ${CMAKE_CURRENT_SOURCE_DIR}/example/calc.burg
                  -asdl ${CALC_IR_JSON}
                  -frames ${CMAKE_SOURCE_DIR}/burgc/frames
                  -o ${CMAKE_CURRENT_BINARY_DIR}/CalcMatcher.h
    DEPENDS burgc ${CMAKE_CURRENT_SOURCE_DIR}/example/calc.burg ${CALC_IR_JSON}
    COMMENT "burgc: generating CalcMatcher.h from calc.burg"
    VERBATIM
)

# --- Auto-regenerate Parser.h/.cpp from calc.peg ---
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/Parser.h
           ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/Parser.cpp
    COMMAND pegc ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.peg
                 -frames ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/frames/
                 -o ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend/
                 -prefix ""
    DEPENDS pegc
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/calc.peg
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/frames/Parser.frame
            ${CMAKE_CURRENT_SOURCE_DIR}/example/grammar/frames/ParserImpl.frame
    COMMENT "Generating calc Parser.h/.cpp from calc.peg"
    VERBATIM
)

# calc_runner — shared API for both the calc_jit CLI and the test
# binary. Encapsulates the parse → compile → JIT → execute pipeline so
# callers don't have to deal with the JIT context, slot tracker, or
# BURG matcher directly.
add_library(calc_runner STATIC
    example/calc_runner.cpp
    example/frontend/Parser.cpp
    ${CALC_AST_HEADER}
    ${CALC_IR_HEADER}
    ${CALC_COMPILE_H}
    ${CALC_STENCIL_TABLE}
    ${CMAKE_CURRENT_BINARY_DIR}/CalcMatcher.h
)
target_include_directories(calc_runner PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/example
    ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_SOURCE_DIR}/pegc/runtime
    ${CMAKE_CURRENT_BINARY_DIR}                        # calc_compile.h, CalcMatcher.h
)

add_executable(calc_jit example/calc_jit.cpp)
target_link_libraries(calc_jit PRIVATE calc_runner)

# Tests: numeric-result regression + structural IR-shape assertions.
find_package(GTest REQUIRED)
add_executable(calc_tests example/calc_tests.cpp)
target_link_libraries(calc_tests PRIVATE calc_runner GTest::GTest GTest::Main)
add_test(NAME calc_tests COMMAND calc_tests)
