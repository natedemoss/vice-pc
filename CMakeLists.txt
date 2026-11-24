cmake_minimum_required(VERSION 3.20)
project(vice_pc LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "vice-pc needs MSVC targeting x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Shared CRT, same as the game uses, so both share one UCRT/VCRuntime instance.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_executable(vice-pc
  src/main.cpp
  src/inflate.cpp
  src/log.cpp
  src/pe_loader.cpp
  src/stubs.cpp
  src/tls.cpp
  src/hle/hle.cpp
  src/hle/xmem.cpp
  src/hle/vcruntime.cpp
  src/hle/d3d12_xs.cpp
  src/hle/probe.cpp
  src/hle/xcrdapi.cpp
  src/hle/xg_xs.cpp
  src/hle/crt.cpp
  src/hle/kernel.cpp
  src/hle/d3d12/vtpatch.cpp
  src/hle/d3d12/device.cpp
  src/hle/d3d12/dxgi.cpp
  src/hle/d3d12/gpumem.cpp
  src/hle/d3d12/cmdlist.cpp
)
target_compile_definitions(vice-pc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
# /Zc:threadSafeInit-: thread-safe statics use implicit TLS, and the game owns TLS slot 0 (see src/tls.cpp).
target_compile_options(vice-pc PRIVATE /W4 /permissive- /Zc:threadSafeInit-)
target_link_libraries(vice-pc PRIVATE psapi d3d12 dxgi dxguid)
# Keep the host out of the game's preferred range (0x140000000+).
target_link_options(vice-pc PRIVATE /BASE:0x10000000 /DYNAMICBASE:NO)

# ---- tests -------------------------------------------------------------------
# fake_game.exe stands in for a console title: it imports from an "Xbox-only" DLL,
# uses thread_local from several threads, and throws C++ exceptions.
include(CTest)
if(BUILD_TESTING)
  add_library(fake_xbox SHARED tests/fake_xbox.cpp)
  add_executable(fake_game tests/fake_game.cpp)
  add_executable(inflate_test tests/inflate_test.cpp src/inflate.cpp)
  add_test(NAME inflate COMMAND inflate_test)
  target_link_libraries(fake_game PRIVATE fake_xbox)
  target_link_options(fake_game PRIVATE /BASE:0x140000000)

  add_test(NAME loads_fake_game
    COMMAND vice-pc $<TARGET_FILE:fake_game> --stub-dll fake_xbox.dll --log fake_game.log)
  set_tests_properties(loads_fake_game PROPERTIES PASS_REGULAR_EXPRESSION "FAKE GAME: all checks passed")

  add_test(NAME no_run_mode
    COMMAND vice-pc $<TARGET_FILE:fake_game> --no-run --stub-dll fake_xbox.dll --log no_run.log)
  set_tests_properties(no_run_mode PROPERTIES PASS_REGULAR_EXPRESSION "not executing")

  add_test(NAME stop_on_stub
    COMMAND vice-pc $<TARGET_FILE:fake_game> --stop-on-stub --stub-dll fake_xbox.dll --log stop.log)
  set_tests_properties(stop_on_stub PROPERTIES PASS_REGULAR_EXPRESSION "fake_xbox.dll!XFakeXboxOnly called from game\\+0x")
endif()
