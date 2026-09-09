/*
  RPCEmu - An Acorn system emulator

  Instruction trace ring: what the machine actually executed.

  For a code generator this answers the question a fault cannot. A crash
  tells you where execution ended; the trace tells you how it got there,
  which is what you need when the emitted code runs to completion and
  produces the wrong answer.

  Filtered by address range, and it has to be. Unfiltered, a million entries
  is about a hundredth of a second of execution and will be entirely
  operating system by the time anything interesting happens. Bounded to the
  image under test, the same million entries covers the whole run.

  Recording happens after the opcode has been fetched by the interpreter, so
  the opcode costs nothing extra to store, and the trace shows exactly the
  instruction word the CPU saw rather than whatever memory holds later.

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

#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "dbg.h"

/* Raised only while tracing. Read directly by arm_exec(). */
int dbg_trace_gate;

static DbgTraceEntry *ring;
static uint32_t ring_capacity;
static uint64_t ring_total;		/**< Entries ever recorded */

static uint64_t instructions_seen;	/**< Instructions since tracing started */
static uint32_t filter_from;
static uint32_t filter_to = 0xffffffffu;

void
dbg_trace_init(void)
{
	free(ring);
	ring = NULL;
	ring_capacity = 0;
	ring_total = 0;
	instructions_seen = 0;
	filter_from = 0;
	filter_to = 0xffffffffu;
	dbg_trace_gate = 0;
}

/**
 * Start tracing, or change the range being traced.
 *
 * @param entries Ring size; the oldest are overwritten once it is full
 * @param from    Lowest address recorded
 * @param to      Highest address recorded
 * @return 0 on success, non-zero if the ring could not be allocated
 */
int
dbg_trace_start(uint32_t entries, uint32_t from, uint32_t to)
{
	if (entries == 0) {
		entries = 1;
	}
	if (entries > DBG_TRACE_MAX_ENTRIES) {
		entries = DBG_TRACE_MAX_ENTRIES;
	}

	if (entries != ring_capacity) {
		DbgTraceEntry *p = realloc(ring, (size_t) entries * sizeof(*ring));

		if (p == NULL) {
			return 1;
		}
		ring = p;
		ring_capacity = entries;
	}

	ring_total = 0;
	instructions_seen = 0;
	filter_from = from;
	filter_to = to;
	dbg_trace_gate = 1;

	return 0;
}

void
dbg_trace_stop(void)
{
	dbg_trace_gate = 0;
}

int
dbg_trace_running(void)
{
	return dbg_trace_gate;
}

uint64_t
dbg_trace_total(void)
{
	return ring_total;
}

uint32_t
dbg_trace_capacity(void)
{
	return ring_capacity;
}

/**
 * Record one executed instruction.
 *
 * Called from arm_exec() once the opcode has been fetched.
 */
void
dbg_trace_record(uint32_t pc, uint32_t opcode)
{
	DbgTraceEntry *e;

	/* Counted before the filter, so the gap between two recorded entries
	   shows how many instructions ran elsewhere in between - which is how
	   an excursion into the operating system shows up in the trace. */
	instructions_seen++;

	if (ring == NULL || pc < filter_from || pc > filter_to) {
		return;
	}

	e = &ring[(size_t) (ring_total % ring_capacity)];
	e->pc = pc;
	e->opcode = opcode;
	e->mode = arm.mode & 0x1f;
	e->instruction = instructions_seen;

	ring_total++;
}

/**
 * Read back the most recent entries, oldest first.
 *
 * @param out   Destination
 * @param max   Entries the destination can hold
 * @param count Receives how many were written
 * @return Absolute index of the first entry returned
 */
uint64_t
dbg_trace_read(DbgTraceEntry *out, uint32_t max, uint32_t *count)
{
	uint64_t held = ring_total;
	uint64_t first;
	uint32_t n;
	uint32_t i;

	if (ring == NULL || ring_total == 0 || max == 0) {
		*count = 0;
		return 0;
	}

	if (held > ring_capacity) {
		held = ring_capacity;
	}

	n = (held < max) ? (uint32_t) held : max;
	first = ring_total - n;

	for (i = 0; i < n; i++) {
		out[i] = ring[(size_t) ((first + i) % ring_capacity)];
	}

	*count = n;

	return first;
}
