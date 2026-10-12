cmake_minimum_required(VERSION 3.20)
project("XenosDis" CXX)

# Reads the shader microcode the title uploads. Standalone and dependency free
# so it can be run over a captured program without building anything else.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(XenosDis "main.cpp")
target_compile_definitions(XenosDis PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
