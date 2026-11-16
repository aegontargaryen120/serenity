cmake_minimum_required(VERSION 3.20)

# Prefer the user's Homebrew LLVM toolchain on Apple Silicon. The C++ driver
# must be clang++, not clang, so the C++ standard library is linked correctly.
if(EXISTS "/opt/homebrew/opt/llvm/bin/clang" AND
   EXISTS "/opt/homebrew/opt/llvm/bin/clang++")
    set(CMAKE_C_COMPILER "/opt/homebrew/opt/llvm/bin/clang")
    set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
endif()

project(serenity VERSION 0.12.0 LANGUAGES C CXX)

option(SERENITY_REQUIRE_LLVM
    "Fail configuration unless LLVM development libraries for AOT/JIT are available"
    OFF
)

set(SERENITY_MACOS_MIN_VERSION "13.0" CACHE STRING
    "Minimum macOS deployment target for directly linked Serenity executables")

# Homebrew LLVM installs its CMake package beneath this prefix.
if(EXISTS "/opt/homebrew/opt/llvm/lib/cmake/llvm")
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/llvm")
endif()
find_package(LLVM CONFIG QUIET)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(SERENITY_REQUIRE_LLVM AND NOT LLVM_FOUND)
    message(FATAL_ERROR
        "LLVM development package not found. Set LLVM_DIR to the directory "
        "containing LLVMConfig.cmake (Homebrew: /opt/homebrew/opt/llvm/lib/cmake/llvm).")
endif()

function(serenity_link_llvm target)
    if(LLVM_FOUND)
        target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
        target_compile_definitions(${target} PRIVATE SERENITY_HAS_LLVM=1)
        separate_arguments(SERENITY_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
        target_compile_options(${target} PRIVATE ${SERENITY_LLVM_DEFINITIONS})
        if(TARGET LLVM)
            target_link_libraries(${target} PRIVATE LLVM)
        else()
            llvm_map_components_to_libnames(SERENITY_LLVM_LIBS
                Core Support OrcJIT ExecutionEngine RuntimeDyld Object
                native nativecodegen
                IRReader AsmParser Analysis TransformUtils ScalarOpts InstCombine
                IPO Vectorize Passes CodeGen MC Target TargetParser AsmPrinter)
            target_link_libraries(${target} PRIVATE ${SERENITY_LLVM_LIBS})
        endif()
    else()
        target_compile_definitions(${target} PRIVATE SERENITY_HAS_LLVM=0)
    endif()
endfunction()

if(LLVM_FOUND)
    message(STATUS "Serenity native backend: LLVM ${LLVM_PACKAGE_VERSION} found at ${LLVM_DIR}")
else()
    message(WARNING "LLVM development package not found: snjit and native AOT object generation will be unavailable. sninterp remains fully usable.")
endif()

set(SERENITY_MACOS_SDKROOT "")
set(SERENITY_MACOS_SDK_VERSION "")
if(APPLE)
    if(EXISTS "/usr/bin/ld")
        set(SERENITY_LINKER_EXECUTABLE "/usr/bin/ld")
    else()
        find_program(SERENITY_LINKER_EXECUTABLE NAMES ld)
    endif()

    # Honor an explicit CMake sysroot first; otherwise ask Xcode/Command Line
    # Tools for the active macOS SDK.
    if(CMAKE_OSX_SYSROOT AND IS_DIRECTORY "${CMAKE_OSX_SYSROOT}")
        set(SERENITY_MACOS_SDKROOT "${CMAKE_OSX_SYSROOT}")
    endif()

    find_program(SERENITY_XCRUN_EXECUTABLE NAMES xcrun)
    if(SERENITY_XCRUN_EXECUTABLE)
        if(NOT SERENITY_MACOS_SDKROOT)
            execute_process(
                COMMAND "${SERENITY_XCRUN_EXECUTABLE}" --sdk macosx --show-sdk-path
                OUTPUT_VARIABLE SERENITY_MACOS_SDKROOT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE SERENITY_XCRUN_PATH_RESULT
            )
            if(NOT SERENITY_XCRUN_PATH_RESULT EQUAL 0)
                set(SERENITY_MACOS_SDKROOT "")
            endif()
        endif()
        execute_process(
            COMMAND "${SERENITY_XCRUN_EXECUTABLE}" --sdk macosx --show-sdk-version
            OUTPUT_VARIABLE SERENITY_MACOS_SDK_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE SERENITY_XCRUN_VERSION_RESULT
        )
        if(NOT SERENITY_XCRUN_VERSION_RESULT EQUAL 0)
            set(SERENITY_MACOS_SDK_VERSION "")
        endif()
    endif()
endif()

add_library(serenity_lexer src/lexer/lexer.cpp)
target_include_directories(serenity_lexer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(serenity_preprocessor src/preprocessor/preprocessor.cpp)
target_include_directories(serenity_preprocessor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(serenity_preprocessor PRIVATE
    SERENITY_STDLIB_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/stdlib"
)

add_library(serenity_ast INTERFACE)
target_include_directories(serenity_ast INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(serenity_ast INTERFACE serenity_lexer)

add_library(serenity_parser src/parser/parser.cpp)
target_include_directories(serenity_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(serenity_parser PUBLIC serenity_ast)

add_library(serenity_semantic src/semantic/analyzer.cpp)
target_include_directories(serenity_semantic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(serenity_semantic PUBLIC serenity_parser)

add_library(serenity_bytecode
    src/bytecode/instruction.cpp
    src/bytecode/module.cpp
    src/bytecode/generator.cpp
)
target_include_directories(serenity_bytecode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(serenity_bytecode PUBLIC serenity_semantic)

add_library(serenity_interpreter src/interpreter/interpreter.cpp)
target_include_directories(serenity_interpreter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(serenity_interpreter PUBLIC serenity_bytecode)

# The native runtime has a stable C ABI and deliberately does not depend on LLVM.
# The compiler links this archive into each generated native executable.
add_library(serenity_runtime STATIC src/runtime/runtime.cpp)
target_include_directories(serenity_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Serenity emits textual LLVM IR, then parses, optimizes, and lowers it through
# LLVM's C++ APIs. The platform linker is invoked directly for final executable
# linking; clang/clang++ are not spawned by the Serenity compiler.
add_library(serenity_aot
    src/aot/llvm_emitter.cpp
    src/aot/llvm_backend.cpp
)
target_include_directories(serenity_aot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(serenity_aot PUBLIC serenity_bytecode)
serenity_link_llvm(serenity_aot)

add_executable(sninterp src/main/sninterp.cpp)
target_link_libraries(sninterp PRIVATE serenity_interpreter serenity_preprocessor)

add_library(serenity_jit src/jit/jit.cpp)
target_include_directories(serenity_jit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(serenity_jit PUBLIC serenity_interpreter)
serenity_link_llvm(serenity_jit)

add_executable(snjit src/main/snjit.cpp)
target_link_libraries(snjit PRIVATE serenity_jit serenity_preprocessor)

add_executable(serenity src/main/serenity.cpp)
target_link_libraries(serenity PRIVATE serenity_aot serenity_preprocessor)
target_compile_definitions(serenity PRIVATE
    SERENITY_RUNTIME_LIBRARY_PATH="$<TARGET_FILE:serenity_runtime>"
    SERENITY_LINKER_PATH="${SERENITY_LINKER_EXECUTABLE}"
    SERENITY_MACOS_SDKROOT="${SERENITY_MACOS_SDKROOT}"
    SERENITY_MACOS_SDK_VERSION="${SERENITY_MACOS_SDK_VERSION}"
    SERENITY_MACOS_MIN_VERSION="${SERENITY_MACOS_MIN_VERSION}"
)
add_dependencies(serenity serenity_runtime)

set(SERENITY_WARNING_TARGETS
    serenity_lexer serenity_preprocessor serenity_parser serenity_semantic
    serenity_bytecode serenity_interpreter serenity_runtime serenity_aot
    serenity_jit sninterp snjit serenity
)
foreach(target IN LISTS SERENITY_WARNING_TARGETS)
    target_compile_options(${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endforeach()

add_executable(serenity_lexer_tests tests/test_lexer.cpp)
target_link_libraries(serenity_lexer_tests PRIVATE serenity_lexer)

add_executable(serenity_preprocessor_tests tests/test_preprocessor.cpp)
target_link_libraries(serenity_preprocessor_tests PRIVATE serenity_preprocessor)

add_executable(serenity_ast_tests tests/test_ast.cpp)
target_link_libraries(serenity_ast_tests PRIVATE serenity_ast)

add_executable(serenity_parser_tests tests/test_parser.cpp)
target_link_libraries(serenity_parser_tests PRIVATE serenity_parser)

add_executable(serenity_semantic_tests tests/test_semantic.cpp)
target_link_libraries(serenity_semantic_tests PRIVATE serenity_semantic)

add_executable(serenity_bytecode_tests tests/test_bytecode.cpp)
target_link_libraries(serenity_bytecode_tests PRIVATE serenity_bytecode)

add_executable(serenity_interpreter_tests tests/test_interpreter.cpp)
target_link_libraries(serenity_interpreter_tests PRIVATE serenity_interpreter)

add_executable(serenity_stdlib_tests tests/test_stdlib.cpp)
target_link_libraries(serenity_stdlib_tests PRIVATE
    serenity_preprocessor serenity_lexer serenity_parser serenity_semantic
    serenity_bytecode serenity_interpreter)
target_compile_definitions(serenity_stdlib_tests PRIVATE
    SERENITY_STDLIB_TEST_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/examples/stdlib_demo.sn"
)

add_executable(serenity_io_tests tests/test_io.cpp)
target_link_libraries(serenity_io_tests PRIVATE
    serenity_preprocessor serenity_lexer serenity_parser serenity_semantic
    serenity_bytecode serenity_interpreter)
target_compile_definitions(serenity_io_tests PRIVATE
    SERENITY_IO_TEST_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/examples/io_demo.sn")

add_executable(serenity_aot_tests tests/test_aot.cpp)
target_link_libraries(serenity_aot_tests PRIVATE serenity_aot)

if(LLVM_FOUND)
    add_executable(serenity_aot_backend_tests tests/test_llvm_backend.cpp)
    target_link_libraries(serenity_aot_backend_tests PRIVATE serenity_aot)
    target_compile_options(serenity_aot_backend_tests PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endif()

add_executable(serenity_jit_tests tests/test_jit.cpp)
target_link_libraries(serenity_jit_tests PRIVATE serenity_jit)

foreach(target serenity_ast_tests serenity_parser_tests serenity_semantic_tests
               serenity_bytecode_tests serenity_interpreter_tests serenity_stdlib_tests
               serenity_io_tests serenity_aot_tests serenity_jit_tests)
    target_compile_options(${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endforeach()

enable_testing()
add_test(NAME serenity_lexer_tests COMMAND serenity_lexer_tests)
add_test(NAME serenity_preprocessor_tests COMMAND serenity_preprocessor_tests)
add_test(NAME serenity_ast_tests COMMAND serenity_ast_tests)
add_test(NAME serenity_parser_tests COMMAND serenity_parser_tests)
add_test(NAME serenity_semantic_tests COMMAND serenity_semantic_tests)
add_test(NAME serenity_bytecode_tests COMMAND serenity_bytecode_tests)
add_test(NAME serenity_interpreter_tests COMMAND serenity_interpreter_tests)
add_test(NAME serenity_stdlib_tests COMMAND serenity_stdlib_tests)
add_test(NAME serenity_io_tests COMMAND serenity_io_tests)
add_test(NAME serenity_aot_tests COMMAND serenity_aot_tests)
add_test(NAME serenity_jit_tests COMMAND serenity_jit_tests)
if(LLVM_FOUND)
    add_test(NAME serenity_aot_backend_tests COMMAND serenity_aot_backend_tests)
endif()
if(LLVM_FOUND AND APPLE)
    add_test(NAME serenity_aot_smoke
        COMMAND ${CMAKE_COMMAND}
            -DSERENITY=$<TARGET_FILE:serenity>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/aot_demo.sn
            -DSNINTERP=$<TARGET_FILE:sninterp>
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/serenity_aot_smoke
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_aot_smoke.cmake
    )
    add_test(NAME serenity_stdlib_aot_smoke
        COMMAND ${CMAKE_COMMAND}
            -DSERENITY=$<TARGET_FILE:serenity>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/stdlib_demo.sn
            -DSNINTERP=$<TARGET_FILE:sninterp>
            -DSNJIT=$<TARGET_FILE:snjit>
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/serenity_stdlib_aot_smoke
            -DEXPECTED_FILE=${CMAKE_CURRENT_SOURCE_DIR}/examples/stdlib_demo.expected
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_aot_smoke.cmake
    )
endif()
