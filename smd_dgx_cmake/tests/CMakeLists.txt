# ---- Tests ----
#
# Links smd_dgx_emu, which is the whole debug layer plus the emulator core, and
# nothing else: no Qt, no IDA, no SDL. That is the point — this is the layer
# every host shares, so it is the layer worth pinning down, and it builds on
# every platform the project claims to support.

add_executable(smd_dgx_tests
  test_main.cpp
  test_backend.cpp
  test_bridge.cpp
  test_input.c          # headless osd_input_update, as every other host has
  test_harness.h
  synth_rom.h
  fixture.h
)
target_link_libraries(smd_dgx_tests PRIVATE smd_dgx_emu)
target_include_directories(smd_dgx_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(smd_dgx_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
)

add_test(NAME smd_dgx_tests COMMAND smd_dgx_tests)
# A hung run-control wait is the failure mode worth guarding: every wait in the
# fixture has its own deadline, this is the backstop if one is ever forgotten.
set_tests_properties(smd_dgx_tests PROPERTIES TIMEOUT 300)
