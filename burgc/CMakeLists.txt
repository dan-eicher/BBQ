cmake_minimum_required(VERSION 3.16)
project(burgc LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Frontend (pegc-generated parser)
add_library(burgc_frontend STATIC
    frontend/Parser.cpp)
target_include_directories(burgc_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/../pegc/runtime)

# Core library (generator)
add_library(burgc_lib STATIC
    src/generator.cpp)
target_include_directories(burgc_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(burgc_lib PUBLIC burgc_frontend)

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
