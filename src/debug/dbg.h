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
	DBG_STOP_BREAKPOINT,
	DBG_STOP_FAULT,		/**< The program faulted */
	DBG_STOP_WATCHPOINT	/**< Watched memory was touched */
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
extern int dbg_cpu_run_until(uint32_t addr);
extern int dbg_cpu_may_execute(uint32_t pc);
extern int dbg_cpu_take_stop_event(DbgStopReason *reason, uint32_t *pc);
extern const char *dbg_stop_reason_name(DbgStopReason reason);

/** Stop, recording a different address than the current PC. Used by the
    fault trap, where the interesting address is the faulting instruction
    rather than wherever the exception vector leads. */
extern void dbg_cpu_halt_at(DbgStopReason reason, uint32_t pc);

/** Re-evaluate whether the per-instruction gate needs to be up. Called when
    breakpoints are added or removed. */
extern void dbg_cpu_refresh_gate(void);

/* ------------------------------------------------------------------ */
/* Breakpoints and fault catching                                     */
/* ------------------------------------------------------------------ */

typedef enum {
	DBG_FAULT_NONE,
	DBG_FAULT_DATA_ABORT,
	DBG_FAULT_PREFETCH_ABORT,
	DBG_FAULT_UNDEFINED
} DbgFaultKind;

/**
 * Which faults to stop on, and where.
 *
 * RISC OS takes aborts as a matter of course, so an unbounded trap would
 * stop the machine constantly. `from`/`to` bound it to the code under test.
 */
typedef struct {
	int		enabled;
	int		data_abort;
	int		prefetch_abort;
	int		undefined;
	uint32_t	from;
	uint32_t	to;
} DbgCatchConfig;

/** The register file as the faulting instruction left it. */
typedef struct {
	DbgFaultKind	kind;
	uint32_t	pc;		/**< Instruction that faulted */
	uint32_t	mode;
	uint32_t	reg[17];
	uint64_t	count;		/**< Faults seen, of any kind */
} DbgFault;

extern void dbg_break_init(void);
extern int dbg_break_set(uint32_t addr, int temporary, uint32_t skip);
extern int dbg_break_clear(int id);
extern int dbg_break_count(void);
extern int dbg_break_present(uint32_t addr);
extern int dbg_break_should_stop(uint32_t addr);
extern int dbg_break_get(int index, uint32_t *addr, int *id, uint32_t *hits,
                         int *temporary);

extern void dbg_catch_set(const DbgCatchConfig *config);
extern void dbg_catch_get(DbgCatchConfig *config);
extern int dbg_fault_get(DbgFault *fault);
extern void dbg_fault_hook(uint32_t mmode, uint32_t address, uint32_t pc);
extern const char *dbg_fault_kind_name(DbgFaultKind kind);

/* ------------------------------------------------------------------ */
/* Watchpoints                                                        */
/* ------------------------------------------------------------------ */

/*
  The check lives in the inlined memory accessors in mem.h, which are the
  hottest code in the emulator. It is one test of a gate that stays zero
  until a watchpoint exists.
*/

typedef struct {
	uint32_t	addr;
	uint32_t	len;
	int		id;
	int		on_read;
	int		on_write;
	uint32_t	hits;
} DbgWatchInfo;

typedef struct {
	uint32_t	addr;		/**< Address touched */
	uint32_t	size;		/**< Bytes touched */
	int		is_write;
	uint32_t	old_value;	/**< What was there before */
	uint32_t	new_value;	/**< What a write put there */
	uint32_t	pc;		/**< Instruction responsible */
	int		id;		/**< Watchpoint that fired */
	uint64_t	count;
} DbgWatchHit;

extern int dbg_watch_gate;

extern void dbg_watch_init(void);
extern int dbg_watch_set(uint32_t addr, uint32_t len, int on_read, int on_write);
extern int dbg_watch_clear(int id);
extern int dbg_watch_count(void);
extern int dbg_watch_get(int index, DbgWatchInfo *info);
extern int dbg_watch_last_hit(DbgWatchHit *hit);
extern void dbg_watch_check(uint32_t addr, uint32_t size, int is_write,
                            uint32_t value);

/** Suspend the check around the debugger's own guest memory accesses, which
    would otherwise fire read watchpoints the program never triggered. */
extern void dbg_watch_suspend(void);
extern void dbg_watch_resume(void);

/* ------------------------------------------------------------------ */
/* Instruction trace                                                  */
/* ------------------------------------------------------------------ */

#define DBG_TRACE_MAX_ENTRIES	(4 * 1024 * 1024)

typedef struct {
	uint32_t	pc;
	uint32_t	opcode;
	uint32_t	mode;
	uint64_t	instruction;	/**< Instructions since tracing began */
} DbgTraceEntry;

extern int dbg_trace_gate;

extern void dbg_trace_init(void);
extern int dbg_trace_start(uint32_t entries, uint32_t from, uint32_t to);
extern void dbg_trace_stop(void);
extern int dbg_trace_running(void);
extern uint64_t dbg_trace_total(void);
extern uint32_t dbg_trace_capacity(void);
extern void dbg_trace_record(uint32_t pc, uint32_t opcode);
extern uint64_t dbg_trace_read(DbgTraceEntry *out, uint32_t max, uint32_t *count);

/* ------------------------------------------------------------------ */
/* Stack and heap                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t	addr;		/**< Where on the stack */
	uint32_t	value;
	const char	*sym;		/**< Symbol the value lands in, or NULL */
	uint32_t	sym_offset;
} DbgStackWord;

extern uint32_t dbg_stack_read(uint32_t sp, DbgStackWord *out, uint32_t words);
extern uint32_t dbg_stack_backtrace(uint32_t sp, DbgStackWord *out,
                                    uint32_t max, uint32_t depth);

typedef struct {
	uint64_t	calls;
	uint64_t	initialises;
	uint64_t	allocations;
	uint64_t	frees;
	uint64_t	resizes;
	uint64_t	bytes_requested;
	uint64_t	live_blocks;
	uint64_t	peak_live_blocks;
	uint32_t	last_heap;
} DbgHeapStats;

typedef struct {
	uint32_t	addr;
	uint32_t	magic;
	int		valid;
	uint32_t	free_offset;
	uint32_t	base_offset;
	uint32_t	end_offset;
} DbgHeapDescriptor;

extern void dbg_heap_init(void);
extern void dbg_heap_get_stats(DbgHeapStats *stats);
extern void dbg_heap_swi(uint32_t reason, uint32_t heap, uint32_t size);
extern int dbg_heap_describe(uint32_t addr, DbgHeapDescriptor *out);

/* ------------------------------------------------------------------ */
/* Symbols                                                            */
/* ------------------------------------------------------------------ */

/**
 * Load an ELF symbol table.
 *
 * The compiler links at a known base and keeps a real ELF beside the flat
 * image, so addresses line up without the debugger having to guess where
 * anything landed.
 *
 * @param path  ELF file to read
 * @param bias  Added to every symbol value, for an image relocated after link
 * @param error Receives a message on failure
 * @return Number of symbols loaded, or -1 on failure
 */
extern int dbg_sym_load(const char *path, uint32_t bias, const char **error);

extern void dbg_sym_clear(void);
extern int dbg_sym_count(void);

/** Address of a named symbol. Returns 0 if unknown. */
extern int dbg_sym_lookup(const char *name, uint32_t *addr);

/**
 * Nearest symbol at or before an address.
 *
 * @param addr   Address to describe
 * @param offset Receives how far past the symbol the address is
 * @return Symbol name, or NULL if nothing covers it
 */
extern const char *dbg_sym_at(uint32_t addr, uint32_t *offset);

extern int dbg_sym_get(int index, const char **name, uint32_t *addr,
                       uint32_t *size);

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
#define SWI_OS_Heap_		0x1d

/* The portal, in the host SWI chunk HostFS lives in. Slots 0/1/2/4 are
   taken; this is slot 5. */
#define SWI_RPCAgent		(0x56ac0 + 5)
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
/* Guest portal                                                       */
/* ------------------------------------------------------------------ */

/*
  A module inside the guest, talking to the host through a SWI in the same
  chunk HostFS uses. It exists for the one thing observation cannot do: ask
  the operating system for something. The host has no safe context from
  which to call into RISC OS; the module has one, and can call out.
*/

typedef struct {
	int		present;	/**< The module has said hello */
	uint32_t	module_version;
	uint32_t	workspace;	/**< Where the module wants commands written */
	int		pending;	/**< Queued, not yet offered to the module */
	int		armed;		/**< Offered: exactly one callback is coming */
	int		running;	/**< Collected, not yet finished */
	int		have_result;
	int		last_failed;	/**< OS_CLI returned an error */
	uint32_t	last_return_code;
	uint32_t	caller_pc[4];	/**< Where the module called from, per reason */
	uint32_t	last_error_number;
	char		last_error[256];
	uint64_t	hellos;
	uint64_t	polls;
	uint64_t	commands;
	char		command[512];
} DbgPortalState;

extern void dbg_portal_init(void);
extern void dbg_portal_get(DbgPortalState *out);
extern int dbg_portal_run(const char *command);
extern void dbg_portal_swi(void);

/* ------------------------------------------------------------------ */
/* Snapshots                                                          */
/* ------------------------------------------------------------------ */

/**
 * Save or restore the whole machine.
 *
 * Restoring a booted machine takes milliseconds where booting takes nine
 * seconds, which is what makes a tight edit-run-inspect loop affordable.
 *
 * @return 0 on success; on failure *error says why
 */
extern int dbg_state_save(const char *path, uint64_t instructions,
                          const char **error);
extern int dbg_state_load(const char *path, uint64_t *instructions,
                          const char **error);

/** Encode bytes as base64, for carrying binary through the text channel.
    Returns a malloc'd string the caller must free. */
/**
 * Disassemble one ARM instruction.
 *
 * Without this everything the instrumentation reports is a hex word: a
 * breakpoint stops at a number and a trace is a column of them.
 *
 * @param addr   Where it lives, so branch targets can be worked out
 * @param op     The instruction word
 * @param out    Receives the text, always NUL terminated
 * @param length Bytes available in out
 */
extern void dbg_disasm(uint32_t addr, uint32_t op, char *out, size_t length);

extern char *dbg_base64_encode(const uint8_t *data, size_t len);

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
