cmake_minimum_required(VERSION 3.16)
project(BBQ LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- C runtime library (for generated C code) ---
add_library(bbq_crt STATIC
    crt/bbq_arena.c
    crt/bbq_buf.c
    crt/bbq_htree.c
)
set_target_properties(bbq_crt PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
target_include_directories(bbq_crt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/crt)

# --- shared frontend library ---
add_library(bbq_frontend STATIC
    frontend/Parser.cpp
    frontend/Errors.cpp
    frontend/Sema.cpp
    frontend/FrameProcessor.cpp
    frontend/BBQ_AST.h
    frontend/Parser.h
)
target_include_directories(bbq_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/pegc/runtime)

# --- C++ backend library ---
add_library(bbq_cpp_backend STATIC
    backends/cpp/TypeEmitter.cpp
    backends/cpp/ParserEmitter.cpp
)
target_include_directories(bbq_cpp_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/cpp/runtime)
target_link_libraries(bbq_cpp_backend PUBLIC bbq_frontend)

# --- C backend library ---
add_library(bbq_c_backend STATIC
    backends/c/CTypeEmitter.cpp
    backends/c/CReaderEmitter.cpp
    backends/c/CWriterEmitter.cpp
)
target_include_directories(bbq_c_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/c
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/c/runtime)
target_link_libraries(bbq_c_backend PUBLIC bbq_frontend)

# --- CEK backend library ---
add_library(bbq_cek_backend STATIC
    backends/cek/Machine.cpp
    backends/cek/Compiler.cpp
)
target_include_directories(bbq_cek_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek)
target_link_libraries(bbq_cek_backend PUBLIC bbq_frontend)

# --- bbqc code generator ---
add_executable(bbqc tools/bbqc.cpp)
target_link_libraries(bbqc PRIVATE bbq_cpp_backend bbq_c_backend)

# --- tests ---
find_package(GTest REQUIRED)
enable_testing()

add_executable(bbq_tests
    test/parse_test.cpp
    test/sema_test.cpp
    test/context_test.cpp
    test/type_emitter_test.cpp
    test/parser_emitter_test.cpp
    test/integration_test.cpp
    test/codegen_compile_test.cpp
    test/cek_test.cpp
    test/c_type_emitter_test.cpp
    test/c_backend_e2e_test.cpp
    test/crt_test.cpp
)
target_link_libraries(bbq_tests PRIVATE bbq_cpp_backend bbq_c_backend bbq_cek_backend bbq_crt GTest::GTest GTest::Main)
target_compile_definitions(bbq_tests PRIVATE
    SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME bbq_tests COMMAND bbq_tests)

add_executable(lib_tests lib/tests/test_worklist.cpp)
target_include_directories(lib_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_link_libraries(lib_tests PRIVATE GTest::GTest GTest::Main)
add_test(NAME lib_tests COMMAND lib_tests)

# --- Python extension module (optional) ---
find_package(Python3 COMPONENTS Development)
if(Python3_FOUND)
    # Static libs linked into the .so need PIC
    set_target_properties(bbq_frontend bbq_cek_backend PROPERTIES
        POSITION_INDEPENDENT_CODE ON)

    add_library(bbq_python MODULE bindings/python/bbq_python.cpp)
    target_link_libraries(bbq_python PRIVATE bbq_frontend bbq_cek_backend Python3::Python)
    set_target_properties(bbq_python PROPERTIES
        PREFIX ""
        OUTPUT_NAME "bbq"
    )
endif()

# --- pegc subproject ---
add_subdirectory(pegc)

# --- asdl subproject ---
add_subdirectory(asdl)

# --- burgc subproject ---
add_subdirectory(burgc)

# --- jitterator subproject ---
add_subdirectory(jitterator)

# --- ddcgc subproject ---
add_subdirectory(ddcgc)

# --- Auto-regenerate BBQ_AST.h from BBQ.asdl ---
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/frontend/BBQ_AST.h
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/grammar/BBQ.asdl
                 -t ${CMAKE_CURRENT_SOURCE_DIR}/grammar/templates/bbq_ast.inja
                 -o ${CMAKE_CURRENT_SOURCE_DIR}/frontend/BBQ_AST.h
    DEPENDS asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/BBQ.asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/templates/bbq_ast.inja
    COMMENT "Generating BBQ_AST.h from BBQ.asdl"
)

# --- Auto-regenerate Parser.h/.cpp from BBQ.peg ---
# Note: VERBATIM ensures the empty-string argument to -prefix is preserved.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.h
           ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.cpp
    COMMAND pegc ${CMAKE_CURRENT_SOURCE_DIR}/grammar/BBQ.peg
                 -frames ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/
                 -o ${CMAKE_CURRENT_SOURCE_DIR}/frontend/
                 -prefix ""
    DEPENDS pegc
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/BBQ.peg
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/Parser.frame
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/ParserImpl.frame
    COMMENT "Generating Parser.h/.cpp from BBQ.peg"
    VERBATIM
)



