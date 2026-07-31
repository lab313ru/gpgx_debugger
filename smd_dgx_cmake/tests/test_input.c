/* Headless input stub for the test host.
 *
 * gx_core is compiled with the SDL osd.h, which maps osd_input_update ->
 * sdl_input_update. Tests drive the pad through IDebugBackend::setPad, so
 * there is no device to poll — same shape as ida_input.c and qt_input.c. */
int sdl_input_update(void)
{
    return 0;
}
