/*
  RPCEmu - An Acorn system emulator

  Debug core: interfaces for observing and controlling the emulated machine.

  The core is plain C with no dependency on any frontend, so both the
  headless and Qt builds can carry it. See RPCEMU-AGENT.md.

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

#ifndef DBG_H
#define DBG_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* CPU run state                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
	DBG_RUNNING,
	DBG_STOPPED,
	DBG_STEPPING
} DbgRunState;

typedef enum {
	DBG_STOP_NONE,
	DBG_STOP_REQUEST,	/**< Asked to stop */
	DBG_STOP_STEP,		/**< Step count reached */
	DBG_STOP_BREAKPOINT
} DbgStopReason;

/** Raised whenever the CPU must be checked before each instruction. Read
    directly by arm_exec() so the common case costs one predictable branch. */
extern int dbg_cpu_gate;

extern void dbg_cpu_init(void);
extern DbgRunState dbg_cpu_state(void);
extern int dbg_cpu_running(void);
extern void dbg_cpu_halt(DbgStopReason reason);
extern void dbg_cpu_continue(void);
extern void dbg_cpu_step(uint64_t count);
extern int dbg_cpu_may_execute(uint32_t pc);
extern int dbg_cpu_take_stop_event(DbgStopReason *reason, uint32_t *pc);
extern const char *dbg_stop_reason_name(DbgStopReason reason);

/* ------------------------------------------------------------------ */
/* VDU stream capture                                                 */
/* ------------------------------------------------------------------ */

/*
  Everything RISC OS prints goes through a handful of SWIs. Catching them in
  opSWI() yields the machine's console output as text: exact bytes, in any
  screen mode, with nothing rendered and nothing scrolled away. It needs no
  cooperation from the guest, so it works from the first instruction of the
  boot ROM onwards.
*/

/** SWI numbers, from the PRM (db/riscos_prm.sqlite). The X (error-returning)
    forms differ only in bit 17, which opSWI has already masked off. */
#define SWI_OS_WriteC		0x00
#define SWI_OS_WriteS		0x01	/* string inline after the instruction */
#define SWI_OS_Write0		0x02	/* string at R0 */
#define SWI_OS_NewLine		0x03
#define SWI_OS_ReadC		0x04
#define SWI_OS_CLI_		0x05
#define SWI_OS_Exit_		0x11
#define SWI_OS_GenerateError_	0x2b
#define SWI_OS_WriteN		0x46
#define SWI_OS_WriteI		0x100	/* &100 + n writes character n */

extern void dbg_vdu_init(void);
extern void dbg_vdu_close(void);

/** Stream captured output to a file as it arrives. NULL to stop. */
extern void dbg_vdu_set_file(FILE *f);

/** Mirror captured output to a stream as it arrives. NULL to stop. */
extern void dbg_vdu_set_echo(FILE *f);

/** Total bytes ever captured, which only increases. */
extern uint64_t dbg_vdu_total(void);

/**
 * Read captured output from the ring buffer.
 *
 * @param buf   Destination
 * @param len   Size of destination
 * @param from  Absolute stream offset to read from (see dbg_vdu_total())
 * @return Bytes copied. Fewer than requested means the stream ended; zero
 *         with a non-zero total means the offset has been overwritten.
 */
extern size_t dbg_vdu_read(char *buf, size_t len, uint64_t from);

/** Number of times the machine has blocked reading a character. A rising
    count with no output means it is sitting at a prompt waiting for input. */
extern uint64_t dbg_vdu_input_waits(void);

/** Most recent error RISC OS raised, or NULL. */
extern const char *dbg_vdu_last_error(void);

/** Increments each time an error is raised, so a watcher can spot a new one
    even when the message is identical to the last. */
extern uint64_t dbg_vdu_error_seq(void);

/** Most recent command passed to OS_CLI, or NULL. */
extern const char *dbg_vdu_last_command(void);

/**
 * SWI observation hook, called from opSWI() before the SWI is actioned.
 *
 * @param swinum SWI number with the X bit already masked off
 * @param pc     Address of the SWI instruction, for inline-string SWIs
 */
extern void dbg_swi_hook(uint32_t swinum, uint32_t pc);

/* ------------------------------------------------------------------ */
/* Control channel (JSON-RPC 2.0 over stdin/stdout)                   */
/* ------------------------------------------------------------------ */

extern void dbg_rpc_start(void);
extern void dbg_rpc_stop(void);
extern void dbg_rpc_poll(void);
extern int dbg_rpc_active(void);
extern int dbg_rpc_quit_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* DBG_H */
