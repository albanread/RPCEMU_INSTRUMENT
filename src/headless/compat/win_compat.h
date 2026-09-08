/*
  RPCEmu - An Acorn system emulator

  Force-included compatibility shims for the MSVC-ABI build.

  Everything here exists because the upstream Windows build targets MinGW,
  which provides a thicker POSIX veneer than the Microsoft CRT. This header
  is injected ahead of every translation unit (-include) so the upstream
  sources compile unmodified and stay easy to contribute back.

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

#ifndef COMPAT_WIN_COMPAT_H
#define COMPAT_WIN_COMPAT_H

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

/* Case-insensitive comparison */
#ifndef strcasecmp
#define strcasecmp	_stricmp
#endif
#ifndef strncasecmp
#define strncasecmp	_strnicmp
#endif

/* Large-file interfaces. The Microsoft CRT's fopen/fseek/ftell are already
   64-bit capable via the _i64 variants; off64_t has no CRT equivalent.
   Note rpcemu.h defines fseeko64 itself for _MSC_VER, so it is not defined
   here. */
typedef __int64 off64_t;

#ifndef fopen64
#define fopen64		fopen
#endif
#ifndef ftello64
#define ftello64	_ftelli64
#endif

/* File truncation. hostfs.c takes an explicit _MSC_VER path that skips
   <unistd.h> entirely, so this has to live here rather than in the unistd
   shim. _chsize_s returns 0 on success, like ftruncate. */
#include <io.h>
#ifndef ftruncate
#define ftruncate(fd, length)	_chsize_s((fd), (__int64) (length))
#endif

/* POSIX stat type predicates, absent from the Microsoft CRT */
#ifndef S_ISREG
#define S_ISREG(m)	(((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m)	(((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISCHR
#define S_ISCHR(m)	(((m) & _S_IFMT) == _S_IFCHR)
#endif

#endif /* COMPAT_WIN_COMPAT_H */
