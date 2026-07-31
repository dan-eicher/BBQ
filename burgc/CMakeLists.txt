cmake_minimum_required(VERSION 3.16)
project(burgc LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Auto-regenerate Parser.h/.cpp from burg.peg ---
set(BURGC_PEG ${CMAKE_CURRENT_SOURCE_DIR}/grammar/burg.peg)
set(BURGC_PARSER_H ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.h)
set(BURGC_PARSER_CPP ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.cpp)

add_custom_command(
    OUTPUT ${BURGC_PARSER_H} ${BURGC_PARSER_CPP}
    COMMAND pegc ${BURGC_PEG}
                 -frames ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/
                 -o ${CMAKE_CURRENT_SOURCE_DIR}/frontend/
                 -prefix ""
    DEPENDS pegc
            ${BURGC_PEG}
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/Parser.frame
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/ParserImpl.frame
    COMMENT "Generating burgc Parser.h/.cpp from burg.peg"
    VERBATIM
)

# Frontend (pegc-generated parser)
add_library(burgc_frontend STATIC
    frontend/Parser.cpp)
target_include_directories(burgc_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/../pegc/runtime)

add_library(burgc_lib STATIC
    src/generator.cpp
    src/completeness.cpp
    src/cpp_backend.cpp
    src/c_backend.cpp)
target_include_directories(burgc_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/lib/third_party)
target_link_libraries(burgc_lib PUBLIC burgc_frontend bbq_frontend)

# Executable
add_executable(burgc src/main.cpp)
target_link_libraries(burgc PRIVATE burgc_lib)

# Tests
find_package(GTest REQUIRED)
enable_testing()
add_executable(burgc_tests
    tests/burgc_test.cpp)
target_link_libraries(burgc_tests PRIVATE burgc_lib GTest::GTest GTest::Main)
target_compile_definitions(burgc_tests PRIVATE
    SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME burgc_tests COMMAND burgc_tests)

# ── Rewrite-error e2e: compile and RUN a generated matcher ──────────────
# burgc_tests asserts on generated text, which cannot tell a matcher that reports the
# wrong thing from one that reports the right thing. These two targets build the same
# grammar with both backends and run the same cases against each, so backend parity is
# a tested claim.
set(BURGC_TEST_DATA ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
set(BURGC_TEST_OUT  ${CMAKE_CURRENT_BINARY_DIR}/tests)

add_custom_command(
    OUTPUT ${BURGC_TEST_OUT}/coverage_burg.h ${BURGC_TEST_OUT}/coverage_burg.c
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BURGC_TEST_OUT}
    COMMAND burgc -i ${BURGC_TEST_DATA}/coverage.burg
                  -lang c
                  --emit-rule-table
                  -frames ${CMAKE_CURRENT_SOURCE_DIR}/frames
                  -o ${BURGC_TEST_OUT}/coverage_burg.h
    DEPENDS burgc ${BURGC_TEST_DATA}/coverage.burg
            ${CMAKE_CURRENT_SOURCE_DIR}/frames/CBurg.frame
            ${CMAKE_CURRENT_SOURCE_DIR}/frames/CBurgImpl.frame
    COMMENT "Generating coverage_burg.h/.c from coverage.burg (C backend)"
    VERBATIM
)

add_custom_command(
    OUTPUT ${BURGC_TEST_OUT}/coverage_matcher.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BURGC_TEST_OUT}
    COMMAND burgc -i ${BURGC_TEST_DATA}/coverage.burg
                  -lang c++
                  --emit-rule-table
                  -frames ${CMAKE_CURRENT_SOURCE_DIR}/frames
                  -o ${BURGC_TEST_OUT}/coverage_matcher.h
    DEPENDS burgc ${BURGC_TEST_DATA}/coverage.burg
            ${CMAKE_CURRENT_SOURCE_DIR}/frames/CppBurg.frame
    COMMENT "Generating coverage_matcher.h from coverage.burg (C++ backend)"
    VERBATIM
)

add_executable(burgc_coverage_c_e2e
    ${BURGC_TEST_DATA}/coverage_main.c
    ${BURGC_TEST_DATA}/coverage_node.c
    ${BURGC_TEST_OUT}/coverage_burg.c
    ${BURGC_TEST_OUT}/coverage_burg.h)
set_target_properties(burgc_coverage_c_e2e PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
target_compile_options(burgc_coverage_c_e2e PRIVATE -Wall -Wextra -Werror)
target_include_directories(burgc_coverage_c_e2e PRIVATE
    ${BURGC_TEST_DATA} ${BURGC_TEST_OUT})
target_link_libraries(burgc_coverage_c_e2e PRIVATE bbq_crt)
add_test(NAME burgc_coverage_c_e2e COMMAND burgc_coverage_c_e2e)

add_executable(burgc_coverage_cpp_e2e
    ${BURGC_TEST_DATA}/coverage_main.cpp
    ${BURGC_TEST_DATA}/coverage_node.c
    ${BURGC_TEST_OUT}/coverage_matcher.h)
target_compile_options(burgc_coverage_cpp_e2e PRIVATE -Wall -Wextra -Werror)
target_include_directories(burgc_coverage_cpp_e2e PRIVATE
    ${BURGC_TEST_DATA} ${BURGC_TEST_OUT})
add_test(NAME burgc_coverage_cpp_e2e COMMAND burgc_coverage_cpp_e2e)
