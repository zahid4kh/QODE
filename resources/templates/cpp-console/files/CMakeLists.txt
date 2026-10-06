cmake_minimum_required(VERSION 3.18)
project({{nameId}} VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD {{std}})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# clangd reads build/compile_commands.json to know your compiler flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

{{#if ccache}}
# ccache: reuse earlier compiler output for unchanged code (only if it is installed).
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

{{/if}}
{{#if mold}}
# mold: a much faster linker (only if the compiler can use it).
include(CheckLinkerFlag)
check_linker_flag(CXX -fuse-ld=mold HAVE_MOLD)
if(HAVE_MOLD)
  add_link_options(-fuse-ld=mold)
endif()

{{/if}}
# Every .cpp under src/ except main.cpp is part of the library, so the tests can use it too.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(core STATIC ${SOURCES})
target_include_directories(core PUBLIC include)
target_compile_options(core PUBLIC -Wall -Wextra -Wpedantic)

add_executable({{nameId}} src/main.cpp)
target_link_libraries({{nameId}} PRIVATE core)
{{#if tests}}

enable_testing()
add_executable(tests tests/test_main.cpp)
target_link_libraries(tests PRIVATE core)
add_test(NAME tests COMMAND tests)
{{/if}}
