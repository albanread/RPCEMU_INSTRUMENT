/*
  RPCEmu - An Acorn system emulator

  Stack and heap analysis.

  Two questions a code generator raises constantly: what is on the stack, and
  where has the heap gone.

  The backtrace is a scan, not an unwind. ARM code compiled without a frame
  pointer leaves no chain to follow, and RISC OS images carry no unwind
  tables, so the honest approach is to read the stack and report which words
  look like return addresses - a word is a candidate if a loaded symbol
  covers it. That finds the call path for ordinary code, and says plainly
  that it is a guess rather than pretending to certainty it does not have.

  Heap accounting is done at the SWI, where it is exact: every OS_Heap call
  is counted with the size asked for. The heap descriptor is also read back,
  but only its header, and only after checking the magic word - the block
  chain layout is not documented in the PRM text this project has, so
  walking it would be guesswork presented as fact.

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

#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "dbg.h"

/* ------------------------------------------------------------------ */
/* Stack                                                              */
/* ------------------------------------------------------------------ */

/**
 * Read words from the stack, newest first.
 *
 * @param sp    Address to read from, normally r13
 * @param out   Destination
 * @param words How many words to read
 * @return Words actually read
 */
uint32_t
dbg_stack_read(uint32_t sp, DbgStackWord *out, uint32_t words)
{
	const uint32_t saved_event = arm.event;
	uint32_t i;

	dbg_watch_suspend();

	for (i = 0; i < words; i++) {
		const uint32_t addr = sp + i * 4;
		uint32_t offset = 0;

		out[i].addr = addr;
		out[i].value = mem_read32(addr);
		out[i].sym = dbg_sym_at(out[i].value, &offset);
		out[i].sym_offset = offset;
	}

	dbg_watch_resume();
	arm.event = saved_event;

	return words;
}

/**
 * Look for return addresses on the stack.
 *
 * Scans upwards from the stack pointer, reporting every word that a loaded
 * symbol covers. With no symbols loaded there is nothing to recognise, and
 * the scan reports nothing rather than guessing.
 *
 * @param sp    Where to start, normally r13
 * @param out   Destination
 * @param max   Frames the destination can hold
 * @param depth Words of stack to scan
 * @return Candidate frames found
 */
uint32_t
dbg_stack_backtrace(uint32_t sp, DbgStackWord *out, uint32_t max, uint32_t depth)
{
	const uint32_t saved_event = arm.event;
	uint32_t found = 0;
	uint32_t i;

	if (dbg_sym_count() == 0) {
		return 0;
	}

	dbg_watch_suspend();

	for (i = 0; i < depth && found < max; i++) {
		const uint32_t addr = sp + i * 4;
		const uint32_t value = mem_read32(addr);
		uint32_t offset = 0;
		const char *name = dbg_sym_at(value, &offset);

		if (name == NULL) {
			continue;
		}

		out[found].addr = addr;
		out[found].value = value;
		out[found].sym = name;
		out[found].sym_offset = offset;
		found++;
	}

	dbg_watch_resume();
	arm.event = saved_event;

	return found;
}

/* ------------------------------------------------------------------ */
/* Heap                                                               */
/* ------------------------------------------------------------------ */

/* 'Heap' as a little-endian word, the magic at the start of a RISC OS heap */
#define HEAP_MAGIC	0x70616548u

static DbgHeapStats heap_stats;

void
dbg_heap_init(void)
{
	memset(&heap_stats, 0, sizeof(heap_stats));
}

void
dbg_heap_get_stats(DbgHeapStats *stats)
{
	*stats = heap_stats;
}

/**
 * Account for an OS_Heap call, from the SWI hook.
 *
 * Counting here is exact: the reason code and the size asked for are in the
 * registers, and no knowledge of the heap's internals is needed.
 *
 * @param reason R0, the OS_Heap reason code
 * @param heap   R1, the heap descriptor address
 * @param size   R3, the size for the operations that take one
 */
void
dbg_heap_swi(uint32_t reason, uint32_t heap, uint32_t size)
{
	heap_stats.calls++;
	heap_stats.last_heap = heap;

	switch (reason) {
	case 0:	/* Initialise heap */
		heap_stats.initialises++;
		break;
	case 2:	/* Get heap block */
		heap_stats.allocations++;
		heap_stats.bytes_requested += size;
		heap_stats.live_blocks++;
		if (heap_stats.live_blocks > heap_stats.peak_live_blocks) {
			heap_stats.peak_live_blocks = heap_stats.live_blocks;
		}
		break;
	case 3:	/* Free heap block */
		heap_stats.frees++;
		if (heap_stats.live_blocks > 0) {
			heap_stats.live_blocks--;
		}
		break;
	case 4:	/* Extend or shrink a block */
		heap_stats.resizes++;
		break;
	default:
		break;
	}
}

/**
 * Read a heap descriptor out of guest memory.
 *
 * Only the header is reported, and only if the magic word is there. The
 * block chain is deliberately not walked: its layout is not in the PRM text
 * available here, and a walker built on a guess would produce confident
 * nonsense.
 *
 * @return Non-zero if a heap was found at that address
 */
int
dbg_heap_describe(uint32_t addr, DbgHeapDescriptor *out)
{
	const uint32_t saved_event = arm.event;
	uint32_t magic;

	dbg_watch_suspend();

	magic = mem_read32(addr);
	out->addr = addr;
	out->magic = magic;
	out->valid = (magic == HEAP_MAGIC);

	if (out->valid) {
		out->free_offset = mem_read32(addr + 4);
		out->base_offset = mem_read32(addr + 8);
		out->end_offset  = mem_read32(addr + 12);
	} else {
		out->free_offset = 0;
		out->base_offset = 0;
		out->end_offset = 0;
	}

	dbg_watch_resume();
	arm.event = saved_event;

	return out->valid;
}
