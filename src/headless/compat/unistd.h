/*
  RPCEmu - An Acorn system emulator

  Just enough <unistd.h> for the MSVC-ABI build.

  MinGW supplies this header; the Microsoft CRT does not. The emulator uses
  only a handful of the POSIX file calls, all of which the CRT provides under
  underscore-prefixed names (already unprefixed here by
  _CRT_NONSTDC_NO_DEPRECATE).

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

#ifndef COMPAT_UNISTD_H
#define COMPAT_UNISTD_H

#include <io.h>			/* access, unlink, _chsize_s, _fileno */
#include <direct.h>		/* rmdir, mkdir, getcwd, chdir */
#include <process.h>		/* getpid */

/* access() mode bits, absent from the Microsoft CRT headers */
#ifndef F_OK
#define F_OK	0
#define X_OK	0		/* Windows has no execute permission bit */
#define W_OK	2
#define R_OK	4
#endif

/* Truncate an open file. _chsize_s returns 0 on success like ftruncate. */
#ifndef ftruncate
#define ftruncate(fd, length)	_chsize_s((fd), (__int64) (length))
#endif

#endif /* COMPAT_UNISTD_H */
