#include "main_qt.h"

int qt_input_update(void)
{
	return 0;
}

/* gx_core is compiled with SDL osd.h which maps osd_input_update -> sdl_input_update */
int sdl_input_update(void)
{
	return 0;
}
