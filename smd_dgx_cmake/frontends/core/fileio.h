/*
 *  fileio.h - Load a normal file, or ZIP/GZ archive.
 *  Returns loaded ROM size (zero if an error occured)
 *
 *  Copyright (C) 1998-2003  Charles Mac Donald
 *  modified by Eke-Eke (Genesis Plus GX)
 */

#pragma once

/* Function prototypes */
extern int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension);
