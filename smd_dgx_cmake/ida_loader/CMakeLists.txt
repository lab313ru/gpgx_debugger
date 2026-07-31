# ---- Sega Genesis / Mega Drive ROM loader for IDA 9.2+ (optional) ----
#
# Gated on the same IDA_SDK_DIR as the debugger plugin. Loaders live in
# <IDA>/loaders/, not plugins/. No Qt and no emulator here — this module only
# builds the database (segments, header, vector table) that the debugger
# plugin then attaches to.

if (NOT IDA_SDK_DIR OR NOT IDA_LIB)
  return()
endif()

# Sibling subdirectories do not share variables, so the platform conventions
# have to be included here too rather than inherited from ida_plugin/.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ida_platform.cmake")

add_library(smd_dgx_ldr SHARED
  smd_dgx_ldr.cpp
  smd_dgx_ldr.h
)
target_include_directories(smd_dgx_ldr PRIVATE "${IDA_SDK_DIR}/include")
target_compile_definitions(smd_dgx_ldr PRIVATE
  ${IDA_PLATFORM_DEF} __X64__ __EA64__
  NO_OBSOLETE_FUNCS
)
target_link_libraries(smd_dgx_ldr PRIVATE "${IDA_LIB}")
set_target_properties(smd_dgx_ldr PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  OUTPUT_NAME "smd_dgx_ldr"
  PREFIX ""
  SUFFIX "${IDA_MODULE_SUFFIX}"
)

if (IDA_INSTALL_DIR)
  add_custom_command(TARGET smd_dgx_ldr POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            -DSRC=$<TARGET_FILE:smd_dgx_ldr> -DDST=${IDA_INSTALL_DIR}/loaders
            -P "${CMAKE_CURRENT_LIST_DIR}/../cmake/install_ida_module.cmake"
    COMMENT "Copy loader to IDA loaders dir")
endif()

# ---- Z80 sound-driver loader ----
#
# Builds the 64K Z80 view (8K RAM, the YM2612/PSG/bank ports, the $8000 window)
# for a driver dumped out of a running game. Its database is the one the Z80
# half of the debugger plugin attaches with.
add_library(smd_dgx_z80_ldr SHARED
  z80_loader.cpp
  z80_loader.h
)
target_include_directories(smd_dgx_z80_ldr PRIVATE "${IDA_SDK_DIR}/include")
target_compile_definitions(smd_dgx_z80_ldr PRIVATE
  ${IDA_PLATFORM_DEF} __X64__ __EA64__
  NO_OBSOLETE_FUNCS
)
target_link_libraries(smd_dgx_z80_ldr PRIVATE "${IDA_LIB}")
set_target_properties(smd_dgx_z80_ldr PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  OUTPUT_NAME "smd_dgx_z80_ldr"
  PREFIX ""
  SUFFIX "${IDA_MODULE_SUFFIX}"
)

if (IDA_INSTALL_DIR)
  add_custom_command(TARGET smd_dgx_z80_ldr POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            -DSRC=$<TARGET_FILE:smd_dgx_z80_ldr> -DDST=${IDA_INSTALL_DIR}/loaders
            -P "${CMAKE_CURRENT_LIST_DIR}/../cmake/install_ida_module.cmake"
    COMMENT "Copy Z80 loader to IDA loaders dir")
endif()
