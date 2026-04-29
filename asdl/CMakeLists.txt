cmake_minimum_required(VERSION 3.16)
project(asdl LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

# --- Auto-regenerate Parser.h/.cpp from asdl.peg ---
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.h
           ${CMAKE_CURRENT_SOURCE_DIR}/frontend/Parser.cpp
    COMMAND pegc ${CMAKE_CURRENT_SOURCE_DIR}/grammar/asdl.peg
                 -frames ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/
                 -o ${CMAKE_CURRENT_SOURCE_DIR}/frontend/
                 -prefix ""
    DEPENDS pegc
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/asdl.peg
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/Parser.frame
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/frames/ParserImpl.frame
    COMMENT "Generating asdl Parser.h/.cpp from asdl.peg"
    VERBATIM
)

add_library(asdl_frontend STATIC
    frontend/Parser.cpp)
target_include_directories(asdl_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    ${CMAKE_SOURCE_DIR}/pegc/runtime)

add_library(asdl_lib STATIC
    src/generator.cpp
    src/validator.cpp
    src/type_registry.cpp
    src/string_utils.cpp)
target_include_directories(asdl_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib/third_party
    ${CMAKE_SOURCE_DIR}/lib/third_party/inja)
target_link_libraries(asdl_lib PUBLIC asdl_frontend)

# Executable
add_executable(asdl src/main.cpp)
target_link_libraries(asdl PRIVATE asdl_lib)

# Tests (GTest, consistent with BBQ/pegc)
find_package(GTest REQUIRED)
enable_testing()
add_executable(asdl_tests
    tests/test_string_utils.cpp
    tests/test_generation.cpp)
target_link_libraries(asdl_tests PRIVATE asdl_lib GTest::GTest GTest::Main)
target_compile_definitions(asdl_tests PRIVATE
    SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME asdl_tests COMMAND asdl_tests)
