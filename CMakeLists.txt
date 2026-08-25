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
    crt/bbq_hmap.c
    crt/bbq_dict.c
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
    frontend/Names.cpp
    frontend/BBQ_AST.h
    frontend/Parser.h
)
target_include_directories(bbq_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/pegc/runtime)

# --- canonical C type-name mapping (shared by the render + writer backends) ---
add_library(bbq_ctypes STATIC
    backends/c/CTypeMapper.cpp
)
target_include_directories(bbq_ctypes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/c
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/c/runtime)
target_link_libraries(bbq_ctypes PUBLIC bbq_frontend)

# --- shared C++ document runtime (bbq::zcow: zero-copy spans + copy-on-write) ---
# The neutral runtime that BOTH the CEK machine and generated C++ parsers depend
# on. It has NO dependency on the machine IR (KontNode.h) — generated code names
# only bbq::, never bbq::cek::. This is the C++ analogue of backends/c/runtime.
add_library(bbq_index_runtime STATIC
    backends/cpp/runtime/CaptureDecode.cpp
    backends/cpp/runtime/Capture.h
    backends/cpp/runtime/CaptureDecode.h
    backends/cpp/runtime/CaptureCow.h
    backends/cpp/runtime/ParseArena.h
    backends/cpp/runtime/bbq_node.h
)
target_include_directories(bbq_index_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/cpp/runtime)

# --- CEK backend library ---
add_library(bbq_cek_backend STATIC
    backends/cek/Machine.cpp
    backends/cek/KontNode.h
    backends/cek/bbq_compile.h
)
target_include_directories(bbq_cek_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek)
target_link_libraries(bbq_cek_backend PUBLIC bbq_frontend bbq_index_runtime)

# --- shape generator (the type model) — the SHAPE half of the contract ---
# The neutral type model, derived from the AST + Sema resolved facts ONLY. This
# lib does NOT link the CEK backend and has NO backends/cek on its include path,
# so the tool's shape path is structurally decoupled from the machine — a stray
# cek reference won't compile, not just "happens to build because cek is in the
# tree." Behavior generators (the parsers) are NOT here: they consume the kont IR
# and live with the readers below.
add_library(bbq_shapegen STATIC
    backends/render/RenderTypes.cpp
)
target_include_directories(bbq_shapegen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/render
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/third_party/inja)
target_link_libraries(bbq_shapegen PUBLIC bbq_frontend bbq_ctypes)

# --- behavior generators (the parsers) — the BEHAVIOR half, IR consumers ---
# Both readers are burg profiles over the kont IR: the C reader (c-owning) and the
# C++ view reader (cpp-zcow) are the same lowering under two ReaderProfiles. Behavior,
# so they live on the IR side and link the CEK backend.
add_library(bbq_render STATIC
    backends/render/RenderEmit.cpp
    backends/render/CompilerCtx.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/BbqMatcher.h
)
target_include_directories(bbq_render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backends/render
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/third_party/inja)
target_link_libraries(bbq_render PUBLIC bbq_cek_backend bbq_ctypes bbq_shapegen)

# --- bbqc code generator ---
add_executable(bbqc tools/bbqc.cpp)
target_link_libraries(bbqc PRIVATE bbq_ctypes bbq_render)

# bbqc C-backend smoke test: the CLI generates C from a spec, which is then
# compiled (-Werror) and round-tripped. Gates the otherwise-untested CLI.
add_test(NAME bbqc_c_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/bbqc_c_smoke.sh
            $<TARGET_FILE:bbqc> ${CMAKE_CURRENT_SOURCE_DIR})

# --- tests ---
find_package(GTest REQUIRED)
enable_testing()

# C++ ZCow parity fixture: bbqc generates the handle classes from the matrix-driven
# fixture grammar, compiled INTO the test so the parity test reads real values
# through them (not g++ -fsyntax-only). Regenerated when the grammar/template change.
set(ZCOW_PARITY_TYPES ${CMAKE_CURRENT_BINARY_DIR}/ZcowParityTypes.h)
add_custom_command(
    OUTPUT ${ZCOW_PARITY_TYPES}
    COMMAND bbqc -backend cpp
                 -prefix ZcowParity -namespace zp
                 -templates ${CMAKE_CURRENT_SOURCE_DIR}/backends/render/templates
                 -o ${CMAKE_CURRENT_BINARY_DIR}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/zcow_parity.bbq
    DEPENDS bbqc
            ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/zcow_parity.bbq
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/render/templates/types_cpp.inja
    COMMENT "bbqc: zcow_parity.bbq -> ZcowParityTypes.h (C++ ZCow parity fixture)"
    VERBATIM
)

# cpp-zcow view parser fixture: bbqc generates the handle classes AND the view
# parser (the index-builder), both compiled into the test so the parser's index is
# parity-checked against the CEK. Grows with the view profile.
set(ZCOW_READER_HDRS ${CMAKE_CURRENT_BINARY_DIR}/ZcowReaderTypes.h
                     ${CMAKE_CURRENT_BINARY_DIR}/ZcowReaderReader.h)
add_custom_command(
    OUTPUT ${ZCOW_READER_HDRS}
    COMMAND bbqc -backend cpp
                 -prefix ZcowReader -namespace zr
                 -templates ${CMAKE_CURRENT_SOURCE_DIR}/backends/render/templates
                 -o ${CMAKE_CURRENT_BINARY_DIR}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/zcow_reader.bbq
    DEPENDS bbqc
            ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/zcow_reader.bbq
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/render/templates/types_cpp.inja
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/render/templates/reader_view_cpp.inja
    COMMENT "bbqc: zcow_reader.bbq -> ZcowReader{Types,Reader}.h (C++ view parser)"
    VERBATIM
)

add_executable(bbq_tests
    test/parse_test.cpp
    test/sema_test.cpp
    test/cek_test.cpp
    test/c_writer_emitter_test.cpp
    test/c_backend_e2e_test.cpp
    test/render_c_test.cpp
    test/render_types_test.cpp
    test/render_types_cpp_test.cpp
    test/render_types_cpp_parity_test.cpp
    test/bbq_reader_test.cpp
    test/render_view_parser_test.cpp
    test/cross_backend_test.cpp
    test/crt_test.cpp
    ${ZCOW_PARITY_TYPES}
    ${ZCOW_READER_HDRS}
)
target_link_libraries(bbq_tests PRIVATE bbq_ctypes bbq_cek_backend bbq_render bbq_crt GTest::GTest GTest::Main)
target_include_directories(bbq_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})  # generated ZcowParityTypes.h
target_compile_definitions(bbq_tests PRIVATE
    SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME bbq_tests COMMAND bbq_tests)

# ZCow — the parsed document. The foundation layer, so it builds and tests on its
# own: no grammar, no parser, no generated code, nothing above it.
add_executable(zcow_tests test/bbq_document_test.cpp)
target_link_libraries(zcow_tests PRIVATE bbq_index_runtime GTest::GTest GTest::Main)
add_test(NAME zcow_tests COMMAND zcow_tests)

# The generated C++ backend — the same laws and the same fixture matrix as cek_tests,
# over documents its own reader built. Index parity with the CEK is not evidence that
# what this backend produces writes back correctly; that needs its own suite.
add_executable(cpp_tests test/cpp_backend_test.cpp ${ZCOW_READER_HDRS})
target_link_libraries(cpp_tests PRIVATE bbq_index_runtime GTest::GTest GTest::Main)
target_include_directories(cpp_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME cpp_tests COMMAND cpp_tests)

# The CEK machine — the reference the generators are generating against. Its own
# target, so it can be verified without the backends that consume it.
add_executable(cek_tests test/cek_test.cpp)
target_link_libraries(cek_tests PRIVATE bbq_cek_backend bbq_ctypes GTest::GTest GTest::Main)
target_compile_definitions(cek_tests PRIVATE SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME cek_tests COMMAND cek_tests)

add_executable(lib_tests lib/tests/test_worklist.cpp)
target_include_directories(lib_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_link_libraries(lib_tests PRIVATE GTest::GTest GTest::Main)
add_test(NAME lib_tests COMMAND lib_tests)

# --- bbq_hmap allocation-failure tests ---
# Separate binary: `--wrap=calloc` is link-wide, and forcing every other test in the
# suite through an interposed allocator to cover one module would be a bad trade. The
# timeout is load-bearing — the bug these tests pin down is an unbounded probe loop,
# which presents as a hang rather than a failed assertion.
add_executable(crt_hmap_oom_tests test/crt_hmap_oom_test.cpp)
target_link_libraries(crt_hmap_oom_tests PRIVATE bbq_crt GTest::GTest GTest::Main)
target_link_options(crt_hmap_oom_tests PRIVATE -Wl,--wrap=calloc)
add_test(NAME crt_hmap_oom_tests COMMAND crt_hmap_oom_tests)
set_tests_properties(crt_hmap_oom_tests PROPERTIES TIMEOUT 30)

# --- Python extension module (optional) ---
find_package(Python3 COMPONENTS Development)
if(Python3_FOUND)
    # Static libs linked into the .so need PIC. The module does not link bbq_render:
    # writing is the document's own (zcow::transient::serialize), not a lowered op-list.
    set_target_properties(bbq_frontend bbq_cek_backend bbq_index_runtime
                          bbq_render bbq_ctypes bbq_shapegen PROPERTIES
        POSITION_INDEPENDENT_CODE ON)

    add_library(bbq_python MODULE bindings/python/bbq_python.cpp bindings/python/bbq_build.cpp)
    target_link_libraries(bbq_python PRIVATE bbq_frontend bbq_cek_backend Python3::Python)
    set_target_properties(bbq_python PROPERTIES
        PREFIX ""
        OUTPUT_NAME "bbq"
    )

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # The python test imports bbq.so (the bbq_python module). add_test does not
        # depend on a target, so without this it would run against a stale/absent
        # module — a stale-artifact false pass. A setup-fixture test rebuilds the
        # module first, wiring the test to its artifact in the build graph.
        add_test(NAME bbq_python_build
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR} --target bbq_python)
        set_tests_properties(bbq_python_build PROPERTIES FIXTURES_SETUP bbq_python_module)

        # pytest belongs to the interpreter, not the system — a second build dir aimed at
        # another interpreter (the free-threaded one below) usually has none. Say so at
        # configure time rather than registering a test that can only fail on "No module
        # named pytest".
        execute_process(COMMAND ${Python3_EXECUTABLE} -c "import pytest"
            RESULT_VARIABLE _bbq_no_pytest OUTPUT_QUIET ERROR_QUIET)
        if(_bbq_no_pytest)
            message(STATUS "bbq: ${Python3_EXECUTABLE} has no pytest — skipping bbq_python_tests")
        else()
            add_test(NAME bbq_python_tests
                COMMAND ${Python3_EXECUTABLE} -m pytest
                    ${CMAKE_CURRENT_SOURCE_DIR}/test/test_bbq_python.py
                    --tb=short
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(bbq_python_tests PROPERTIES
                FIXTURES_REQUIRED bbq_python_module
                ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}")
        endif()

        # Free-threading stress. The module declares Py_MOD_GIL_NOT_USED, so it must
        # serialize its own shared state; this hammers each piece of it from many threads
        # with no GIL to hide behind. Only meaningful on a free-threaded interpreter —
        # configure a second build dir against one to get it:
        #   cmake -B build-ft -DPython3_FIND_ABI='ANY;ANY;ANY;ON' \
        #                     -DPython3_EXECUTABLE=/usr/bin/python3.14t
        # (Python3_FREE_THREADED is not populated by CMake 3.31, so ask the interpreter.)
        execute_process(COMMAND ${Python3_EXECUTABLE} -c
            "import sysconfig,sys; sys.exit(0 if sysconfig.get_config_var('Py_GIL_DISABLED') else 1)"
            RESULT_VARIABLE _bbq_not_freethreaded OUTPUT_QUIET ERROR_QUIET)
        if(NOT _bbq_not_freethreaded)
            add_test(NAME bbq_python_freethreaded
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/test/test_bbq_freethreaded.py)
            set_tests_properties(bbq_python_freethreaded PROPERTIES
                FIXTURES_REQUIRED bbq_python_module
                TIMEOUT 300
                ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}")
        endif()
    endif()
endif()

# --- pegc subproject ---
add_subdirectory(pegc)

# --- asdl subproject ---
add_subdirectory(asdl)

# --- burgc subproject ---
add_subdirectory(burgc)

# --- opgen subproject (opcode-spec -> VM/tables/headers generator) ---
add_subdirectory(opgen)

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

# --- Auto-regenerate KontNode.h + bbq_ir.json from bbq_ir.asdl ---
# bbq_ir.json is a JSON sidecar consumed by ddcgc when generating
# bbq_compile.h from bbq.ddcg.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/KontNode.h
           ${CMAKE_CURRENT_BINARY_DIR}/bbq_ir.json
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/bbq_ir.asdl
                 -t ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/templates/bbq_ir.inja
                 -o ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/KontNode.h
                 --json ${CMAKE_CURRENT_BINARY_DIR}/bbq_ir.json
    DEPENDS asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/bbq_ir.asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/templates/bbq_ir.inja
    COMMENT "Generating KontNode.h + bbq_ir.json from bbq_ir.asdl"
)

# --- Auto-generate BbqMatcher.h from bbq.burg (the C reader lowering) ---
# burgc lowers the kont-graph IR (bbq_ir.json) to the continuation-stencil
# records the backend templates render; bbq_render's RenderEmit includes it.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/BbqMatcher.h
    COMMAND burgc -i ${CMAKE_CURRENT_SOURCE_DIR}/backends/render/bbq.burg
                  -asdl ${CMAKE_CURRENT_BINARY_DIR}/bbq_ir.json
                  -frames ${CMAKE_CURRENT_SOURCE_DIR}/burgc/frames
                  -o ${CMAKE_CURRENT_BINARY_DIR}/BbqMatcher.h
    DEPENDS burgc
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/render/bbq.burg
            ${CMAKE_CURRENT_BINARY_DIR}/bbq_ir.json
    COMMENT "Generating BbqMatcher.h from bbq.burg"
)

# --- Auto-regenerate bbq.json (AST schema sidecar for ddcgc) ---
# ddcgc consumes this JSON when compiling bbq.ddcg → bbq_compile.h.
# It mirrors what bbq_ast.inja generates as C++, but in JSON form so
# ddcgc can introspect AST shapes at compile-time.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bbq.json
    COMMAND asdl -i ${CMAKE_CURRENT_SOURCE_DIR}/grammar/BBQ.asdl
                 --json ${CMAKE_CURRENT_BINARY_DIR}/bbq.json
    DEPENDS asdl
            ${CMAKE_CURRENT_SOURCE_DIR}/grammar/BBQ.asdl
    COMMENT "Generating bbq.json from BBQ.asdl"
)

# --- Auto-regenerate bbq_compile.h from bbq.ddcg ---
# ddcgc compiles the BBQ.asdl → kont-IR translation rule set into a
# `class Compiler` C++ header. Driver (bbq_runner.cpp) consumes this.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/bbq_compile.h
    COMMAND ddcgc -i ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/bbq.ddcg
                  -ast ${CMAKE_CURRENT_BINARY_DIR}/bbq.json
                  -ir  ${CMAKE_CURRENT_BINARY_DIR}/bbq_ir.json
                  -L   ${CMAKE_CURRENT_SOURCE_DIR}/ddcgc/lib
                  -o   ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/bbq_compile.h
    DEPENDS ddcgc
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/cek/bbq.ddcg
            ${CMAKE_CURRENT_BINARY_DIR}/bbq.json
            ${CMAKE_CURRENT_BINARY_DIR}/bbq_ir.json
            ${CMAKE_CURRENT_SOURCE_DIR}/ddcgc/lib/dybvig.ddcg
    COMMENT "Generating bbq_compile.h from bbq.ddcg"
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



