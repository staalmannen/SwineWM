cmake_minimum_required(VERSION 3.25)
project(SwineWindowsShell LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "The native shell bridge requires Windows.")
endif()

add_library(swine_win32_bridge STATIC
  src/window_bridge.cpp
)
target_include_directories(swine_win32_bridge PUBLIC include)
target_compile_features(swine_win32_bridge PUBLIC cxx_std_20)
target_compile_definitions(swine_win32_bridge PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(swine_win32_bridge PUBLIC user32)

