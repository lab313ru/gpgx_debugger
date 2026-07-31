/*
 *  fileio.c - Load a normal file, or ZIP/GZ archive into ROM buffer.
 *  Returns loaded ROM size (zero if an error occured)
 *
 *  Copyright (C) 1998-2003  Charles Mac Donald
 *  modified by Eke-Eke (Genesis Plus GX)
 */

#include "shared.h"

static int check_zip(char *filename);

int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension)
{
  if (!filename || !buffer) return 0;

  /* copy file extension (last 3 chars before NUL) */
  if (extension)
  {
    int len = (int)strlen(filename);
    if (len >= 3)
      strncpy(extension, &filename[len - 3], 3);
    else
      memset(extension, 0, 3);
    extension[3] = 0;
  }

  FILE *fd = fopen(filename, "rb");
  if (!fd) return 0;

  fseek(fd, 0, SEEK_END);
  long filesize = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  int size = (filesize > maxsize) ? maxsize : (int)filesize;
  size = (int)fread(buffer, 1, size, fd);
  fclose(fd);

  return size;
}

static int check_zip(char *filename)
{
  uint8 buf[2];
  FILE *fd = fopen(filename, "rb");
  if(!fd) return (0);
  fread(buf, 2, 1, fd);
  fclose(fd);
  if(memcmp(buf, "PK", 2) == 0) return (1);
  return (0);
}
