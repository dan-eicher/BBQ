cmake_minimum_required(VERSION 3.16)
project(opgen LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generic opcode-spec tool: reads an opcodes.def (the consumer's op-set, e.g.
# javelina's WASM/JCVM spec — NOT shipped here) and emits the interpreter VM,
# opcode tables, headers, and verifier corpus. A full C++ tool with a pegc/asdl-
# generated C++ frontend (like ddcgc/burgc); the VM *output* it emits stays C.

# --- Generate the tag-dispatched C++ AST from opgen.asdl ---
set(OPGEN_ASDL   ${CMAKE_CURRENT_SOURCE_DIR}/grammar/opgen.asdl)
set(OPGEN_TPL    ${CMAKE_CURRENT_SOURCE_DIR}/grammar/templates/opgen_ast.inja)
set(OPGEN_AST_H  ${CMAKE_CURRENT_SOURCE_DIR}/frontend/opgen_ast.h)

add_custom_command(
    OUTPUT ${OPGEN_AST_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    COMMAND asdl -i ${OPGEN_ASDL} -t ${OPGEN_TPL} -o ${OPGEN_AST_H}
    DEPENDS asdl ${OPGEN_ASDL} ${OPGEN_TPL}
    COMMENT "Generating opgen_ast.h from opgen.asdl"
    VERBATIM)

# --- Generate the C++ parser from opgen.peg ---
set(OPGEN_PEG        ${CMAKE_CURRENT_SOURCE_DIR}/grammar/opgen.peg)
set(OPGEN_PARSER_H   ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.h)
set(OPGEN_PARSER_CPP ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.cpp)

add_custom_command(
    OUTPUT ${OPGEN_PARSER_H} ${OPGEN_PARSER_CPP}
    COMMAND pegc ${OPGEN_PEG}
                 -frames ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/
                 -o ${CMAKE_CURRENT_SOURCE_DIR}/frontend/
                 -prefix ""
    DEPENDS pegc ${OPGEN_PEG} ${OPGEN_AST_H}
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/Parser.frame
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/ParserImpl.frame
    COMMENT "Generating opgen Parser.h/.cpp from opgen.peg"
    VERBATIM)

# --- Frontend library (pegc-generated parser + asdl-generated AST) ---
add_library(opgen_frontend STATIC
    frontend/Parser.cpp
    ${OPGEN_AST_H})
target_include_directories(opgen_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    ${CMAKE_SOURCE_DIR}/pegc/runtime)

# --- Core library (the tool: model + lowering + emitters) ---
add_library(opgen_core STATIC
    src/spec.cpp
    src/semlower.cpp
    src/vmemit.cpp
    src/tablesemit.cpp)
target_include_directories(opgen_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(opgen_core PUBLIC opgen_frontend)

# --- opgen executable (the tool) ---
add_executable(opgen src/main.cpp)
target_link_libraries(opgen PRIVATE opgen_core)

# --- Tests ---
find_package(GTest REQUIRED)
enable_testing()

add_executable(opgen_tests
    test/parse_test.cpp
    test/spec_test.cpp
    test/semlower_test.cpp
    test/vmemit_test.cpp
    test/bbq_emit_test.cpp)
target_include_directories(opgen_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
target_link_libraries(opgen_tests PRIVATE opgen_core GTest::GTest GTest::Main)
target_compile_definitions(opgen_tests PRIVATE
    SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    BBQ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME opgen_tests COMMAND opgen_tests)

# --- Functional e2e: opgen generates a VM from calc.def; it is compiled (clang,
#     for the handlers' musttail) and EXECUTED on real bytecode, checking each
#     opcode computes the right result. The real test of the generated tool. ---
find_program(OPGEN_CLANG clang)
if(OPGEN_CLANG)
    set(CALC_DEF ${CMAKE_CURRENT_SOURCE_DIR}/test/data/calc.def)
    set(CALC_OUT ${CMAKE_CURRENT_BINARY_DIR}/calc_gen)

    add_custom_command(
        OUTPUT ${CALC_OUT}/gen_interp.c ${CALC_OUT}/gen_interp.h
               ${CALC_OUT}/opcodes.h ${CALC_OUT}/runtime_api.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CALC_OUT}
        COMMAND $<TARGET_FILE:opgen> -i ${CALC_DEF} -prefix calc -o ${CALC_OUT}
        DEPENDS opgen ${CALC_DEF}
        COMMENT "opgen: generating the calc VM for the interp e2e"
        VERBATIM)

    add_custom_command(
        OUTPUT ${CALC_OUT}/opgen_interp_e2e
        COMMAND ${OPGEN_CLANG} -std=c11 -O1 -fwrapv -Wall -I${CALC_OUT}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/calc_interp.c
                ${CALC_OUT}/gen_interp.c
                -o ${CALC_OUT}/opgen_interp_e2e
        DEPENDS ${CALC_OUT}/gen_interp.c
                ${CMAKE_CURRENT_SOURCE_DIR}/test/calc_interp.c
        COMMENT "clang: building the opgen interp e2e"
        VERBATIM)

    add_custom_target(opgen_interp_e2e_build ALL DEPENDS ${CALC_OUT}/opgen_interp_e2e)
    add_test(NAME opgen_interp_e2e COMMAND ${CALC_OUT}/opgen_interp_e2e)
endif()
