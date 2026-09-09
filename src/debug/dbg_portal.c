/*
  RPCEmu - An Acorn system emulator

  The guest portal: the host side.

  Everything else in the debug core observes the machine from outside, which
  is why it works on a stock RISC OS from the first instruction of the boot
  ROM. What it cannot do is ask the operating system for anything. There is
  no safe moment for the host to call into RISC OS: it would be calling from
  whatever context the machine happened to be in, possibly inside an
  interrupt handler, possibly mid-SWI, and RISC OS offers no reentrancy
  guarantee that makes that sound.

  A module inside the guest has what the host lacks - a legitimate execution
  context. It cannot be called, but it can call out, and then act on what it
  is told. That inversion is the whole point of the portal.

  The transport already exists: HostFS is a host-intercepted SWI in the same
  chunk, so this is a well-trodden path rather than a new mechanism.

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

/* Reason codes in R0, matching tools/rpcagent.s */
#define AGENT_HELLO	0	/* R1 = workspace, R2 = module version */
#define AGENT_POLL	1	/* -> R0 = 1 if a command is waiting */
#define AGENT_FETCH	2	/* R1 = workspace; -> R0 = 1 if filled in */
#define AGENT_DONE	3	/* R1 = V flag, R2 = error block or 0 */

#define HOST_PROTOCOL_VERSION	1

static DbgPortalState portal;

void
dbg_portal_init(void)
{
	memset(&portal, 0, sizeof(portal));
}

void
dbg_portal_get(DbgPortalState *out)
{
	*out = portal;
}

/**
 * Queue a command for the guest to run.
 *
 * It is not run here. The module notices it on its next tick and runs it
 * from a callback, which is the only context in which calling OS_CLI is
 * legitimate.
 *
 * @return 0 if queued, non-zero if the module is absent or already busy
 */
int
dbg_portal_run(const char *command)
{
	if (!portal.present) {
		return 1;
	}
	if (portal.pending || portal.armed || portal.running) {
		return 2;
	}

	snprintf(portal.command, sizeof(portal.command), "%s", command);
	portal.pending = 1;
	portal.armed = 0;
	portal.running = 0;
	portal.have_result = 0;
	portal.last_error[0] = '\0';
	portal.last_error_number = 0;

	return 0;
}

/**
 * Read a RISC OS error block out of guest memory.
 *
 * A word of error number followed by a terminated message. Reading it turns
 * "the command failed" into something a caller can act on.
 */
static void
read_error_block(uint32_t addr)
{
	size_t i;

	dbg_watch_suspend();

	portal.last_error_number = mem_read32(addr);

	for (i = 0; i < sizeof(portal.last_error) - 1; i++) {
		const uint8_t c = (uint8_t) mem_read8(addr + 4 + (uint32_t) i);

		if (c == 0 || c == 10 || c == 13) {
			break;
		}
		portal.last_error[i] = (char) c;
	}
	portal.last_error[i] = '\0';

	dbg_watch_resume();
}

/**
 * Service a call from the module.
 *
 * Called from opSWI() with the machine's registers in hand, exactly as
 * HostFS is.
 */
void
dbg_portal_swi(void)
{
	const uint32_t reason = arm.reg[0];

	switch (reason) {
	case AGENT_HELLO:
		portal.present = 1;
		portal.workspace = arm.reg[1];
		portal.module_version = arm.reg[2];
		portal.hellos++;
		rpclog("RPCAgent: module version %u, workspace &%08X\n",
		       (unsigned) portal.module_version,
		       (unsigned) portal.workspace);
		arm.reg[0] = HOST_PROTOCOL_VERSION;
		break;

	case AGENT_POLL:
		portal.polls++;

		/* Answering yes arms exactly one callback. The ticker runs
		   every few centiseconds, and the callback does not run until
		   the OS chooses a moment, so leaving the command visible
		   across several ticks queues several callbacks for the one
		   command - which nests OS_CLI, exhausts the system stack and
		   ends in a data abort. */
		if (portal.pending && !portal.armed && !portal.running) {
			portal.pending = 0;
			portal.armed = 1;
			arm.reg[0] = 1;
		} else {
			arm.reg[0] = 0;
		}
		break;

	case AGENT_FETCH: {
		const uint32_t buffer = arm.reg[1];
		size_t i;

		if (!portal.armed) {
			arm.reg[0] = 0;
			break;
		}

		/* Write the command into the module's workspace, terminated
		   with a carriage return as OS_CLI expects. */
		dbg_watch_suspend();
		for (i = 0; portal.command[i] != '\0'; i++) {
			mem_write8(buffer + (uint32_t) i,
			           (uint8_t) portal.command[i]);
		}
		mem_write8(buffer + (uint32_t) i, 13);
		dbg_watch_resume();

		portal.armed = 0;
		portal.running = 1;
		portal.commands++;
		arm.reg[0] = 1;
		break;
	}

	case AGENT_DONE:
		portal.armed = 0;
		portal.running = 0;
		portal.have_result = 1;
		portal.last_failed = (arm.reg[1] != 0);
		portal.last_return_code = 0;
		portal.last_error[0] = '\0';
		portal.last_error_number = 0;

		if (portal.last_failed && arm.reg[2] != 0) {
			read_error_block(arm.reg[2]);
			rpclog("RPCAgent: '%s' failed: &%08X %s\n",
			       portal.command,
			       (unsigned) portal.last_error_number,
			       portal.last_error);
		}
		break;

	default:
		rpclog("RPCAgent: unknown reason %u\n", (unsigned) reason);
		arm.reg[0] = 0;
		break;
	}
}
