cmake_minimum_required (VERSION 3.20)

# Pick up vcpkg when the caller has not chosen a toolchain and VCPKG_ROOT is
# set. This cannot live in the presets: a preset cache variable is expanded
# unconditionally, so "$env{VCPKG_ROOT}/scripts/..." collapses to an invalid
# path — and a hard configure error — on a machine that has no vcpkg. Here it
# is simply skipped, and dependencies resolve from the system instead, which is
# how a Linux build normally wants to work.
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "Vcpkg toolchain file")
endif()

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("smd_dgx" LANGUAGES C CXX)

set(GX_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

include(cmake/gx_options.cmake)
include(cmake/gx_core.cmake)
include(cmake/gx_libchdr.cmake)

add_library(gx_platform INTERFACE)
target_include_directories(gx_platform INTERFACE "frontends/sdl" "frontends/core")

gx_add_core(gx_core "${GX_ROOT}")

target_link_libraries(gx_core PUBLIC gx_platform)

# zlib: build the copy already vendored in the emulator tree (libchdr deps)
# instead of an external one. Keeps every artifact self-contained — the IDA
# plugin in particular must not depend on a zlib1.dll that IDA does not ship —
# and drops one platform dependency.
file(GLOB GX_ZLIB_SOURCES "${GX_ROOT}/core/cd_hw/libchdr/deps/zlib-1.3.1/*.c")
add_library(gx_zlib STATIC ${GX_ZLIB_SOURCES})
target_include_directories(gx_zlib PUBLIC "${GX_ROOT}/core/cd_hw/libchdr/deps/zlib-1.3.1")
target_compile_definitions(gx_zlib PRIVATE _CRT_SECURE_NO_WARNINGS)
add_library(ZLIB::ZLIB ALIAS gx_zlib)

target_link_libraries(gx_core PRIVATE gx_zlib)

if (GX_ENABLE_CHD)
  gx_add_libchdr(gx_libchdr "${GX_ROOT}")
  target_link_libraries(gx_core PUBLIC gx_libchdr)
endif()

# Shared frontend code (config, error, fileio, save_state, unzip) - reused by SDL and Qt6
set(GX_FRONTEND_CORE "${CMAKE_CURRENT_LIST_DIR}/frontends/core")
add_library(frontend_common STATIC
  "${GX_FRONTEND_CORE}/config.c"
  "${GX_FRONTEND_CORE}/error.c"
  "${GX_FRONTEND_CORE}/fileio.c"
  "${GX_FRONTEND_CORE}/save_state_message.cpp"
  "${GX_FRONTEND_CORE}/gx/gx.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/frontends/sdl/unzip.c"
)
target_include_directories(frontend_common PUBLIC "${GX_FRONTEND_CORE}")
target_link_libraries(frontend_common PRIVATE gx_core gx_zlib)
set_target_properties(frontend_common PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
# std::filesystem may require explicit link on some toolchains (e.g. GCC with libstdc++)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL ""))
  target_link_libraries(frontend_common PRIVATE stdc++fs)
endif()

# ---- Headless debugger core (NO Qt) ----
# Emulator + debug backend + transport + host, statically linkable into any
# frontend: the standalone Qt app, and (phase 2) the IDA 9.2+ plugin.
# Depends only on the emulator core and frontend_common; zero UI/toolkit deps.
add_library(smd_dgx_emu STATIC
  "${CMAKE_CURRENT_LIST_DIR}/debugger/GpgxBackend.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/debugger/InProcTransport.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/debugger/EmuHost.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/debugger/BridgeServer.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/debugger/RemoteBackend.cpp"   # the client side of it
)
if (WIN32)
  target_link_libraries(smd_dgx_emu PUBLIC ws2_32)   # BridgeServer sockets
endif()
target_include_directories(smd_dgx_emu PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(smd_dgx_emu PUBLIC gx_core frontend_common)
set_target_properties(smd_dgx_emu PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

find_package(SDL2 CONFIG REQUIRED)

# ---- Audio device ----
#
# Its own target so the backend's dependency travels with it. Every host used
# to have to know that audio means winmm, which is precisely how the audio
# device ended up being the one thing keeping the hosts on Windows.
add_library(gx_audio STATIC "${CMAKE_CURRENT_LIST_DIR}/platform/AudioOutput.cpp")
target_include_directories(gx_audio PUBLIC "${CMAKE_CURRENT_LIST_DIR}/platform")
if (WIN32)
  target_link_libraries(gx_audio PRIVATE winmm)          # waveOut
else()
  target_link_libraries(gx_audio PRIVATE SDL2::SDL2)
endif()
set_target_properties(gx_audio PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES
                                          POSITION_INDEPENDENT_CODE ON)

# ---- SDL2 + ImGui frontend (optional) ----
#
# Optional because imgui ships no CMake config package on mainstream Linux
# distributions, and a hard find_package(imgui CONFIG REQUIRED) here aborted
# the configure before the Qt6 debugger — the target a Linux user actually
# wants — was ever reached.
option(SMD_DGX_BUILD_SDL_FRONTEND "Build the SDL2 + ImGui frontend" ON)
find_package(OpenGL QUIET)
find_package(imgui CONFIG QUIET)

if (SMD_DGX_BUILD_SDL_FRONTEND AND OpenGL_FOUND AND imgui_FOUND)
  set(GX_PLATFORM_SDL "${CMAKE_CURRENT_LIST_DIR}/frontends/sdl")
  add_library(gx_platform_lib STATIC "${GX_PLATFORM_SDL}/sdl.c")
  target_include_directories(gx_platform_lib PUBLIC "${GX_PLATFORM_SDL}" "${GX_FRONTEND_CORE}")
  target_link_libraries(gx_platform_lib PRIVATE gx_core frontend_common)

  add_executable(smd_dgx_cmake)
  target_sources(smd_dgx_cmake PRIVATE "frontends/sdl/osd.h" "frontends/sdl/main_sdl2.cpp")
  target_include_directories(smd_dgx_cmake PRIVATE "frontends/sdl" "frontends/core")
  target_link_libraries(smd_dgx_cmake PRIVATE gx_core gx_platform_lib frontend_common smd_dgx_emu SDL2::SDL2 OpenGL::GL imgui::imgui)
  set_target_properties(smd_dgx_cmake PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
elseif (SMD_DGX_BUILD_SDL_FRONTEND)
  message(STATUS "SDL frontend: skipped (needs OpenGL and an imgui CMake package)")
endif()

# ---- Qt6 frontend (optional) ----
find_package(Qt6 COMPONENTS Widgets Gui Core QUIET)
if (Qt6_FOUND)
  set(GX_PLATFORM_QT6 "${CMAKE_CURRENT_LIST_DIR}/frontends/qt6")

  add_library(gx_platform_qt6 STATIC "${GX_PLATFORM_QT6}/qt_input.c")
  target_include_directories(gx_platform_qt6 PUBLIC "${GX_PLATFORM_QT6}" "${GX_FRONTEND_CORE}")
  target_link_libraries(gx_platform_qt6 PRIVATE gx_core frontend_common)

  add_executable(smd_dgx_qt6)
  target_sources(smd_dgx_qt6 PRIVATE
    # entry point
    "${GX_PLATFORM_QT6}/main_qt6.cpp"
    # frontend
    "${GX_PLATFORM_QT6}/MainWindow.h"
    "${GX_PLATFORM_QT6}/MainWindow.cpp"
    "${GX_PLATFORM_QT6}/ViewSettings.h"
    "${GX_PLATFORM_QT6}/ViewSettings.cpp"
    # views (parity set with the Gens debug windows)
    "${GX_PLATFORM_QT6}/views/EmulatorScreen.h"
    "${GX_PLATFORM_QT6}/views/EmulatorScreen.cpp"
    "${GX_PLATFORM_QT6}/views/VdpRegView.h"
    "${GX_PLATFORM_QT6}/views/VdpRegView.cpp"
    "${GX_PLATFORM_QT6}/views/SaveStateView.h"
    "${GX_PLATFORM_QT6}/views/SaveStateView.cpp"
    "${GX_PLATFORM_QT6}/views/ScrollView.h"
    "${GX_PLATFORM_QT6}/views/ScrollView.cpp"
    "${GX_PLATFORM_QT6}/views/VdpRamView.h"
    "${GX_PLATFORM_QT6}/views/VdpRamView.cpp"
    "${GX_PLATFORM_QT6}/views/VdpSpritesView.h"
    "${GX_PLATFORM_QT6}/views/VdpSpritesView.cpp"
    "${GX_PLATFORM_QT6}/views/PlaneExplorerView.h"
    "${GX_PLATFORM_QT6}/views/PlaneExplorerView.cpp"
    "${GX_PLATFORM_QT6}/views/SoundDebugView.h"
    "${GX_PLATFORM_QT6}/views/SoundDebugView.cpp"
    "${GX_PLATFORM_QT6}/views/MemoryView.h"
    "${GX_PLATFORM_QT6}/views/MemoryView.cpp"
    "${GX_PLATFORM_QT6}/views/RamSearchView.h"
    "${GX_PLATFORM_QT6}/views/RamSearchView.cpp"
    "${GX_PLATFORM_QT6}/views/RamWatchView.h"
    "${GX_PLATFORM_QT6}/views/RamWatchView.cpp"
  )

  target_include_directories(smd_dgx_qt6 PRIVATE
    "${GX_PLATFORM_QT6}"
    "${GX_PLATFORM_QT6}/views"
    "${CMAKE_CURRENT_LIST_DIR}"          # for "debugger/..." includes
    "${CMAKE_CURRENT_LIST_DIR}/frontends/core"
  )

  target_link_libraries(smd_dgx_qt6 PRIVATE
    gx_core gx_platform_qt6 frontend_common smd_dgx_emu gx_audio
    Qt6::Widgets Qt6::Gui Qt6::Core
  )

  set_target_properties(smd_dgx_qt6 PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    AUTOMOC ON
  )

  if(WIN32)
    set_target_properties(smd_dgx_qt6 PROPERTIES WIN32_EXECUTABLE TRUE)
    get_filename_component(_qt6_root "${Qt6_DIR}/../.." REALPATH)
    add_custom_command(TARGET smd_dgx_qt6 POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -DEXE_PATH="$<TARGET_FILE:smd_dgx_qt6>" -DQT_ROOT="${_qt6_root}" -P "${CMAKE_CURRENT_LIST_DIR}/cmake/deploy_qt.cmake"
      COMMENT "Deploy Qt DLLs and plugins to build folder"
    )
  endif()
endif()

# ---- IDA 9.2+ modules (optional, gated on IDA_SDK_DIR) ----
add_subdirectory(ida_plugin)   # debugger plugin; also locates IDA_LIB
add_subdirectory(ida_loader)   # ROM loader (uses IDA_LIB found above)

# ---- Tests ----
option(SMD_DGX_BUILD_TESTS "Build the test suite" ON)
if (SMD_DGX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()