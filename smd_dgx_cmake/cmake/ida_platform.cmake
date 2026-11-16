# Per-platform IDA SDK conventions, shared by the plugin and the loaders.
#
# This lives in its own module because the two live in sibling subdirectories:
# variables set inside one add_subdirectory() are invisible to the other, so
# defining them next to the plugin left the loaders with an empty suffix and no
# platform define. An extension-less module is the worst kind of build error —
# it links fine and IDA silently ignores it.
#
# Sets: IDA_LIB_HINTS, IDA_PLATFORM_DEF, IDA_MODULE_SUFFIX, IDA_QT_LIB_DIR

if (WIN32)
  set(IDA_LIB_HINTS "${IDA_SDK_DIR}/lib/x64_win_vc_64"        # licensed SDK layout
                    "${IDA_SDK_DIR}/lib/x64_win_vc_64_teams"
                    "${IDA_SDK_DIR}/lib/x64_win_64")          # open-source ida-sdk
  set(IDA_PLATFORM_DEF __NT__)
  set(IDA_MODULE_SUFFIX ".dll")
  set(IDA_QT_LIB_DIR "${IDA_SDK_DIR}/lib/x64_win_qt")
elseif (APPLE)
  set(IDA_LIB_HINTS "${IDA_SDK_DIR}/lib/arm64_mac_clang_64"
                    "${IDA_SDK_DIR}/lib/x64_mac_clang_64"
                    "${IDA_SDK_DIR}/lib/arm64_mac_64"
                    "${IDA_SDK_DIR}/lib/x64_mac_64")
  set(IDA_PLATFORM_DEF __MAC__)
  set(IDA_MODULE_SUFFIX ".dylib")
  set(IDA_QT_LIB_DIR "${IDA_SDK_DIR}/lib/arm64_mac_qt" "${IDA_SDK_DIR}/lib/x64_mac_qt")
else()
  set(IDA_LIB_HINTS "${IDA_SDK_DIR}/lib/x64_linux_gcc_64"
                    "${IDA_SDK_DIR}/lib/x64_linux_64")
  set(IDA_PLATFORM_DEF __LINUX__)
  set(IDA_MODULE_SUFFIX ".so")
  set(IDA_QT_LIB_DIR "${IDA_SDK_DIR}/lib/x64_linux_qt")
endif()

if (NOT IDA_MODULE_SUFFIX)
  message(FATAL_ERROR "ida_platform: no module suffix for this platform")
endif()

# ida_module_exports(<target> <PLUGIN|LDSC>)
#
# Link a module the way the SDK's own makefiles do on Linux (module.mak,
# allmake.mak). ELF exports every non-static symbol by default, so without
# this the plugin would publish the whole emulator, Qt views and all, into
# IDA's process, where it can bind to — or be bound by — another module's
# symbol of the same name. The version script leaves exactly one entry point;
# hidden visibility keeps the module's own code from going through the PLT;
# the rpath finds libida.so one level up, from plugins/ or loaders/.
function(ida_module_exports target entry)
  if (WIN32 OR APPLE)
    return()   # Windows exports by declaration; macOS is not covered here
  endif()
  if (entry STREQUAL "PLUGIN")
    set(_def "${IDA_SDK_DIR}/plugins/exports.def")
  else()
    set(_def "${IDA_SDK_DIR}/ldr/exports.def")
  endif()
  if (NOT EXISTS "${_def}")
    # Same content as the SDK's file, for SDK copies that leave it out.
    set(_def "${CMAKE_CURRENT_BINARY_DIR}/${target}_exports.def")
    file(WRITE "${_def}" "{\nglobal: ${entry};\nlocal: *;\n};\n")
  endif()
  set_target_properties(${target} PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    BUILD_WITH_INSTALL_RPATH ON     # no build-tree paths baked into the module
    INSTALL_RPATH "\$ORIGIN/.."
    LINK_DEPENDS "${_def}"
  )
  target_link_options(${target} PRIVATE
    "LINKER:--version-script=${_def}" "LINKER:-z,origin")
endfunction()
