cmake_minimum_required(VERSION 3.20)
project(Avant VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Avant WIN32 src/main.cpp)
target_compile_definitions(Avant PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(Avant PRIVATE winhttp shlwapi shell32 comctl32)
if(MSVC)
  target_compile_options(Avant PRIVATE /W4 /utf-8)
endif()
