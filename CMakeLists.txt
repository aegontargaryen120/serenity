cmake_minimum_required(VERSION 3.20)

project(serenity_lexer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(serenity_lexer
    src/lexer/lexer.cpp
)

target_include_directories(serenity_lexer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(serenity_lexer
    PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(serenity_lexer_tests
    tests/test_lexer.cpp
)

target_link_libraries(serenity_lexer_tests PRIVATE serenity_lexer)

enable_testing()
add_test(NAME serenity_lexer_tests COMMAND serenity_lexer_tests)
