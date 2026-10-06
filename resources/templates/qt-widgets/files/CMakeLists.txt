cmake_minimum_required(VERSION 3.21)
project({{nameId}} VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable({{nameId}}
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
)
{{#if resources}}
qt_add_resources({{nameId}} app
    PREFIX "/"
    BASE resources
    FILES resources/icon.svg
)
{{/if}}
target_include_directories({{nameId}} PRIVATE src)
target_link_libraries({{nameId}} PRIVATE Qt6::Widgets)
