cmake_minimum_required(VERSION 3.25)

project(TargetLines
    VERSION 0.1.0
    LANGUAGES CXX
)

# FFXI is a 32-bit process, so the module must be too. With MSVC that means
# configuring with -A Win32; there is no way to fix it from inside this file.
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "TargetLines must be built 32-bit. Configure with: cmake -A Win32 ..")
endif()

add_library(_TargetLines SHARED
    TargetLinesModule.cpp
    exports.def
)

set_target_properties(_TargetLines PROPERTIES PREFIX "")

# Broguypal's shared draw_scene bus. Every participant must build against a
# byte-identical copy of SceneHook.h -- the ABI version is checked at runtime and
# a mismatched module refuses to join rather than guess at the layout.
target_include_directories(_TargetLines PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../SceneHook")

target_compile_features(_TargetLines PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(_TargetLines PRIVATE /W4 /EHsc)
else()
    target_compile_options(_TargetLines PRIVATE -Wall -Wextra -Wpedantic -m32)
    target_link_options(_TargetLines PRIVATE -m32 -static -static-libgcc -static-libstdc++)
endif()
