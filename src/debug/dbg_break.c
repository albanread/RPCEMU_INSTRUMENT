/*
  RPCEmu - An Acorn system emulator

  Breakpoints, and catching the faults that generated code causes.

  For testing a new code generator the fault trap matters more than the
  breakpoints. When emitted code goes wrong the machine takes a data abort,
  a prefetch abort or an undefined instruction, and by the time RISC OS has
  printed an error the registers have moved on. Catching the exception at
  the point it is raised preserves the whole register file as the faulting
  instruction left it, and reports the address that faulted rather than the
  vector it jumped to.

  The trap is bounded by an address range on purpose. RISC OS takes aborts
  routinely as part of normal memory management, so catching every abort
  would stop the machine constantly during boot. Bounding it to the program
  under test - which is linked at a known base - means "stop if my code
  faults, ignore the operating system's own".

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
#include "dbg.h"

/* ------------------------------------------------------------------ */
/* Breakpoints                                                        */
/* ------------------------------------------------------------------ */

#define BP_MAX		64
#define BP_HASH_SIZE	512		/* power of two, well above BP_MAX */
#define BP_HASH_EMPTY	0xffffffffu

typedef struct {
	uint32_t	addr;
	int		id;
	int		enabled;
	int		temporary;	/**< Removed once hit; used by run_until */
	uint32_t	hits;
	uint32_t	skip;		/**< Ignore this many hits before stopping */
} Breakpoint;

static Breakpoint breakpoints[BP_MAX];
static int breakpoint_count;
static int next_breakpoint_id = 1;

/* Direct lookup keyed on the address, so the per-instruction check is one
   load and one compare in the common case. */
static uint32_t bp_hash[BP_HASH_SIZE];

static unsigned
bp_slot(uint32_t addr)
{
	return (unsigned) ((addr >> 2) & (BP_HASH_SIZE - 1));
}

static void
bp_hash_rebuild(void)
{
	int i;

	for (i = 0; i < BP_HASH_SIZE; i++) {
		bp_hash[i] = BP_HASH_EMPTY;
	}

	for (i = 0; i < breakpoint_count; i++) {
		unsigned slot;

		if (!breakpoints[i].enabled) {
			continue;
		}
		slot = bp_slot(breakpoints[i].addr);
		while (bp_hash[slot] != BP_HASH_EMPTY &&
		       bp_hash[slot] != breakpoints[i].addr)
		{
			slot = (slot + 1) & (BP_HASH_SIZE - 1);
		}
		bp_hash[slot] = breakpoints[i].addr;
	}
}

int
dbg_break_count(void)
{
	return breakpoint_count;
}

/**
 * Is there a breakpoint at this address?
 *
 * Called for every instruction while the gate is up, so it must stay cheap:
 * one hash probe, and the table is mostly empty.
 */
int
dbg_break_present(uint32_t addr)
{
	unsigned slot = bp_slot(addr);

	while (bp_hash[slot] != BP_HASH_EMPTY) {
		if (bp_hash[slot] == addr) {
			return 1;
		}
		slot = (slot + 1) & (BP_HASH_SIZE - 1);
	}

	return 0;
}

static Breakpoint *
bp_find(uint32_t addr)
{
	int i;

	for (i = 0; i < breakpoint_count; i++) {
		if (breakpoints[i].enabled && breakpoints[i].addr == addr) {
			return &breakpoints[i];
		}
	}

	return NULL;
}

/**
 * Decide whether a breakpoint at this address should stop the machine.
 *
 * Counts the hit either way, so a breakpoint can be used purely to find out
 * how often a path runs.
 *
 * @return Non-zero to stop
 */
int
dbg_break_should_stop(uint32_t addr)
{
	Breakpoint *bp = bp_find(addr);

	if (bp == NULL) {
		return 0;
	}

	bp->hits++;

	if (bp->skip != 0) {
		bp->skip--;
		return 0;
	}

	if (bp->temporary) {
		dbg_break_clear(bp->id);
	}

	return 1;
}

/**
 * Add a breakpoint.
 *
 * @param addr      Word-aligned address to stop before
 * @param temporary Remove it once hit
 * @param skip      Ignore this many hits first
 * @return The breakpoint id, or -1 if the table is full
 */
int
dbg_break_set(uint32_t addr, int temporary, uint32_t skip)
{
	Breakpoint *bp;

	addr &= ~3u;

	bp = bp_find(addr);
	if (bp != NULL) {
		/* Setting the same address twice adjusts it rather than
		   filling the table with duplicates that can never all fire. */
		bp->temporary = temporary;
		bp->skip = skip;
		return bp->id;
	}

	if (breakpoint_count >= BP_MAX) {
		return -1;
	}

	bp = &breakpoints[breakpoint_count++];
	bp->addr = addr;
	bp->id = next_breakpoint_id++;
	bp->enabled = 1;
	bp->temporary = temporary;
	bp->hits = 0;
	bp->skip = skip;

	bp_hash_rebuild();
	dbg_cpu_refresh_gate();

	return bp->id;
}

/**
 * Remove a breakpoint by id, or every breakpoint if id is 0.
 *
 * @return Number removed
 */
int
dbg_break_clear(int id)
{
	int removed = 0;
	int i;

	for (i = 0; i < breakpoint_count; i++) {
		if (id == 0 || breakpoints[i].id == id) {
			breakpoints[i] = breakpoints[breakpoint_count - 1];
			breakpoint_count--;
			removed++;
			i--;
			if (id != 0) {
				break;
			}
		}
	}

	if (removed != 0) {
		bp_hash_rebuild();
		dbg_cpu_refresh_gate();
	}

	return removed;
}

int
dbg_break_get(int index, uint32_t *addr, int *id, uint32_t *hits, int *temporary)
{
	if (index < 0 || index >= breakpoint_count) {
		return 0;
	}

	if (addr != NULL)      { *addr = breakpoints[index].addr; }
	if (id != NULL)        { *id = breakpoints[index].id; }
	if (hits != NULL)      { *hits = breakpoints[index].hits; }
	if (temporary != NULL) { *temporary = breakpoints[index].temporary; }

	return 1;
}

/* ------------------------------------------------------------------ */
/* Fault catching                                                     */
/* ------------------------------------------------------------------ */

static DbgCatchConfig catch_config;
static DbgFault last_fault;
static int have_fault;

void
dbg_catch_set(const DbgCatchConfig *config)
{
	catch_config = *config;
}

void
dbg_catch_get(DbgCatchConfig *config)
{
	*config = catch_config;
}

int
dbg_fault_get(DbgFault *fault)
{
	if (!have_fault) {
		return 0;
	}
	*fault = last_fault;

	return 1;
}

/**
 * Called from exception() before the mode switch.
 *
 * Records the fault and, if it is one we were asked to catch, stops the CPU.
 * The exception itself is still taken: interfering with it would change the
 * behaviour of the program being observed.
 *
 * @param mmode   Mode the exception enters (ABORT, UNDEFINED, ...)
 * @param address Exception vector address
 * @param pc      Address of the instruction that faulted
 */
void
dbg_fault_hook(uint32_t mmode, uint32_t address, uint32_t pc)
{
	DbgFaultKind kind;

	switch (mmode) {
	case ABORT:
		kind = (address == 0x10) ? DBG_FAULT_PREFETCH_ABORT
		                         : DBG_FAULT_DATA_ABORT;
		break;
	case UNDEFINED:
		kind = DBG_FAULT_UNDEFINED;
		break;
	default:
		/* SWIs, IRQ and FIQ are ordinary machine operation. */
		return;
	}

	/* Capture the register file before the exception rewrites the mode and
	   the link register: this is the state the faulting instruction left,
	   and it is the whole point of trapping here rather than afterwards. */
	memcpy(last_fault.reg, arm.reg, sizeof(last_fault.reg));
	last_fault.kind = kind;
	last_fault.pc = pc;
	last_fault.mode = arm.mode;
	last_fault.count++;
	have_fault = 1;

	if (!catch_config.enabled) {
		return;
	}

	switch (kind) {
	case DBG_FAULT_DATA_ABORT:
		if (!catch_config.data_abort) { return; }
		break;
	case DBG_FAULT_PREFETCH_ABORT:
		if (!catch_config.prefetch_abort) { return; }
		break;
	case DBG_FAULT_UNDEFINED:
		if (!catch_config.undefined) { return; }
		break;
	default:
		return;
	}

	/* Outside the range being watched, this is the operating system going
	   about its business. */
	if (pc < catch_config.from || pc > catch_config.to) {
		return;
	}

	dbg_cpu_halt_at(DBG_STOP_FAULT, pc);
}

const char *
dbg_fault_kind_name(DbgFaultKind kind)
{
	switch (kind) {
	case DBG_FAULT_DATA_ABORT:     return "data_abort";
	case DBG_FAULT_PREFETCH_ABORT: return "prefetch_abort";
	case DBG_FAULT_UNDEFINED:      return "undefined_instruction";
	default:                       return "none";
	}
}

void
dbg_break_init(void)
{
	breakpoint_count = 0;
	next_breakpoint_id = 1;
	have_fault = 0;
	memset(&last_fault, 0, sizeof(last_fault));
	memset(&catch_config, 0, sizeof(catch_config));
	catch_config.to = 0xffffffffu;
	bp_hash_rebuild();
}
