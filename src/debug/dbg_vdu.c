/*
  RPCEmu - An Acorn system emulator

  VDU stream capture by SWI interception.

  RISC OS funnels all character output through a small set of SWIs. Watching
  them in opSWI() gives the machine's console output as exact text, in any
  screen mode, without rendering anything or reading the framebuffer. Output
  that scrolls off the top of the screen is still captured; MODE 7 and the
  desktop are captured identically, because this happens before any of that.

  The hook must not perturb the machine. Reading a guest string can touch an
  unmapped page and set a data-abort event, so arm.event is saved and
  restored around every guest access here: an observation that changed the
  program being observed would be worse than no observation.

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

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "dbg.h"

#define VDU_RING_SIZE		(1024 * 1024)
#define GUEST_STRING_MAX	4096

static char ring[VDU_RING_SIZE];
static uint64_t ring_total;		/**< Bytes ever written */

static FILE *stream_file;
static int stream_echo;

static uint64_t input_waits;
static char last_error[256];
static char last_command[256];
static int have_error;
static int have_command;

void
dbg_vdu_init(void)
{
	ring_total = 0;
	input_waits = 0;
	have_error = 0;
	have_command = 0;
	stream_file = NULL;
	stream_echo = 0;
}

void
dbg_vdu_close(void)
{
	if (stream_file != NULL) {
		fflush(stream_file);
	}
}

void
dbg_vdu_set_file(FILE *f)
{
	stream_file = f;
}

void
dbg_vdu_set_echo(int enable)
{
	stream_echo = enable;
}

uint64_t
dbg_vdu_total(void)
{
	return ring_total;
}

uint64_t
dbg_vdu_input_waits(void)
{
	return input_waits;
}

const char *
dbg_vdu_last_error(void)
{
	return have_error ? last_error : NULL;
}

const char *
dbg_vdu_last_command(void)
{
	return have_command ? last_command : NULL;
}

/**
 * Append captured bytes to the ring and to any live destinations.
 */
static void
vdu_emit(const char *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		ring[(size_t) (ring_total % VDU_RING_SIZE)] = data[i];
		ring_total++;
	}

	if (stream_file != NULL) {
		fwrite(data, 1, len, stream_file);
	}
	if (stream_echo) {
		fwrite(data, 1, len, stdout);
	}
}

size_t
dbg_vdu_read(char *buf, size_t len, uint64_t from)
{
	uint64_t oldest;
	size_t n = 0;

	if (from >= ring_total) {
		return 0;
	}

	oldest = (ring_total > VDU_RING_SIZE) ? ring_total - VDU_RING_SIZE : 0;
	if (from < oldest) {
		from = oldest;
	}

	while (from < ring_total && n < len) {
		buf[n++] = ring[(size_t) (from % VDU_RING_SIZE)];
		from++;
	}

	return n;
}

/**
 * Copy a NUL-terminated string out of guest memory.
 *
 * Used for text destined for the VDU stream, where control codes are part of
 * the data and only NUL ends the string.
 *
 * @return Number of bytes copied, excluding the terminator
 */
static size_t
guest_string(uint32_t addr, char *out, size_t max)
{
	size_t n = 0;

	while (n < max - 1) {
		const uint8_t c = (uint8_t) mem_read8(addr + (uint32_t) n);

		if (c == 0) {
			break;
		}
		out[n++] = (char) c;
	}
	out[n] = '\0';

	return n;
}

/**
 * Copy a single line out of guest memory.
 *
 * RISC OS terminates command lines and error messages with NUL, LF or CR;
 * reading past a CR walks into whatever the buffer held previously.
 *
 * @return Number of bytes copied, excluding the terminator
 */
static size_t
guest_line(uint32_t addr, char *out, size_t max)
{
	size_t n = 0;

	while (n < max - 1) {
		const uint8_t c = (uint8_t) mem_read8(addr + (uint32_t) n);

		if (c == 0 || c == 10 || c == 13) {
			break;
		}
		out[n++] = (char) c;
	}
	out[n] = '\0';

	return n;
}

void
dbg_swi_hook(uint32_t swinum, uint32_t pc)
{
	char buf[GUEST_STRING_MAX];
	const uint32_t saved_event = arm.event;
	size_t len;

	/* OS_WriteI (SWI &100 + n) is deliberately NOT captured. The ROM
	   implements it by executing a real OS_WriteC SWI, so capturing both
	   duplicates every character written that way — visible as doubled
	   CRs and BELs in the transcript. OS_WriteC is the true bottleneck. */

	switch (swinum) {
	case SWI_OS_WriteC: {
		const char c = (char) (arm.reg[0] & 0xff);

		vdu_emit(&c, 1);
		break;
	}

	case SWI_OS_WriteS:
		/* The string follows the SWI instruction inline */
		len = guest_string(pc + 4, buf, sizeof(buf));
		vdu_emit(buf, len);
		break;

	case SWI_OS_Write0:
		len = guest_string(arm.reg[0], buf, sizeof(buf));
		vdu_emit(buf, len);
		break;

	case SWI_OS_NewLine:
		vdu_emit("\n", 1);
		break;

	case SWI_OS_WriteN: {
		uint32_t n = arm.reg[1];
		uint32_t i;

		if (n > sizeof(buf)) {
			n = sizeof(buf);
		}
		for (i = 0; i < n; i++) {
			buf[i] = (char) mem_read8(arm.reg[0] + i);
		}
		vdu_emit(buf, n);
		break;
	}

	case SWI_OS_ReadC:
		/* The machine is asking for a character. A rising count with no
		   new output is what "sitting at a prompt" looks like from
		   outside. */
		input_waits++;
		break;

	case SWI_OS_CLI_:
		guest_line(arm.reg[0], last_command, sizeof(last_command));
		have_command = 1;
		break;

	case SWI_OS_GenerateError_:
		/* R0 points at an error block: a word of error number followed
		   by a terminated message. */
		guest_line(arm.reg[0] + 4, last_error, sizeof(last_error));
		have_error = 1;
		rpclog("RISC OS error &%08X: %s\n",
		       (unsigned) mem_read32(arm.reg[0]), last_error);
		break;

	default:
		break;
	}

	/* Undo any abort our own reads may have provoked */
	arm.event = saved_event;
}
