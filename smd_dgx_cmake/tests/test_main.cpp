#include "test_harness.h"

// Each case builds a whole emulator, so the suite is deliberately small and
// slow-ish rather than broad and shallow. Pass a substring to run a subset:
//   smd_dgx_tests z80
int main(int argc, char** argv)
{
    // Unbuffered: a case that hangs is the failure mode worth diagnosing, and
    // with block buffering a redirected run leaves an empty log that says
    // nothing about which case it died in.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("smd_dgx tests\n\n");
    return t::run(argc > 1 ? argv[1] : nullptr);
}
