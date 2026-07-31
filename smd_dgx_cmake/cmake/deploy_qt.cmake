# Run windeployqt with PATH set so it finds Qt and ICU DLLs.
# Usage: cmake -DEXE_PATH=<path/to/exe> -DQT_ROOT=<qt root> -P deploy_qt.cmake
# Supports both layouts:
#   vcpkg:       QT_ROOT=vcpkg_installed/x64-windows  -> tools/Qt6/bin/windeployqt6.exe (+ qtpaths.debug.bat)
#   official Qt: QT_ROOT=C:/Qt/x.y.z/msvcYYYY_64[/lib] -> bin/windeployqt.exe
if(NOT EXE_PATH OR NOT QT_ROOT)
  message(FATAL_ERROR "deploy_qt.cmake requires EXE_PATH and QT_ROOT")
endif()

# CMakeLists passes Qt6_DIR/../.. which is <root>/lib for the official layout — normalize.
if(NOT EXISTS "${QT_ROOT}/bin" AND EXISTS "${QT_ROOT}/../bin")
  get_filename_component(QT_ROOT "${QT_ROOT}/.." REALPATH)
endif()

set(qt_debug_bin "${QT_ROOT}/debug/bin")
set(qt_bin "${QT_ROOT}/bin")
set(qt_tools_bin "${QT_ROOT}/tools/Qt6/bin")

find_program(windeployqt_exe
  NAMES windeployqt6.exe windeployqt.exe windeployqt
  HINTS "${qt_tools_bin}" "${qt_bin}"
  NO_DEFAULT_PATH
)
if(NOT windeployqt_exe)
  message(FATAL_ERROR "windeployqt not found under ${qt_tools_bin} or ${qt_bin}")
endif()

# vcpkg's windeployqt needs an explicit qtpaths for the debug tree; official Qt does not.
set(deploy_args "")
if(EXISTS "${qt_tools_bin}/qtpaths.debug.bat")
  list(APPEND deploy_args --qtpaths "${qt_tools_bin}/qtpaths.debug.bat")
endif()

# Set PATH so child process finds Qt/ICU DLLs (execute_process has no ENVIRONMENT option)
set(ENV{PATH} "${qt_debug_bin};${qt_bin};$ENV{PATH}")
get_filename_component(exe_dir "${EXE_PATH}" DIRECTORY)
execute_process(
  COMMAND "${windeployqt_exe}" ${deploy_args} "${EXE_PATH}"
  WORKING_DIRECTORY "${exe_dir}"
  RESULT_VARIABLE res
  ERROR_VARIABLE err
)
# Exit code 1 often means non-fatal (e.g. missing translations); only fail on real errors
if(res AND NOT res EQUAL 1)
  message(FATAL_ERROR "windeployqt failed: ${res}\n${err}")
endif()
if(res)
  message(STATUS "windeployqt finished with code ${res} (translations warning is ok)\n${err}")
endif()
message(STATUS "Qt deployed to ${EXE_PATH}")
