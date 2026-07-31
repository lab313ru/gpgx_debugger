#ifndef _MAIN_COMMON_H_
#define _MAIN_COMMON_H_

#define MAX_INPUTS 8

extern int debug_on;
extern int log_error;
extern int turbo_mode;
extern int use_sound;
extern int fullscreen;

/* Frontend must define this function (e.g. sdl_input_update or qt_input_update). */
extern int osd_input_update(void);

#endif /* _MAIN_COMMON_H_ */
