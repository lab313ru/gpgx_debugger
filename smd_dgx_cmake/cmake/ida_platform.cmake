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
