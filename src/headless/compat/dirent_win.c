/*
  RPCEmu - An Acorn system emulator

  POSIX directory reading over the Win32 find-file API. See dirent.h.

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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "dirent.h"

struct DIR {
	HANDLE		handle;		/**< INVALID_HANDLE_VALUE until first read */
	WIN32_FIND_DATAA data;
	int		pending;	/**< data holds an entry not yet returned */
	struct dirent	entry;
	char		pattern[MAX_PATH];
};

/**
 * Open a directory for reading.
 *
 * @param name Directory path, with or without a trailing separator
 * @return Directory handle, or NULL with errno set
 */
DIR *
opendir(const char *name)
{
	DIR *dir;
	size_t len;

	if (name == NULL || *name == '\0') {
		errno = ENOENT;
		return NULL;
	}

	dir = calloc(1, sizeof(*dir));
	if (dir == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	len = strlen(name);
	if (len + 3 >= sizeof(dir->pattern)) {
		free(dir);
		errno = ENAMETOOLONG;
		return NULL;
	}

	strcpy(dir->pattern, name);
	if (dir->pattern[len - 1] != '\\' && dir->pattern[len - 1] != '/') {
		strcat(dir->pattern, "\\");
	}
	strcat(dir->pattern, "*");

	dir->handle = FindFirstFileA(dir->pattern, &dir->data);
	if (dir->handle == INVALID_HANDLE_VALUE) {
		const DWORD err = GetLastError();

		free(dir);
		errno = (err == ERROR_FILE_NOT_FOUND) ? ENOENT :
		        (err == ERROR_PATH_NOT_FOUND) ? ENOENT :
		        (err == ERROR_ACCESS_DENIED)  ? EACCES : EINVAL;
		return NULL;
	}

	dir->pending = 1;

	return dir;
}

/**
 * Return the next directory entry, or NULL at the end.
 *
 * The returned pointer is owned by the DIR and is overwritten by the next
 * call, matching POSIX.
 */
struct dirent *
readdir(DIR *dir)
{
	if (dir == NULL) {
		errno = EBADF;
		return NULL;
	}

	if (!dir->pending) {
		if (!FindNextFileA(dir->handle, &dir->data)) {
			return NULL;
		}
	}
	dir->pending = 0;

	snprintf(dir->entry.d_name, sizeof(dir->entry.d_name), "%s",
	         dir->data.cFileName);

	return &dir->entry;
}

/**
 * Restart the directory scan from the beginning.
 */
void
rewinddir(DIR *dir)
{
	if (dir == NULL) {
		return;
	}

	if (dir->handle != INVALID_HANDLE_VALUE) {
		FindClose(dir->handle);
	}

	dir->handle = FindFirstFileA(dir->pattern, &dir->data);
	dir->pending = (dir->handle != INVALID_HANDLE_VALUE);
}

int
closedir(DIR *dir)
{
	if (dir == NULL) {
		errno = EBADF;
		return -1;
	}

	if (dir->handle != INVALID_HANDLE_VALUE) {
		FindClose(dir->handle);
	}
	free(dir);

	return 0;
}
