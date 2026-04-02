cmake_minimum_required(VERSION 3.16)
project(pegc LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- pegc frontend library ---
add_library(pegc_frontend STATIC
    frontend/Parser.cpp
    frontend/PegAnalysis.cpp
)
target_include_directories(pegc_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime)

# --- pegc backend library ---
# Reuses FrameProcessor from BBQ frontend
add_library(pegc_backend STATIC
    backend/ParserEmitter.cpp
    backend/CParserEmitter.cpp
    ../frontend/FrameProcessor.cpp
)
target_include_directories(pegc_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backend)
target_include_directories(pegc_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../frontend)
target_link_libraries(pegc_backend PUBLIC pegc_frontend)

# --- pegc code generator ---
add_executable(pegc tools/pegc.cpp)
target_link_libraries(pegc PRIVATE pegc_backend)

# --- tests ---
find_package(GTest REQUIRED)
enable_testing()

add_executable(pegc_tests
    tests/test_grammar_parser.cpp
    tests/test_runtime.cpp
    tests/test_peg_analysis.cpp
    tests/test_codegen.cpp
)
target_link_libraries(pegc_tests PRIVATE pegc_backend GTest::GTest GTest::Main)
target_compile_definitions(pegc_tests PRIVATE
    SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
gtest_discover_tests(pegc_tests)
