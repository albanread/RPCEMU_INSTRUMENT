/*
  RPCEmu - An Acorn system emulator

  CPU run state: halt, continue, step.

  Halting stops the CPU alone. The rest of the machine — VIDC scan-out, the
  view window, the control channel — carries on, which is what makes a
  screenshot at a breakpoint a true picture rather than a stale one. Guest
  time does not advance while the CPU is stopped: the frontend resynchronises
  the periodic interrupts on resume, so the guest never sees a backlog.

  The per-instruction check is one test of a global that is zero whenever the
  CPU is running freely, so the cost to normal execution is a predictable
  branch.

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

/* Non-zero whenever the CPU needs checking before each instruction. Kept
   separate from the state enum so the hot path tests one global. */
int dbg_cpu_gate;

static DbgRunState run_state = DBG_RUNNING;
static uint64_t step_budget;
static DbgStopReason stop_reason = DBG_STOP_NONE;
static uint32_t stop_pc;
static int stop_pending;

void
dbg_cpu_init(void)
{
	run_state = DBG_RUNNING;
	step_budget = 0;
	stop_reason = DBG_STOP_NONE;
	stop_pending = 0;
	dbg_cpu_gate = 0;
}

DbgRunState
dbg_cpu_state(void)
{
	return run_state;
}

int
dbg_cpu_running(void)
{
	return run_state != DBG_STOPPED;
}

/**
 * Stop the CPU.
 *
 * Safe to call from any thread: it only raises the gate, and the CPU
 * notices before its next instruction.
 */
void
dbg_cpu_halt(DbgStopReason reason)
{
	if (run_state == DBG_STOPPED) {
		return;
	}

	run_state = DBG_STOPPED;
	step_budget = 0;
	stop_reason = reason;
	stop_pc = PC;
	stop_pending = 1;
	dbg_cpu_gate = 1;
}

void
dbg_cpu_continue(void)
{
	run_state = DBG_RUNNING;
	step_budget = 0;
	stop_reason = DBG_STOP_NONE;
	dbg_cpu_gate = 0;
}

/**
 * Run exactly `count` instructions, then stop.
 */
void
dbg_cpu_step(uint64_t count)
{
	if (count == 0) {
		count = 1;
	}

	run_state = DBG_STEPPING;
	step_budget = count;
	stop_reason = DBG_STOP_NONE;
	dbg_cpu_gate = 1;
}

/**
 * Per-instruction gate, called from arm_exec() before each instruction.
 *
 * @param pc Address of the instruction about to execute
 * @return Non-zero to execute it, zero to stop before it
 */
int
dbg_cpu_may_execute(uint32_t pc)
{
	switch (run_state) {
	case DBG_STOPPED:
		return 0;

	case DBG_STEPPING:
		if (step_budget == 0) {
			run_state = DBG_STOPPED;
			stop_reason = DBG_STOP_STEP;
			stop_pc = pc;
			stop_pending = 1;
			return 0;
		}
		step_budget--;
		return 1;

	case DBG_RUNNING:
	default:
		/* The gate is only raised while stopped or stepping; getting
		   here means it was lowered concurrently, which is harmless. */
		dbg_cpu_gate = 0;
		return 1;
	}
}

/**
 * Collect a stop event, if one has happened since this was last called.
 *
 * @param reason Receives why the CPU stopped
 * @param pc     Receives where it stopped
 * @return Non-zero if there was an event to report
 */
int
dbg_cpu_take_stop_event(DbgStopReason *reason, uint32_t *pc)
{
	if (!stop_pending) {
		return 0;
	}

	stop_pending = 0;
	if (reason != NULL) {
		*reason = stop_reason;
	}
	if (pc != NULL) {
		*pc = stop_pc;
	}

	return 1;
}

const char *
dbg_stop_reason_name(DbgStopReason reason)
{
	switch (reason) {
	case DBG_STOP_REQUEST:    return "request";
	case DBG_STOP_STEP:       return "step";
	case DBG_STOP_BREAKPOINT: return "breakpoint";
	default:                  return "none";
	}
}
