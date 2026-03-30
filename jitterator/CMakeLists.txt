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
add_executable(calc_jit
    example/calc_jit.cpp
    example/frontend/Parser.cpp
)
target_include_directories(calc_jit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/example
    ${CMAKE_CURRENT_SOURCE_DIR}/example/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_SOURCE_DIR}/pegc/runtime
)
