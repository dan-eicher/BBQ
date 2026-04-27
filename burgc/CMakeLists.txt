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
gtest_discover_tests(burgc_tests)
