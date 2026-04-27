cmake_minimum_required(VERSION 3.16)
project(asdl LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_library(asdl_frontend STATIC
    frontend/Parser.cpp)
target_include_directories(asdl_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime)

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
gtest_discover_tests(asdl_tests)
