cmake_minimum_required(VERSION 3.20)
project("PpcAsm" CXX)

# Assembles the PowerPC test sources XenonRecomp's test mode consumes, using the
# opcode tables the project already carries for disassembly.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(PpcAsm "main.cpp")
target_link_libraries(PpcAsm PRIVATE XenonUtils)
target_compile_definitions(PpcAsm PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
