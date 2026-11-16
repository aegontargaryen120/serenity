cmake_minimum_required(VERSION 3.20)

project(serenity_frontend VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_COMPILER "/opt/homebrew/opt/llvm/bin/clang")
set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")

add_library(serenity_lexer
    src/lexer/lexer.cpp
)

target_include_directories(serenity_lexer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(serenity_preprocessor
    src/preprocessor/preprocessor.cpp
)

target_include_directories(serenity_preprocessor
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

foreach(target serenity_lexer serenity_preprocessor)
    target_compile_options(${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endforeach()

add_executable(serenity_lexer_tests
    tests/test_lexer.cpp
)
target_link_libraries(serenity_lexer_tests PRIVATE serenity_lexer)

add_executable(serenity_preprocessor_tests
    tests/test_preprocessor.cpp
)
target_link_libraries(serenity_preprocessor_tests PRIVATE serenity_preprocessor)

enable_testing()
add_test(NAME serenity_lexer_tests COMMAND serenity_lexer_tests)
add_test(NAME serenity_preprocessor_tests COMMAND serenity_preprocessor_tests)
