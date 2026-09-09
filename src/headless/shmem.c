/*
  RPCEmu - An Acorn system emulator

  Shared memory: the machine's video, visible to another process without
  copying it through a pipe.

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

#include <stdio.h>
#include <string.h>

#include <windows.h>

#include "rpcemu.h"
#include "shmem.h"

static HANDLE		section;
static HANDLE		frame_event;
static ShmemHeader	*header;
static uint8_t		*base;
static uint64_t		section_bytes;
static char		section_name[64];
static char		event_name[64];

/*
  Names are per-process so two emulators can run at once, and live in the
  session's Local namespace so no privilege is needed to create them and
  nothing outside the session can see them.
*/
static void
names_build(void)
{
	const unsigned pid = (unsigned) GetCurrentProcessId();

	snprintf(section_name, sizeof(section_name),
	         "Local\\RPCEmu.%u.frames", pid);
	snprintf(event_name, sizeof(event_name),
	         "Local\\RPCEmu.%u.frame", pid);
}

int
shmem_init(void)
{
	uint64_t bytes;

	if (header != NULL) {
		return 0;
	}

	names_build();

	bytes = (uint64_t) sizeof(ShmemHeader);
	bytes = (bytes + 4095u) & ~4095ull;	/* pixels start page aligned */

	{
		const uint64_t slots_offset = bytes;

		bytes += (uint64_t) SHMEM_SLOTS * (uint64_t) SHMEM_SLOT_BYTES;

		section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
		                             PAGE_READWRITE,
		                             (DWORD) (bytes >> 32),
		                             (DWORD) (bytes & 0xffffffffu),
		                             section_name);
		if (section == NULL) {
			rpclog("shmem: CreateFileMapping failed (%lu)\n",
			       GetLastError());
			return 1;
		}

		base = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
		if (base == NULL) {
			rpclog("shmem: MapViewOfFile failed (%lu)\n",
			       GetLastError());
			CloseHandle(section);
			section = NULL;
			return 1;
		}

		/* Manual reset, and the reader resets it before checking the
		   newest serial. That order means a frame published during the
		   gap leaves the event signalled rather than being missed, and
		   every reader wakes rather than just one. */
		frame_event = CreateEventA(NULL, TRUE, FALSE, event_name);
		if (frame_event == NULL) {
			rpclog("shmem: CreateEvent failed (%lu)\n",
			       GetLastError());
			UnmapViewOfFile(base);
			CloseHandle(section);
			base = NULL;
			section = NULL;
			return 1;
		}

		header = (ShmemHeader *) base;
		memset(header, 0, sizeof(*header));

		header->magic         = SHMEM_MAGIC;
		header->version       = SHMEM_VERSION;
		header->header_bytes  = (uint32_t) sizeof(ShmemHeader);
		header->pid           = (uint32_t) GetCurrentProcessId();
		header->slot_count    = SHMEM_SLOTS;
		header->slot_bytes    = SHMEM_SLOT_BYTES;
		header->slots_offset  = slots_offset;

		section_bytes = bytes;
	}

	rpclog("shmem: %s, %u slots of %u bytes, %llu bytes total\n",
	       section_name, (unsigned) SHMEM_SLOTS,
	       (unsigned) SHMEM_SLOT_BYTES,
	       (unsigned long long) section_bytes);

	return 0;
}

void
shmem_close(void)
{
	if (frame_event != NULL) {
		CloseHandle(frame_event);
		frame_event = NULL;
	}
	if (base != NULL) {
		UnmapViewOfFile(base);
		base = NULL;
	}
	if (section != NULL) {
		CloseHandle(section);
		section = NULL;
	}

	header = NULL;
	section_bytes = 0;
}

int
shmem_active(void)
{
	return header != NULL;
}

const char *
shmem_section_name(void)
{
	return (header != NULL) ? section_name : NULL;
}

const char *
shmem_event_name(void)
{
	return (header != NULL) ? event_name : NULL;
}

uint64_t
shmem_section_bytes(void)
{
	return section_bytes;
}

ShmemHeader *
shmem_header(void)
{
	return header;
}

/**
 * Publish one frame.
 *
 * thread: video
 *
 * The slot is written under a sequence lock: odd while it is being filled,
 * even when it is whole. A reader that sees the same even value before and
 * after its copy knows nothing moved underneath it, and never has to take a
 * lock the video thread would then have to wait on.
 */
void
shmem_publish_frame(const uint32_t *pixels, uint32_t byte_length,
                    const ShmemSlot *meta)
{
	ShmemSlot *slot;
	unsigned index;

	if (header == NULL || pixels == NULL || meta == NULL) {
		return;
	}

	if (byte_length > SHMEM_SLOT_BYTES) {
		/* Cannot happen for any mode VIDC20 scans out, but a truncated
		   frame would be worse than none. */
		rpclog("shmem: frame of %u bytes exceeds slot, not published\n",
		       (unsigned) byte_length);
		return;
	}

	index = (unsigned) (header->writes % SHMEM_SLOTS);
	slot = &header->slots[index];

	/* Odd: this slot is being written. */
	InterlockedIncrement((volatile LONG *) &slot->seq);
	MemoryBarrier();

	memcpy(base + header->slots_offset + (uint64_t) index * SHMEM_SLOT_BYTES,
	       pixels, byte_length);

	slot->serial        = meta->serial;
	slot->when_ns       = meta->when_ns;
	slot->instructions  = meta->instructions;
	slot->mode_serial   = meta->mode_serial;
	slot->xsize         = meta->xsize;
	slot->ysize         = meta->ysize;
	slot->host_xsize    = meta->host_xsize;
	slot->host_ysize    = meta->host_ysize;
	slot->doublesize    = meta->doublesize;
	slot->bpp           = meta->bpp;
	slot->border        = meta->border;
	slot->cursor_x      = meta->cursor_x;
	slot->cursor_y      = meta->cursor_y;
	slot->cursor_height = meta->cursor_height;
	slot->byte_length   = byte_length;

	MemoryBarrier();
	InterlockedIncrement((volatile LONG *) &slot->seq);	/* Even: whole */

	/* Only now say the frame exists. A reader takes newest first, so it
	   can never be pointed at a slot that is still being filled. */
	header->writes++;
	header->newest_index = index;
	header->newest = meta->serial;

	MemoryBarrier();

	if (frame_event != NULL) {
		SetEvent(frame_event);
	}
}
