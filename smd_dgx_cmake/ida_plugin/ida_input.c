/* Headless input stub for the IDA plugin host.
 *
 * gx_core is compiled with the SDL osd.h, which maps osd_input_update ->
 * sdl_input_update. A debugger host drives no controller input, so this is a
 * no-op — the same shape as qt_input.c / sdl.c in the standalone frontends. */
int sdl_input_update(void)
{
    return 0;
}
