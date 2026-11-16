cmake_minimum_required(VERSION 3.20)

# Prefer the user's Homebrew LLVM toolchain on Apple Silicon. The C++ driver
# must be clang++, not clang, so the C++ standard library is linked correctly.
if(EXISTS "/opt/homebrew/opt/llvm/bin/clang" AND
   EXISTS "/opt/homebrew/opt/llvm/bin/clang++")
    set(CMAKE_C_COMPILER "/opt/homebrew/opt/llvm/bin/clang")
    set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
endif()

project(serenity VERSION 0.8.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(SERENITY_CLANGXX_EXECUTABLE
    NAMES clang++
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
)
if(NOT SERENITY_CLANGXX_EXECUTABLE)
    set(SERENITY_CLANGXX_EXECUTABLE "clang++")
endif()

add_library(serenity_lexer src/lexer/lexer.cpp)
target_include_directories(serenity_lexer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(serenity_preprocessor src/preprocessor/preprocessor.cpp)
target_include_directories(serenity_preprocessor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

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

# Serenity emits textual LLVM IR and uses the installed clang++ driver to lower
# and link it. This avoids coupling the compiler binary to LLVM's C++ library ABI.
add_library(serenity_aot src/aot/llvm_emitter.cpp)
target_include_directories(serenity_aot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(serenity_aot PUBLIC serenity_bytecode)

add_executable(sninterp src/main/sninterp.cpp)
target_link_libraries(sninterp PRIVATE serenity_interpreter serenity_preprocessor)

add_executable(serenity src/main/serenity.cpp)
target_link_libraries(serenity PRIVATE serenity_aot serenity_preprocessor)
target_compile_definitions(serenity PRIVATE
    SERENITY_CLANGXX_PATH="${SERENITY_CLANGXX_EXECUTABLE}"
    SERENITY_RUNTIME_LIBRARY_PATH="$<TARGET_FILE:serenity_runtime>"
)
add_dependencies(serenity serenity_runtime)

set(SERENITY_WARNING_TARGETS
    serenity_lexer serenity_preprocessor serenity_parser serenity_semantic
    serenity_bytecode serenity_interpreter serenity_runtime serenity_aot
    sninterp serenity
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

add_executable(serenity_aot_tests tests/test_aot.cpp)
target_link_libraries(serenity_aot_tests PRIVATE serenity_aot)

foreach(target serenity_ast_tests serenity_parser_tests serenity_semantic_tests
               serenity_bytecode_tests serenity_interpreter_tests serenity_aot_tests)
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
add_test(NAME serenity_aot_tests COMMAND serenity_aot_tests)
add_test(NAME serenity_aot_smoke
    COMMAND ${CMAKE_COMMAND}
        -DSERENITY=$<TARGET_FILE:serenity>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/aot_demo.sn
        -DSNINTERP=$<TARGET_FILE:sninterp>
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/serenity_aot_smoke
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_aot_smoke.cmake
)
