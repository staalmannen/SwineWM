cmake_minimum_required(VERSION 3.20)
project(SwineWM LANGUAGES C VERSION 0.0.1)

# The portable core (tree, commands, config, IPC JSON) builds everywhere so
# that its unit tests run on Linux CI as well as on Windows. The executables
# need Windows (MSVC or MinGW-w64).

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(SWINEWM_BUILD_TESTS "Build the unit tests" ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_library(swine_common STATIC
  common/fileutil.c
  common/json.c
  common/json-parse.c
  common/list.c
  common/log.c
  common/stringop.c
)
target_include_directories(swine_common PUBLIC include)
if(WIN32)
  target_sources(swine_common PRIVATE common/ipc-client.c)
  target_compile_definitions(swine_common PRIVATE UNICODE _UNICODE)
  target_link_libraries(swine_common PUBLIC advapi32)
endif()

add_library(swinewm_core STATIC
  swinewm/bar/bar.c
  swinewm/bar/css.c
  swinewm/bar/format.c
  swinewm/commands.c
  swinewm/commands/bar.c
  swinewm/commands/bind.c
  swinewm/commands/exec.c
  swinewm/commands/floating.c
  swinewm/commands/focus.c
  swinewm/commands/gaps.c
  swinewm/commands/layout.c
  swinewm/commands/mode.c
  swinewm/commands/move.c
  swinewm/commands/notify.c
  swinewm/commands/resize.c
  swinewm/commands/session.c
  swinewm/commands/set.c
  swinewm/commands/settings.c
  swinewm/commands/split.c
  swinewm/commands/workspace.c
  swinewm/config.c
  swinewm/desktop/transaction.c
  swinewm/input/keyboard.c
  swinewm/input/seat.c
  swinewm/ipc-json.c
  swinewm/ipc-server.c
  swinewm/notify/notify.c
  swinewm/server.c
  swinewm/tree/arrange.c
  swinewm/tree/container.c
  swinewm/tree/node.c
  swinewm/tree/output.c
  swinewm/tree/root.c
  swinewm/tree/view.c
  swinewm/tree/workspace.c
)
target_link_libraries(swinewm_core PUBLIC swine_common)
if(NOT MSVC)
  target_link_libraries(swinewm_core PUBLIC m)
endif()

if(WIN32)
  add_executable(swinewm
    swinewm/main.c
    swinewm/win32/exec.c
    swinewm/win32/bar.c
    swinewm/win32/draw.c
    swinewm/win32/ipc.c
    swinewm/win32/keyboard.c
    swinewm/win32/notify.c
    swinewm/win32/output.c
    swinewm/win32/platform.c
    swinewm/win32/recover.c
    swinewm/win32/sysinfo.c
    swinewm/win32/util.c
    swinewm/win32/window.c
  )
  target_compile_definitions(swinewm PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN
    WINVER=0x0A00 _WIN32_WINNT=0x0A00)
  target_link_libraries(swinewm PRIVATE swinewm_core user32 gdi32 dwmapi shell32
    shcore advapi32)

  add_executable(swinemsg swinemsg/main.c)
  target_compile_definitions(swinemsg PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
  target_link_libraries(swinemsg PRIVATE swine_common)

  # Ship the default config next to the executable; swinewm falls back to
  # it when the user has none.
  add_custom_target(swinewm_default_config ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_SOURCE_DIR}/config.in $<TARGET_FILE_DIR:swinewm>/config
    DEPENDS config.in)
endif()

if(SWINEWM_BUILD_TESTS)
  enable_testing()
  add_executable(swinewm_tests
    tests/fake_platform.c
    tests/test_main.c
    tests/test_commands.c
    tests/test_common.c
    tests/test_config.c
    tests/test_tree.c
    tests/test_bar.c
    tests/test_notify.c
  )
  target_link_libraries(swinewm_tests PRIVATE swinewm_core)
  add_test(NAME swinewm_tests COMMAND swinewm_tests)
endif()
if(SWINEWM_BUILD_TESTS)
  target_compile_definitions(swinewm_tests PRIVATE
    SWINEWM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()
