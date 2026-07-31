# Copy a freshly built IDA module into an IDA installation, tolerating failure.
#
# IDA holds its plugins and loaders open for as long as it runs, so a developer
# who rebuilds while IDA is open cannot overwrite them. That should not fail the
# build: the module built fine, it just could not be installed yet. Report it
# and carry on.
#
# Invoked as: cmake -DSRC=<file> -DDST=<dir> -P install_ida_module.cmake

if (NOT EXISTS "${SRC}")
  message(WARNING "install_ida_module: ${SRC} does not exist")
  return()
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${DST}/"
  RESULT_VARIABLE rc
  ERROR_QUIET
)

if (NOT rc EQUAL 0)
  get_filename_component(_name "${SRC}" NAME)
  message(STATUS "NOT installed: ${_name} is in use (close IDA to update it)")
endif()
