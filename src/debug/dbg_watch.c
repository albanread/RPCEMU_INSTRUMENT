/*
  RPCEmu - An Acorn system emulator

  Watchpoints: stop when a particular piece of memory is read or written.

  This is the tool for "something is corrupting my variable". Breakpoints
  answer where execution is; watchpoints answer who touched a value, which is
  the question generated code usually raises.

  The check sits in the inlined memory accessors, which are the hottest code
  in the emulator, so it is one test of a global that stays zero until a
  watchpoint exists. Everything expensive - working out which watchpoint,
  reading the previous value, stopping the CPU - happens only once the gate
  is up and an address matches.

  The debugger's own reads must not trip it. The VDU capture reads guest
  strings on every output SWI, and the control channel reads memory on
  request; either would fire a read watchpoint on memory the program never
  touched. Those paths suspend the check around their accesses.

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

#define WATCH_MAX	16

typedef struct {
	uint32_t	addr;
	uint32_t	len;
	int		id;
	int		on_read;
	int		on_write;
	int		enabled;
	uint32_t	hits;
} Watchpoint;

static Watchpoint watchpoints[WATCH_MAX];
static int watch_count;
static int next_watch_id = 1;

/* Raised only while at least one watchpoint exists. Read directly by the
   inlined accessors in mem.h. */
int dbg_watch_gate;

/* Non-zero while the debugger itself is reading guest memory. */
static int watch_suspended;

static DbgWatchHit last_hit;
static int have_hit;

void
dbg_watch_init(void)
{
	watch_count = 0;
	next_watch_id = 1;
	watch_suspended = 0;
	have_hit = 0;
	dbg_watch_gate = 0;
	memset(&last_hit, 0, sizeof(last_hit));
}

/**
 * Suspend and resume the check around the debugger's own memory accesses.
 *
 * Nests, so a caller need not know whether an outer level already suspended.
 */
void
dbg_watch_suspend(void)
{
	watch_suspended++;
}

void
dbg_watch_resume(void)
{
	if (watch_suspended > 0) {
		watch_suspended--;
	}
}

int
dbg_watch_count(void)
{
	return watch_count;
}

/**
 * Add a watchpoint.
 *
 * @param addr     First byte watched
 * @param len      Bytes watched, at least one
 * @param on_read  Stop on reads
 * @param on_write Stop on writes
 * @return The watchpoint id, or -1 if the table is full
 */
int
dbg_watch_set(uint32_t addr, uint32_t len, int on_read, int on_write)
{
	Watchpoint *w;

	if (watch_count >= WATCH_MAX) {
		return -1;
	}
	if (len == 0) {
		len = 1;
	}
	if (!on_read && !on_write) {
		on_write = 1;
	}

	w = &watchpoints[watch_count++];
	w->addr = addr;
	w->len = len;
	w->id = next_watch_id++;
	w->on_read = on_read;
	w->on_write = on_write;
	w->enabled = 1;
	w->hits = 0;

	dbg_watch_gate = 1;

	return w->id;
}

int
dbg_watch_clear(int id)
{
	int removed = 0;
	int i;

	for (i = 0; i < watch_count; i++) {
		if (id == 0 || watchpoints[i].id == id) {
			watchpoints[i] = watchpoints[watch_count - 1];
			watch_count--;
			removed++;
			i--;
			if (id != 0) {
				break;
			}
		}
	}

	dbg_watch_gate = (watch_count != 0);

	return removed;
}

int
dbg_watch_get(int index, DbgWatchInfo *info)
{
	if (index < 0 || index >= watch_count) {
		return 0;
	}

	info->addr = watchpoints[index].addr;
	info->len = watchpoints[index].len;
	info->id = watchpoints[index].id;
	info->on_read = watchpoints[index].on_read;
	info->on_write = watchpoints[index].on_write;
	info->hits = watchpoints[index].hits;

	return 1;
}

int
dbg_watch_last_hit(DbgWatchHit *hit)
{
	if (!have_hit) {
		return 0;
	}
	*hit = last_hit;

	return 1;
}

/**
 * Called from the inlined memory accessors when the gate is up.
 *
 * @param addr     Address being accessed
 * @param size     Bytes accessed
 * @param is_write Non-zero for a write
 * @param value    Value being written, meaningless for a read
 */
void
dbg_watch_check(uint32_t addr, uint32_t size, int is_write, uint32_t value)
{
	int i;

	if (watch_suspended) {
		return;
	}

	for (i = 0; i < watch_count; i++) {
		const Watchpoint *w = &watchpoints[i];

		if (!w->enabled) {
			continue;
		}
		if (is_write ? !w->on_write : !w->on_read) {
			continue;
		}

		/* Overlap, not equality: a word store at addr-2 still touches a
		   byte being watched at addr. */
		if (addr + size <= w->addr || addr >= w->addr + w->len) {
			continue;
		}

		watchpoints[i].hits++;

		last_hit.addr = addr;
		last_hit.size = size;
		last_hit.is_write = is_write;
		last_hit.new_value = value;
		last_hit.id = w->id;
		last_hit.pc = PC;
		last_hit.count++;

		/* The old value is worth more than the new one for a
		   corruption hunt, and reading it here costs nothing because
		   this path only runs on an actual hit. */
		dbg_watch_suspend();
		last_hit.old_value = (size == 1) ? mem_read8(addr) : mem_read32(addr & ~3u);
		dbg_watch_resume();

		have_hit = 1;

		dbg_cpu_halt_at(DBG_STOP_WATCHPOINT, PC);
		return;
	}
}
