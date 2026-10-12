cmake_minimum_required(VERSION 3.20)
project("CoD3Scan")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Pull in XenonRecomp's libraries (XenonUtils, LibXenonAnalyse, fmt, disasm...).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../XenonRecomp ${CMAKE_BINARY_DIR}/XenonRecomp)

add_executable(CoD3Scan "main.cpp")
target_link_libraries(CoD3Scan PRIVATE XenonUtils LibXenonAnalyse fmt::fmt)
target_compile_definitions(CoD3Scan PRIVATE _CRT_SECURE_NO_WARNINGS)
