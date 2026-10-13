cmake_minimum_required(VERSION 3.25)
project(SwayHwnd LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "sway-hwnd requires Windows.")
endif()

# Reuse the project-owned HWND and layout libraries. This native shell target
# does not compile or modify Sway's Wayland compositor sources.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../windows
                 ${CMAKE_CURRENT_BINARY_DIR}/windows-platform)

add_executable(sway-hwnd src/main.cpp)
target_compile_features(sway-hwnd PRIVATE cxx_std_20)
target_compile_definitions(sway-hwnd PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(sway-hwnd PRIVATE swine_win32_bridge user32)
