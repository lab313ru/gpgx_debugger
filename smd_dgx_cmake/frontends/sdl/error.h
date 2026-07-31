#ifndef _ERROR_H_
#define _ERROR_H_

#ifdef __cplusplus
extern "C" {
#endif
	/* Function prototypes */
	void error_init(void);
	void error_shutdown(void);
	void error(char *format, ...);

#ifdef __cplusplus
	}
#endif

#endif /* _ERROR_H_ */

