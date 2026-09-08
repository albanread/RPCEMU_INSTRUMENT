/*
  RPCEmu - An Acorn system emulator

  POSIX directory reading for the MSVC-ABI build.

  MinGW supplies <dirent.h>; the Microsoft CRT does not. This is the small
  subset hostfs.c, podulerom.c and romload.c actually use, over the Win32
  find-file API. Kept here rather than patched into those files so the
  upstream sources stay untouched.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef COMPAT_DIRENT_H
#define COMPAT_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

#define COMPAT_NAME_MAX	260

struct dirent {
	char d_name[COMPAT_NAME_MAX];
};

typedef struct DIR DIR;

extern DIR *opendir(const char *name);
extern struct dirent *readdir(DIR *dir);
extern int closedir(DIR *dir);
extern void rewinddir(DIR *dir);

#ifdef __cplusplus
}
#endif

#endif /* COMPAT_DIRENT_H */
