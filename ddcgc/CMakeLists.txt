cmake_minimum_required(VERSION 3.16)
project(ddcgc LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Auto-regenerate ddcg_ast.h from ddcg.asdl ---
set(DDCG_ASDL    ${CMAKE_CURRENT_SOURCE_DIR}/grammar/ddcg.asdl)
set(DDCG_TPL     ${CMAKE_SOURCE_DIR}/pegc/grammar/templates/grammar_ast.inja)
set(DDCG_AST_H   ${CMAKE_CURRENT_SOURCE_DIR}/frontend/ddcg_ast.h)
set(DDCG_JSON    ${CMAKE_CURRENT_SOURCE_DIR}/grammar/ddcg.json)

add_custom_command(
    OUTPUT ${DDCG_AST_H} ${DDCG_JSON}
    COMMAND asdl -i ${DDCG_ASDL}
                 -t ${DDCG_TPL}
                 -o ${DDCG_AST_H}
                 --json ${DDCG_JSON}
    DEPENDS asdl ${DDCG_ASDL} ${DDCG_TPL}
    COMMENT "Generating ddcg_ast.h + ddcg.json from ddcg.asdl"
    VERBATIM
)

# --- Auto-regenerate Parser.h/.cpp from ddcg.peg ---
set(DDCG_PEG         ${CMAKE_CURRENT_SOURCE_DIR}/grammar/ddcg.peg)
set(DDCG_PARSER_H    ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.h)
set(DDCG_PARSER_CPP  ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.cpp)

add_custom_command(
    OUTPUT ${DDCG_PARSER_H} ${DDCG_PARSER_CPP}
    COMMAND pegc ${DDCG_PEG}
                 -frames ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/
                 -o ${CMAKE_CURRENT_SOURCE_DIR}/frontend/
                 -prefix ""
    DEPENDS pegc
            ${DDCG_PEG}
            ${DDCG_AST_H}
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/Parser.frame
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/ParserImpl.frame
    COMMENT "Generating ddcgc Parser.h/.cpp from ddcg.peg"
    VERBATIM
)

# Frontend (pegc-generated parser + asdl-generated AST)
add_library(ddcgc_frontend STATIC
    frontend/Parser.cpp
    ${DDCG_AST_H})
target_include_directories(ddcgc_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    ${CMAKE_SOURCE_DIR}/pegc/runtime)

# Library (schema loader, type checker, generator base, backends)
add_library(ddcgc_lib STATIC
    src/schema.cpp
    src/typecheck.cpp
    src/emit_backend.cpp
    src/cpp_backend.cpp
    src/c_backend.cpp
    ${DDCG_JSON})
set_source_files_properties(${DDCG_JSON} PROPERTIES HEADER_FILE_ONLY ON)
target_include_directories(ddcgc_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib/third_party)
target_link_libraries(ddcgc_lib PUBLIC ddcgc_frontend)

# Executable
add_executable(ddcgc src/main.cpp)
target_link_libraries(ddcgc PRIVATE ddcgc_lib)

# Tests
find_package(GTest REQUIRED)
enable_testing()
add_executable(ddcgc_tests
    tests/ddcgc_test.cpp)
target_link_libraries(ddcgc_tests PRIVATE ddcgc_lib GTest::GTest GTest::Main)
target_compile_definitions(ddcgc_tests PRIVATE
    SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    BBQ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# One ctest entry instead of one per gtest TEST(...) — gtest schedules
# the cases in-process, no per-test fork+exec startup cost.
add_test(NAME ddcgc_tests COMMAND ddcgc_tests)

# ── End-to-end test: ddcgc generates a class-wrapped .h from tiny.ddcg,──
# ── that .h is included by tiny_main.cpp which builds + runs as a ctest──
set(DDCG_TEST_DATA   ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
set(DDCG_TEST_OUT    ${CMAKE_CURRENT_BINARY_DIR}/tests)

# 1. asdl regen for the tiny test schemas (.asdl -> .json)
add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/tiny_ast.json ${DDCG_TEST_OUT}/tiny_target.json
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DDCG_TEST_OUT}
    COMMAND asdl -i ${DDCG_TEST_DATA}/tiny_ast.asdl
                 --json ${DDCG_TEST_OUT}/tiny_ast.json
    COMMAND asdl -i ${DDCG_TEST_DATA}/tiny_target.asdl
                 --json ${DDCG_TEST_OUT}/tiny_target.json
    DEPENDS asdl
            ${DDCG_TEST_DATA}/tiny_ast.asdl
            ${DDCG_TEST_DATA}/tiny_target.asdl
    COMMENT "Generating tiny test schemas"
    VERBATIM
)

# 2. ddcgc generates tiny_compile.h from tiny.ddcg + the JSON sidecars.
#    The output is a header (class with inline method bodies); main
#    #includes it directly.
add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/tiny_compile.h
    COMMAND ddcgc -i ${DDCG_TEST_DATA}/tiny.ddcg
                  -ast ${DDCG_TEST_OUT}/tiny_ast.json
                  -ir  ${DDCG_TEST_OUT}/tiny_target.json
                  -o   ${DDCG_TEST_OUT}/tiny_compile.h
    DEPENDS ddcgc
            ${DDCG_TEST_DATA}/tiny.ddcg
            ${DDCG_TEST_OUT}/tiny_ast.json
            ${DDCG_TEST_OUT}/tiny_target.json
    COMMENT "Running ddcgc on tests/data/tiny.ddcg"
    VERBATIM
)

# 3. Compile + link the generated dispatcher with the stub runtime + main
add_executable(ddcgc_tiny_e2e
    ${DDCG_TEST_DATA}/tiny_main.cpp
    ${DDCG_TEST_OUT}/tiny_compile.h)
target_include_directories(ddcgc_tiny_e2e PRIVATE
    ${DDCG_TEST_DATA}
    ${DDCG_TEST_OUT})

# 4. Register as a ctest case
add_test(NAME ddcgc_tiny_e2e COMMAND ddcgc_tiny_e2e)

# ── C-backend e2e: kitchensink — single comprehensive C test that  ──
# ── exercises every ddcgc rule-body form (lists, tuples, match,    ──
# ── build, label, sequence-bind, etc.). Mirrors big_main.cpp's     ──
# ── feature-table structure but compiled and run as C.             ──
add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/kitchensink_ast.json
           ${DDCG_TEST_OUT}/kitchensink_target.json
           ${DDCG_TEST_OUT}/kitchensink_ast.h
           ${DDCG_TEST_OUT}/kitchensink_target.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DDCG_TEST_OUT}
    COMMAND asdl -lang c
                 -i ${DDCG_TEST_DATA}/kitchensink_ast.asdl
                 -t ${CMAKE_SOURCE_DIR}/asdl/templates/c.inja
                 -o ${DDCG_TEST_OUT}/kitchensink_ast.h
                 --json ${DDCG_TEST_OUT}/kitchensink_ast.json
    COMMAND asdl -lang c
                 -i ${DDCG_TEST_DATA}/kitchensink_target.asdl
                 -t ${CMAKE_SOURCE_DIR}/asdl/templates/c.inja
                 -o ${DDCG_TEST_OUT}/kitchensink_target.h
                 --json ${DDCG_TEST_OUT}/kitchensink_target.json
    DEPENDS asdl
            ${DDCG_TEST_DATA}/kitchensink_ast.asdl
            ${DDCG_TEST_DATA}/kitchensink_target.asdl
            ${CMAKE_SOURCE_DIR}/asdl/templates/c.inja
    COMMENT "Generating kitchensink schemas (asdl-c headers + JSON sidecars)"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/kitchensink_compile.h
           ${DDCG_TEST_OUT}/kitchensink_compile.c
    COMMAND ddcgc -lang c
                  -i ${DDCG_TEST_DATA}/kitchensink.ddcg
                  -ast ${DDCG_TEST_OUT}/kitchensink_ast.json
                  -ir  ${DDCG_TEST_OUT}/kitchensink_target.json
                  -o   ${DDCG_TEST_OUT}/kitchensink_compile.h
    DEPENDS ddcgc
            ${DDCG_TEST_DATA}/kitchensink.ddcg
            ${DDCG_TEST_OUT}/kitchensink_ast.json
            ${DDCG_TEST_OUT}/kitchensink_target.json
    COMMENT "Running ddcgc -lang c on tests/data/kitchensink.ddcg"
    VERBATIM
)

add_executable(ddcgc_kitchensink_c_e2e
    ${DDCG_TEST_DATA}/kitchensink_main.c
    ${DDCG_TEST_OUT}/kitchensink_compile.c
    ${DDCG_TEST_OUT}/kitchensink_compile.h)
set_target_properties(ddcgc_kitchensink_c_e2e PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
target_compile_options(ddcgc_kitchensink_c_e2e PRIVATE
    -Wall -Wextra -Werror)
target_include_directories(ddcgc_kitchensink_c_e2e PRIVATE
    ${DDCG_TEST_DATA}
    ${DDCG_TEST_OUT})
target_link_libraries(ddcgc_kitchensink_c_e2e PRIVATE bbq_crt)
add_test(NAME ddcgc_kitchensink_c_e2e COMMAND ddcgc_kitchensink_c_e2e)

# ── scoped — Phase 1 fixture for chained-rho-pangolin (recursive ρ). ──
add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/scoped_ast.json
           ${DDCG_TEST_OUT}/scoped_target.json
           ${DDCG_TEST_OUT}/scoped_ast.h
           ${DDCG_TEST_OUT}/scoped_target.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DDCG_TEST_OUT}
    COMMAND asdl -lang c
                 -i ${DDCG_TEST_DATA}/scoped_ast.asdl
                 -t ${CMAKE_SOURCE_DIR}/asdl/templates/c.inja
                 -o ${DDCG_TEST_OUT}/scoped_ast.h
                 --json ${DDCG_TEST_OUT}/scoped_ast.json
    COMMAND asdl -lang c
                 -i ${DDCG_TEST_DATA}/scoped_target.asdl
                 -t ${CMAKE_SOURCE_DIR}/asdl/templates/c.inja
                 -o ${DDCG_TEST_OUT}/scoped_target.h
                 --json ${DDCG_TEST_OUT}/scoped_target.json
    DEPENDS asdl
            ${DDCG_TEST_DATA}/scoped_ast.asdl
            ${DDCG_TEST_DATA}/scoped_target.asdl
            ${CMAKE_SOURCE_DIR}/asdl/templates/c.inja
    COMMENT "Generating scoped schemas"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/scoped_compile.h
           ${DDCG_TEST_OUT}/scoped_compile.c
    COMMAND ddcgc -lang c
                  -i ${DDCG_TEST_DATA}/scoped.ddcg
                  -ast ${DDCG_TEST_OUT}/scoped_ast.json
                  -ir  ${DDCG_TEST_OUT}/scoped_target.json
                  -o   ${DDCG_TEST_OUT}/scoped_compile.h
    DEPENDS ddcgc
            ${DDCG_TEST_DATA}/scoped.ddcg
            ${DDCG_TEST_OUT}/scoped_ast.json
            ${DDCG_TEST_OUT}/scoped_target.json
    COMMENT "Running ddcgc -lang c on tests/data/scoped.ddcg"
    VERBATIM
)

add_executable(ddcgc_scoped_e2e
    ${DDCG_TEST_DATA}/scoped_main.c
    ${DDCG_TEST_OUT}/scoped_compile.c
    ${DDCG_TEST_OUT}/scoped_compile.h)
set_target_properties(ddcgc_scoped_e2e PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
target_compile_options(ddcgc_scoped_e2e PRIVATE -Wall -Wextra -Werror)
target_include_directories(ddcgc_scoped_e2e PRIVATE
    ${DDCG_TEST_DATA}
    ${DDCG_TEST_OUT})
target_link_libraries(ddcgc_scoped_e2e PRIVATE bbq_crt)
add_test(NAME ddcgc_scoped_e2e COMMAND ddcgc_scoped_e2e)

# ── "big" e2e — wider feature surface (nested patterns, predicate     ──
# ── guard, for-accumulator, list concat, ternary, Sequence field).    ──
add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/big_ast.json ${DDCG_TEST_OUT}/big_target.json
    COMMAND asdl -i ${DDCG_TEST_DATA}/big_ast.asdl
                 --json ${DDCG_TEST_OUT}/big_ast.json
    COMMAND asdl -i ${DDCG_TEST_DATA}/big_target.asdl
                 --json ${DDCG_TEST_OUT}/big_target.json
    DEPENDS asdl
            ${DDCG_TEST_DATA}/big_ast.asdl
            ${DDCG_TEST_DATA}/big_target.asdl
    COMMENT "Generating big test schemas"
    VERBATIM
)
add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/big_compile.h
    COMMAND ddcgc -i ${DDCG_TEST_DATA}/big.ddcg
                  -ast ${DDCG_TEST_OUT}/big_ast.json
                  -ir  ${DDCG_TEST_OUT}/big_target.json
                  -o   ${DDCG_TEST_OUT}/big_compile.h
    DEPENDS ddcgc
            ${DDCG_TEST_DATA}/big.ddcg
            ${DDCG_TEST_DATA}/big_imports.ddcg
            ${DDCG_TEST_OUT}/big_ast.json
            ${DDCG_TEST_OUT}/big_target.json
    COMMENT "Running ddcgc on tests/data/big.ddcg"
    VERBATIM
)
add_executable(ddcgc_big_e2e
    ${DDCG_TEST_DATA}/big_main.cpp
    ${DDCG_TEST_OUT}/big_compile.h)
target_include_directories(ddcgc_big_e2e PRIVATE
    ${DDCG_TEST_DATA}
    ${DDCG_TEST_OUT})
add_test(NAME ddcgc_big_e2e COMMAND ddcgc_big_e2e)

# ── "paper_consumer" e2e — exercises the shipped dybvig.ddcg library ──
# ── via -L search path. Verifies paper-direct cg_helpers (Lnext, ρ).  ──
set(DDCG_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# Generate the reference IR schema's JSON (lib ships the asdl, build
# emits the JSON the consumer points -ir at).
add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/dybvig_ir.json
    COMMAND asdl -i ${DDCG_LIB_DIR}/dybvig_ir.asdl
                 --json ${DDCG_TEST_OUT}/dybvig_ir.json
    DEPENDS asdl ${DDCG_LIB_DIR}/dybvig_ir.asdl
    COMMENT "Generating dybvig_ir.json from lib/dybvig_ir.asdl"
    VERBATIM
)

# Per-consumer AST schema.
add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/paper_consumer_ast.json
    COMMAND asdl -i ${DDCG_TEST_DATA}/paper_consumer_ast.asdl
                 --json ${DDCG_TEST_OUT}/paper_consumer_ast.json
    DEPENDS asdl ${DDCG_TEST_DATA}/paper_consumer_ast.asdl
    COMMENT "Generating paper_consumer_ast.json"
    VERBATIM
)

# ddcgc invocation: `-L lib/` lets `import "dybvig.ddcg"` resolve
# without copying the file or relative-pathing it.
add_custom_command(
    OUTPUT ${DDCG_TEST_OUT}/paper_consumer_compile.h
    COMMAND ddcgc -i ${DDCG_TEST_DATA}/paper_consumer.ddcg
                  -ast ${DDCG_TEST_OUT}/paper_consumer_ast.json
                  -ir  ${DDCG_TEST_OUT}/dybvig_ir.json
                  -L   ${DDCG_LIB_DIR}
                  -o   ${DDCG_TEST_OUT}/paper_consumer_compile.h
    DEPENDS ddcgc
            ${DDCG_TEST_DATA}/paper_consumer.ddcg
            ${DDCG_LIB_DIR}/dybvig.ddcg
            ${DDCG_TEST_OUT}/paper_consumer_ast.json
            ${DDCG_TEST_OUT}/dybvig_ir.json
    COMMENT "Running ddcgc on paper_consumer.ddcg with lib/dybvig.ddcg"
    VERBATIM
)
add_executable(ddcgc_paper_e2e
    ${DDCG_TEST_DATA}/paper_consumer_main.cpp
    ${DDCG_TEST_OUT}/paper_consumer_compile.h)
target_include_directories(ddcgc_paper_e2e PRIVATE
    ${DDCG_TEST_DATA}
    ${DDCG_TEST_OUT})
add_test(NAME ddcgc_paper_e2e COMMAND ddcgc_paper_e2e)
