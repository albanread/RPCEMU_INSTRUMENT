/*
  RPCEmu - An Acorn system emulator

  Control channel: JSON-RPC 2.0 over stdin and stdout.

  Shaped like an MCP stdio server on purpose. One JSON object per line in,
  one per line out, methods with named parameters, and a `describe` method
  that reports the command surface. An agent can spawn the emulator as a
  subprocess and talk to it over pipes: no ports, no listening socket, no
  question of who else can reach a channel that can read and write all of
  guest memory. Wrapping this as a literal MCP server is a thin adapter over
  the same methods.

  Because stdout carries the protocol, everything human-readable goes to
  stderr while the channel is open.

  Commands are parsed on a reader thread but executed on the emulator thread
  at a safe point, never concurrently with the CPU.

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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "headless.h"
#include "dbg.h"
#include "json.h"

#define RPC_LINE_MAX	(64 * 1024)
#define RPC_QUEUE_SIZE	64
#define RPC_NODE_POOL	256
#define MEM_READ_MAX	4096

/* JSON-RPC error codes */
#define ERR_PARSE	(-32700)
#define ERR_REQUEST	(-32600)
#define ERR_NO_METHOD	(-32601)
#define ERR_PARAMS	(-32602)
#define ERR_INTERNAL	(-32603)

static int rpc_enabled;
static int rpc_should_quit;

/* Lines from stdin, filled by the reader thread, drained by the emulator */
static CRITICAL_SECTION queue_lock;
static char *queue[RPC_QUEUE_SIZE];
static int queue_head;
static int queue_tail;
static HANDLE reader_thread;
static volatile LONG reader_eof;

static JsonOut out;
static uint64_t reported_error_seq;

static void write_symbol_of(uint32_t addr);

/* ------------------------------------------------------------------ */
/* Transport                                                          */
/* ------------------------------------------------------------------ */

static void
emit_line(void)
{
	if (out.failed || out.buf == NULL) {
		return;
	}

	fputs(out.buf, stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

static DWORD WINAPI
reader_thread_runner(LPVOID param)
{
	char *line = malloc(RPC_LINE_MAX);

	NOT_USED(param);

	if (line == NULL) {
		return 1;
	}

	while (fgets(line, RPC_LINE_MAX, stdin) != NULL) {
		char *copy;
		size_t n = strlen(line);
		int next;

		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
			line[--n] = '\0';
		}
		if (n == 0) {
			continue;
		}

		copy = malloc(n + 1);
		if (copy == NULL) {
			continue;
		}
		memcpy(copy, line, n + 1);

		EnterCriticalSection(&queue_lock);
		next = (queue_tail + 1) % RPC_QUEUE_SIZE;
		if (next == queue_head) {
			/* The emulator is not keeping up. Dropping the oldest
			   request is better than blocking the reader, which
			   would deadlock a client waiting on our reply. */
			free(queue[queue_head]);
			queue_head = (queue_head + 1) % RPC_QUEUE_SIZE;
		}
		queue[queue_tail] = copy;
		queue_tail = next;
		LeaveCriticalSection(&queue_lock);

		free(line);
		line = malloc(RPC_LINE_MAX);
		if (line == NULL) {
			break;
		}
	}

	free(line);
	InterlockedExchange(&reader_eof, 1);

	return 0;
}

static char *
queue_take(void)
{
	char *line = NULL;

	EnterCriticalSection(&queue_lock);
	if (queue_head != queue_tail) {
		line = queue[queue_head];
		queue_head = (queue_head + 1) % RPC_QUEUE_SIZE;
	}
	LeaveCriticalSection(&queue_lock);

	return line;
}

/* ------------------------------------------------------------------ */
/* Replies                                                            */
/* ------------------------------------------------------------------ */

static void
reply_begin(const JsonValue *id)
{
	json_out_reset(&out);
	json_out_raw(&out, "{\"jsonrpc\":\"2.0\",\"id\":");

	if (id == NULL) {
		json_out_raw(&out, "null");
	} else if (id->type == JSON_STRING) {
		json_out_string(&out, id->string);
	} else {
		json_out_printf(&out, "%lld", (long long) json_int(id, 0));
	}

	json_out_raw(&out, ",\"result\":");
}

static void
reply_end(void)
{
	json_out_raw(&out, "}");
	emit_line();
}

static void
reply_error(const JsonValue *id, int code, const char *message)
{
	json_out_reset(&out);
	json_out_raw(&out, "{\"jsonrpc\":\"2.0\",\"id\":");
	if (id == NULL) {
		json_out_raw(&out, "null");
	} else if (id->type == JSON_STRING) {
		json_out_string(&out, id->string);
	} else {
		json_out_printf(&out, "%lld", (long long) json_int(id, 0));
	}
	json_out_printf(&out, ",\"error\":{\"code\":%d,\"message\":", code);
	json_out_string(&out, message);
	json_out_raw(&out, "}}");
	emit_line();
}

static void
notify_begin(const char *method)
{
	json_out_reset(&out);
	json_out_raw(&out, "{\"jsonrpc\":\"2.0\",\"method\":");
	json_out_string(&out, method);
	json_out_raw(&out, ",\"params\":");
}

/* ------------------------------------------------------------------ */
/* Machine state as JSON                                              */
/* ------------------------------------------------------------------ */

static const char *
state_name(void)
{
	switch (dbg_cpu_state()) {
	case DBG_STOPPED:  return "stopped";
	case DBG_STEPPING: return "stepping";
	default:           return "running";
	}
}

static void
write_status(void)
{
	json_out_printf(&out,
	    "{\"state\":\"%s\",\"pc\":%u,\"instructions\":%llu,"
	    "\"mode\":%u,\"vdu_bytes\":%llu,\"input_waits\":%llu,"
	    "\"frames\":%d,\"typing\":%s",
	    state_name(),
	    (unsigned) PC,
	    (unsigned long long) headless_instructions(),
	    (unsigned) (arm.mode & 0x1f),
	    (unsigned long long) dbg_vdu_total(),
	    (unsigned long long) dbg_vdu_input_waits(),
	    headless_frames_available(),
	    headless_type_busy() ? "true" : "false");

	if (dbg_vdu_last_command() != NULL) {
		json_out_raw(&out, ",\"last_command\":");
		json_out_string(&out, dbg_vdu_last_command());
	}
	if (dbg_vdu_last_error() != NULL) {
		json_out_raw(&out, ",\"last_error\":");
		json_out_string(&out, dbg_vdu_last_error());
	}
	json_out_printf(&out, ",\"breakpoints\":%d,\"symbols\":%d",
	                dbg_break_count(), dbg_sym_count());
	write_symbol_of(PC);

	json_out_raw(&out, "}");
}

/**
 * Append `,"sym":"name+offset"` for an address, if a symbol covers it.
 *
 * Every address the channel reports goes through this, so a client never has
 * to hold a symbol table of its own to make sense of a stop.
 */
static void
write_symbol_of(uint32_t addr)
{
	uint32_t offset = 0;
	const char *name = dbg_sym_at(addr, &offset);

	if (name == NULL) {
		return;
	}

	json_out_raw(&out, ",\"sym\":");
	json_out_string(&out, name);
	json_out_printf(&out, ",\"sym_offset\":%u", (unsigned) offset);
}

/**
 * Read an address given either as `addr` or as a `symbol` name.
 *
 * @return Non-zero on success; on failure *err says why
 */
static int
resolve_addr(const JsonDoc *doc, int params, uint32_t *addr, const char **err)
{
	const JsonValue *sym = json_member(doc, params, "symbol");
	const JsonValue *a = json_member(doc, params, "addr");

	if (sym != NULL) {
		const char *name = json_string(sym, NULL);

		if (name == NULL || !dbg_sym_lookup(name, addr)) {
			*err = "unknown symbol";
			return 0;
		}
		return 1;
	}

	if (a != NULL) {
		*addr = (uint32_t) json_int(a, 0);
		return 1;
	}

	*err = "addr or symbol is required";

	return 0;
}

/**
 * Map a register name to its index in arm.reg[].
 *
 * @return -1 if the name is not a register
 */
static int
register_index(const char *name)
{
	int n;

	if (name == NULL) {
		return -1;
	}
	if (strcmp(name, "pc") == 0 || strcmp(name, "PC") == 0) {
		return 15;
	}
	if (strcmp(name, "cpsr") == 0 || strcmp(name, "CPSR") == 0) {
		return 16;
	}
	if (strcmp(name, "sp") == 0) {
		return 13;
	}
	if (strcmp(name, "lr") == 0) {
		return 14;
	}
	if ((name[0] == 'r' || name[0] == 'R') && name[1] != '\0') {
		char *end = NULL;

		n = (int) strtol(name + 1, &end, 10);
		if (end != NULL && *end == '\0' && n >= 0 && n <= 16) {
			return n;
		}
	}

	return -1;
}

/* ------------------------------------------------------------------ */
/* Method dispatch                                                    */
/* ------------------------------------------------------------------ */

static const char DESCRIBE_JSON[] =
"{\"methods\":["
 "{\"name\":\"describe\",\"summary\":\"List the available methods\"},"
 "{\"name\":\"status\",\"summary\":\"Run state, PC, instruction count, output totals\"},"
 "{\"name\":\"halt\",\"summary\":\"Stop the CPU; the rest of the machine keeps running\"},"
 "{\"name\":\"continue\",\"summary\":\"Resume the CPU\"},"
 "{\"name\":\"step\",\"params\":{\"count\":\"integer, default 1\"},"
   "\"summary\":\"Run exactly this many instructions, then stop\"},"
 "{\"name\":\"reset\",\"summary\":\"Reset the machine\"},"
 "{\"name\":\"regs.read\",\"summary\":\"All ARM registers plus mode\"},"
 "{\"name\":\"regs.write\",\"params\":{\"reg\":\"r0-r16, pc, sp, lr, cpsr\",\"value\":\"integer\"}},"
 "{\"name\":\"mem.read\",\"params\":{\"addr\":\"integer\",\"len\":\"integer, max 4096\"},"
   "\"summary\":\"Read guest memory, returned as hex\"},"
 "{\"name\":\"mem.write\",\"params\":{\"addr\":\"integer\",\"hex\":\"string\"}},"
 "{\"name\":\"type\",\"params\":{\"text\":\"string, \\\\n for Return\"},"
   "\"summary\":\"Type into the machine, paced against its echo\"},"
 "{\"name\":\"vdu.read\",\"params\":{\"from\":\"integer stream offset\",\"max\":\"integer\"},"
   "\"summary\":\"Console output captured from the OS output SWIs\"},"
 "{\"name\":\"screenshot\",\"params\":{\"path\":\"string\"}},"
 "{\"name\":\"frames.save\",\"params\":{\"prefix\":\"string\"},"
   "\"summary\":\"Write the whole held frame history, oldest first\"},"
 "{\"name\":\"frames.info\",\"summary\":\"How many frames are held, and the newest serial\"},"
 "{\"name\":\"bp.set\",\"params\":{\"addr\":\"integer\",\"symbol\":\"string\","
   "\"temporary\":\"bool\",\"skip\":\"ignore this many hits first\"}},"
 "{\"name\":\"bp.clear\",\"params\":{\"id\":\"integer, 0 for all\"}},"
 "{\"name\":\"bp.list\"},"
 "{\"name\":\"run_until\",\"params\":{\"addr\":\"integer\",\"symbol\":\"string\"},"
   "\"summary\":\"Continue until an address is reached\"},"
 "{\"name\":\"catch.set\",\"params\":{\"data_abort\":\"bool\","
   "\"prefetch_abort\":\"bool\",\"undefined\":\"bool\","
   "\"from\":\"integer\",\"to\":\"integer\"},"
   "\"summary\":\"Stop when code in the given range faults. Bound it to the "
   "program under test: RISC OS takes aborts routinely\"},"
 "{\"name\":\"fault.info\",\"summary\":\"The last fault, with registers as the "
   "faulting instruction left them\"},"
 "{\"name\":\"sym.load\",\"params\":{\"path\":\"ELF file\",\"bias\":\"integer\"}},"
 "{\"name\":\"sym.lookup\",\"params\":{\"name\":\"string\"}},"
 "{\"name\":\"sym.at\",\"params\":{\"addr\":\"integer\"}},"
 "{\"name\":\"wp.set\",\"params\":{\"addr\":\"integer\",\"symbol\":\"string\","
   "\"len\":\"bytes, default 4\",\"on\":\"r, w or rw\"},"
   "\"summary\":\"Stop when memory is touched. For finding what corrupts a value\"},"
 "{\"name\":\"wp.clear\",\"params\":{\"id\":\"integer, 0 for all\"}},"
 "{\"name\":\"wp.list\"},"
 "{\"name\":\"wp.last\",\"summary\":\"The last watchpoint hit, with the old and new value\"},"
 "{\"name\":\"trace.start\",\"params\":{\"entries\":\"ring size\","
   "\"from\":\"integer\",\"to\":\"integer\"},"
   "\"summary\":\"Record executed instructions. Bound the range: unfiltered, a "
   "million entries is a hundredth of a second\"},"
 "{\"name\":\"trace.stop\"},"
 "{\"name\":\"trace.read\",\"params\":{\"max\":\"entries, default 64\"},"
   "\"summary\":\"Most recent instructions, oldest first\"},"
 "{\"name\":\"stack.read\",\"params\":{\"sp\":\"defaults to r13\",\"words\":\"default 16\"},"
   "\"summary\":\"After a fault pass the faulting r13 from fault.info: taking "
   "the exception banks r13, so the live one is not the program's\"},"
 "{\"name\":\"stack.backtrace\",\"params\":{\"sp\":\"defaults to r13\","
   "\"depth\":\"words to scan\"},"
   "\"summary\":\"Stack words that land inside a known symbol. A scan, not an "
   "unwind: ARM code without a frame pointer has no chain to follow\"},"
 "{\"name\":\"heap.stats\",\"summary\":\"OS_Heap activity, counted at the SWI\"},"
 "{\"name\":\"heap.describe\",\"params\":{\"addr\":\"defaults to the last heap seen\"},"
   "\"summary\":\"Read a heap descriptor header, if the magic word is there\"},"
 "{\"name\":\"quit\",\"summary\":\"Shut the emulator down\"}"
"],\"events\":["
 "{\"name\":\"event/stopped\",\"summary\":\"The CPU stopped, with reason and PC\"},"
 "{\"name\":\"event/error\",\"summary\":\"RISC OS raised an error\"}"
"]}";

static void
handle_request(char *line)
{
	JsonValue pool[RPC_NODE_POOL];
	JsonDoc doc;
	const JsonValue *id;
	const JsonValue *method_v;
	const char *method;
	int root;
	int params;

	root = json_parse(&doc, line, pool, RPC_NODE_POOL);
	if (root < 0) {
		reply_error(NULL, ERR_PARSE, doc.error ? doc.error : "bad JSON");
		return;
	}

	id = json_member(&doc, root, "id");
	method_v = json_member(&doc, root, "method");
	method = json_string(method_v, NULL);

	if (method == NULL) {
		reply_error(id, ERR_REQUEST, "missing method");
		return;
	}

	{
		const JsonValue *p = json_member(&doc, root, "params");

		params = (p != NULL) ? (int) (p - doc.nodes) : -1;
	}

	if (strcmp(method, "describe") == 0) {
		reply_begin(id);
		json_out_raw(&out, DESCRIBE_JSON);
		reply_end();

	} else if (strcmp(method, "status") == 0) {
		reply_begin(id);
		write_status();
		reply_end();

	} else if (strcmp(method, "halt") == 0) {
		dbg_cpu_halt(DBG_STOP_REQUEST);
		reply_begin(id);
		write_status();
		reply_end();

	} else if (strcmp(method, "continue") == 0) {
		dbg_cpu_continue();
		reply_begin(id);
		write_status();
		reply_end();

	} else if (strcmp(method, "step") == 0) {
		const long long count =
		    json_int(json_member(&doc, params, "count"), 1);

		if (count < 1) {
			reply_error(id, ERR_PARAMS, "count must be at least 1");
			return;
		}
		dbg_cpu_step((uint64_t) count);
		reply_begin(id);
		write_status();
		reply_end();

	} else if (strcmp(method, "reset") == 0) {
		resetrpc();
		reply_begin(id);
		write_status();
		reply_end();

	} else if (strcmp(method, "regs.read") == 0) {
		int i;

		reply_begin(id);
		json_out_raw(&out, "{\"r\":[");
		for (i = 0; i < 16; i++) {
			json_out_printf(&out, "%s%u", (i != 0) ? "," : "",
			                (unsigned) arm.reg[i]);
		}
		json_out_printf(&out,
		    "],\"cpsr\":%u,\"mode\":%u,\"pc\":%u}",
		    (unsigned) arm.reg[16], (unsigned) (arm.mode & 0x1f),
		    (unsigned) PC);
		reply_end();

	} else if (strcmp(method, "regs.write") == 0) {
		const char *name =
		    json_string(json_member(&doc, params, "reg"), NULL);
		const int index = register_index(name);

		if (index < 0) {
			reply_error(id, ERR_PARAMS, "unknown register");
			return;
		}
		arm.reg[index] = (uint32_t)
		    json_int(json_member(&doc, params, "value"), 0);

		reply_begin(id);
		json_out_printf(&out, "{\"reg\":%d,\"value\":%u}", index,
		                (unsigned) arm.reg[index]);
		reply_end();

	} else if (strcmp(method, "mem.read") == 0) {
		const uint32_t addr = (uint32_t)
		    json_int(json_member(&doc, params, "addr"), 0);
		long long len = json_int(json_member(&doc, params, "len"), 16);
		const uint32_t saved_event = arm.event;
		long long i;

		if (len < 1 || len > MEM_READ_MAX) {
			reply_error(id, ERR_PARAMS, "len must be 1..4096");
			return;
		}

		dbg_watch_suspend();

		reply_begin(id);
		json_out_printf(&out, "{\"addr\":%u,\"len\":%lld,\"hex\":\"",
		                (unsigned) addr, len);
		for (i = 0; i < len; i++) {
			json_out_printf(&out, "%02x",
			    (unsigned) (mem_read8(addr + (uint32_t) i) & 0xff));
		}
		json_out_raw(&out, "\"}");

		/* Reading unmapped guest memory must not leave an abort
		   pending on a machine that never asked for one. */
		arm.event = saved_event;
		dbg_watch_resume();
		reply_end();

	} else if (strcmp(method, "mem.write") == 0) {
		const uint32_t addr = (uint32_t)
		    json_int(json_member(&doc, params, "addr"), 0);
		const char *hex =
		    json_string(json_member(&doc, params, "hex"), NULL);
		const uint32_t saved_event = arm.event;
		size_t written = 0;
		size_t i;

		if (hex == NULL || (strlen(hex) % 2) != 0) {
			reply_error(id, ERR_PARAMS, "hex must be an even number of digits");
			return;
		}

		for (i = 0; hex[i] != '\0'; i += 2) {
			char byte[3];

			byte[0] = hex[i];
			byte[1] = hex[i + 1];
			byte[2] = '\0';
			mem_write8(addr + (uint32_t) written,
			           (uint8_t) strtoul(byte, NULL, 16));
			written++;
		}
		arm.event = saved_event;
		dbg_watch_resume();

		reply_begin(id);
		json_out_printf(&out, "{\"addr\":%u,\"written\":%u}",
		                (unsigned) addr, (unsigned) written);
		reply_end();

	} else if (strcmp(method, "type") == 0) {
		const char *text =
		    json_string(json_member(&doc, params, "text"), NULL);
		int queued;

		if (text == NULL) {
			reply_error(id, ERR_PARAMS, "text is required");
			return;
		}
		queued = headless_type_string(text);

		reply_begin(id);
		json_out_printf(&out, "{\"queued\":%d}", queued);
		reply_end();

	} else if (strcmp(method, "vdu.read") == 0) {
		const uint64_t total = dbg_vdu_total();
		uint64_t from = (uint64_t)
		    json_int(json_member(&doc, params, "from"), 0);
		long long max = json_int(json_member(&doc, params, "max"), 8192);
		char *buf;
		size_t got;

		if (max < 1 || max > 256 * 1024) {
			max = 8192;
		}
		buf = malloc((size_t) max);
		if (buf == NULL) {
			reply_error(id, ERR_INTERNAL, "out of memory");
			return;
		}

		got = dbg_vdu_read(buf, (size_t) max, from);

		reply_begin(id);
		json_out_printf(&out, "{\"from\":%llu,\"total\":%llu,\"text\":",
		                (unsigned long long) from,
		                (unsigned long long) total);
		json_out_stringn(&out, buf, got);
		json_out_raw(&out, "}");
		reply_end();
		free(buf);

	} else if (strcmp(method, "screenshot") == 0) {
		const char *path =
		    json_string(json_member(&doc, params, "path"), NULL);

		if (path == NULL) {
			reply_error(id, ERR_PARAMS, "path is required");
			return;
		}
		if (headless_screenshot(path) != 0) {
			reply_error(id, ERR_INTERNAL, "could not write the screenshot");
			return;
		}

		reply_begin(id);
		json_out_raw(&out, "{\"path\":");
		json_out_string(&out, path);
		json_out_raw(&out, "}");
		reply_end();

	} else if (strcmp(method, "frames.save") == 0) {
		const char *prefix =
		    json_string(json_member(&doc, params, "prefix"), NULL);
		int written;

		if (prefix == NULL) {
			reply_error(id, ERR_PARAMS, "prefix is required");
			return;
		}
		written = headless_frames_save(prefix);

		reply_begin(id);
		json_out_printf(&out, "{\"written\":%d,\"prefix\":", written);
		json_out_string(&out, prefix);
		json_out_raw(&out, "}");
		reply_end();

	} else if (strcmp(method, "frames.info") == 0) {
		reply_begin(id);
		json_out_printf(&out, "{\"held\":%d,\"serial\":%llu}",
		                headless_frames_available(),
		                (unsigned long long) headless_frame_serial());
		reply_end();

	} else if (strcmp(method, "bp.set") == 0) {
		uint32_t addr;
		const char *err = NULL;
		int bpid;

		if (!resolve_addr(&doc, params, &addr, &err)) {
			reply_error(id, ERR_PARAMS, err);
			return;
		}
		bpid = dbg_break_set(addr,
		    json_bool(json_member(&doc, params, "temporary"), 0),
		    (uint32_t) json_int(json_member(&doc, params, "skip"), 0));

		if (bpid < 0) {
			reply_error(id, ERR_INTERNAL, "too many breakpoints");
			return;
		}

		reply_begin(id);
		json_out_printf(&out, "{\"id\":%d,\"addr\":%u", bpid,
		                (unsigned) addr);
		write_symbol_of(addr);
		json_out_raw(&out, "}");
		reply_end();

	} else if (strcmp(method, "bp.clear") == 0) {
		const int bpid = (int) json_int(json_member(&doc, params, "id"), 0);
		const int removed = dbg_break_clear(bpid);

		reply_begin(id);
		json_out_printf(&out, "{\"removed\":%d}", removed);
		reply_end();

	} else if (strcmp(method, "bp.list") == 0) {
		int i;

		reply_begin(id);
		json_out_raw(&out, "{\"breakpoints\":[");
		for (i = 0; i < dbg_break_count(); i++) {
			uint32_t addr, hits;
			int bpid, temporary;

			if (!dbg_break_get(i, &addr, &bpid, &hits, &temporary)) {
				continue;
			}
			json_out_printf(&out,
			    "%s{\"id\":%d,\"addr\":%u,\"hits\":%u,\"temporary\":%s",
			    (i != 0) ? "," : "", bpid, (unsigned) addr,
			    (unsigned) hits, temporary ? "true" : "false");
			write_symbol_of(addr);
			json_out_raw(&out, "}");
		}
		json_out_raw(&out, "]}");
		reply_end();

	} else if (strcmp(method, "run_until") == 0) {
		uint32_t addr;
		const char *err = NULL;

		if (!resolve_addr(&doc, params, &addr, &err)) {
			reply_error(id, ERR_PARAMS, err);
			return;
		}
		if (dbg_cpu_run_until(addr) < 0) {
			reply_error(id, ERR_INTERNAL, "too many breakpoints");
			return;
		}

		reply_begin(id);
		json_out_printf(&out, "{\"addr\":%u}", (unsigned) addr);
		reply_end();

	} else if (strcmp(method, "catch.set") == 0) {
		DbgCatchConfig cfg;

		dbg_catch_get(&cfg);
		cfg.data_abort =
		    json_bool(json_member(&doc, params, "data_abort"), cfg.data_abort);
		cfg.prefetch_abort =
		    json_bool(json_member(&doc, params, "prefetch_abort"), cfg.prefetch_abort);
		cfg.undefined =
		    json_bool(json_member(&doc, params, "undefined"), cfg.undefined);
		cfg.from = (uint32_t)
		    json_int(json_member(&doc, params, "from"), (long long) cfg.from);
		cfg.to = (uint32_t)
		    json_int(json_member(&doc, params, "to"), (long long) cfg.to);
		cfg.enabled = cfg.data_abort || cfg.prefetch_abort || cfg.undefined;

		dbg_catch_set(&cfg);

		reply_begin(id);
		json_out_printf(&out,
		    "{\"enabled\":%s,\"data_abort\":%s,\"prefetch_abort\":%s,"
		    "\"undefined\":%s,\"from\":%u,\"to\":%u}",
		    cfg.enabled ? "true" : "false",
		    cfg.data_abort ? "true" : "false",
		    cfg.prefetch_abort ? "true" : "false",
		    cfg.undefined ? "true" : "false",
		    (unsigned) cfg.from, (unsigned) cfg.to);
		reply_end();

	} else if (strcmp(method, "fault.info") == 0) {
		DbgFault fault;

		reply_begin(id);
		if (!dbg_fault_get(&fault)) {
			json_out_raw(&out, "{\"faults\":0}");
		} else {
			int i;

			json_out_printf(&out,
			    "{\"faults\":%llu,\"kind\":\"%s\",\"pc\":%u,\"mode\":%u,\"r\":[",
			    (unsigned long long) fault.count,
			    dbg_fault_kind_name(fault.kind),
			    (unsigned) fault.pc, (unsigned) (fault.mode & 0x1f));
			for (i = 0; i < 16; i++) {
				json_out_printf(&out, "%s%u", (i != 0) ? "," : "",
				                (unsigned) fault.reg[i]);
			}
			json_out_printf(&out, "],\"cpsr\":%u",
			                (unsigned) fault.reg[16]);
			write_symbol_of(fault.pc);
			json_out_raw(&out, "}");
		}
		reply_end();

	} else if (strcmp(method, "sym.load") == 0) {
		const char *path =
		    json_string(json_member(&doc, params, "path"), NULL);
		const uint32_t bias = (uint32_t)
		    json_int(json_member(&doc, params, "bias"), 0);
		const char *err = NULL;
		int loaded;

		if (path == NULL) {
			reply_error(id, ERR_PARAMS, "path is required");
			return;
		}
		loaded = dbg_sym_load(path, bias, &err);
		if (loaded < 0) {
			reply_error(id, ERR_PARAMS, (err != NULL) ? err : "load failed");
			return;
		}

		reply_begin(id);
		json_out_printf(&out, "{\"loaded\":%d}", loaded);
		reply_end();

	} else if (strcmp(method, "sym.lookup") == 0) {
		const char *name =
		    json_string(json_member(&doc, params, "name"), NULL);
		uint32_t addr;

		if (name == NULL || !dbg_sym_lookup(name, &addr)) {
			reply_error(id, ERR_PARAMS, "unknown symbol");
			return;
		}

		reply_begin(id);
		json_out_printf(&out, "{\"addr\":%u,\"name\":", (unsigned) addr);
		json_out_string(&out, name);
		json_out_raw(&out, "}");
		reply_end();

	} else if (strcmp(method, "sym.at") == 0) {
		const uint32_t addr = (uint32_t)
		    json_int(json_member(&doc, params, "addr"), 0);

		reply_begin(id);
		json_out_printf(&out, "{\"addr\":%u", (unsigned) addr);
		write_symbol_of(addr);
		json_out_raw(&out, "}");
		reply_end();

	} else if (strcmp(method, "wp.set") == 0) {
		uint32_t addr;
		const char *err = NULL;
		const char *on = json_string(json_member(&doc, params, "on"), "w");
		int wpid;

		if (!resolve_addr(&doc, params, &addr, &err)) {
			reply_error(id, ERR_PARAMS, err);
			return;
		}
		wpid = dbg_watch_set(addr,
		    (uint32_t) json_int(json_member(&doc, params, "len"), 4),
		    strchr(on, 'r') != NULL, strchr(on, 'w') != NULL);

		if (wpid < 0) {
			reply_error(id, ERR_INTERNAL, "too many watchpoints");
			return;
		}

		reply_begin(id);
		json_out_printf(&out, "{\"id\":%d,\"addr\":%u}", wpid,
		                (unsigned) addr);
		reply_end();

	} else if (strcmp(method, "wp.clear") == 0) {
		const int removed =
		    dbg_watch_clear((int) json_int(json_member(&doc, params, "id"), 0));

		reply_begin(id);
		json_out_printf(&out, "{\"removed\":%d}", removed);
		reply_end();

	} else if (strcmp(method, "wp.list") == 0) {
		int i;

		reply_begin(id);
		json_out_raw(&out, "{\"watchpoints\":[");
		for (i = 0; i < dbg_watch_count(); i++) {
			DbgWatchInfo w;

			if (!dbg_watch_get(i, &w)) {
				continue;
			}
			json_out_printf(&out,
			    "%s{\"id\":%d,\"addr\":%u,\"len\":%u,\"read\":%s,"
			    "\"write\":%s,\"hits\":%u}",
			    (i != 0) ? "," : "", w.id, (unsigned) w.addr,
			    (unsigned) w.len, w.on_read ? "true" : "false",
			    w.on_write ? "true" : "false", (unsigned) w.hits);
		}
		json_out_raw(&out, "]}");
		reply_end();

	} else if (strcmp(method, "wp.last") == 0) {
		DbgWatchHit hit;

		reply_begin(id);
		if (!dbg_watch_last_hit(&hit)) {
			json_out_raw(&out, "{\"hits\":0}");
		} else {
			json_out_printf(&out,
			    "{\"hits\":%llu,\"id\":%d,\"addr\":%u,\"size\":%u,"
			    "\"write\":%s,\"old\":%u,\"new\":%u,\"pc\":%u",
			    (unsigned long long) hit.count, hit.id,
			    (unsigned) hit.addr, (unsigned) hit.size,
			    hit.is_write ? "true" : "false",
			    (unsigned) hit.old_value, (unsigned) hit.new_value,
			    (unsigned) hit.pc);
			write_symbol_of(hit.pc);
			json_out_raw(&out, "}");
		}
		reply_end();

	} else if (strcmp(method, "trace.start") == 0) {
		const uint32_t entries = (uint32_t)
		    json_int(json_member(&doc, params, "entries"), 65536);
		const uint32_t from = (uint32_t)
		    json_int(json_member(&doc, params, "from"), 0);
		const uint32_t to = (uint32_t)
		    json_int(json_member(&doc, params, "to"), 0xffffffffu);

		if (dbg_trace_start(entries, from, to) != 0) {
			reply_error(id, ERR_INTERNAL, "could not allocate the trace ring");
			return;
		}

		reply_begin(id);
		json_out_printf(&out,
		    "{\"entries\":%u,\"from\":%u,\"to\":%u}",
		    (unsigned) dbg_trace_capacity(), (unsigned) from, (unsigned) to);
		reply_end();

	} else if (strcmp(method, "trace.stop") == 0) {
		dbg_trace_stop();
		reply_begin(id);
		json_out_printf(&out, "{\"recorded\":%llu}",
		                (unsigned long long) dbg_trace_total());
		reply_end();

	} else if (strcmp(method, "trace.read") == 0) {
		long long max = json_int(json_member(&doc, params, "max"), 64);
		DbgTraceEntry *entries;
		uint32_t count = 0;
		uint64_t first;
		uint32_t i;

		if (max < 1 || max > 4096) {
			max = 64;
		}
		entries = malloc((size_t) max * sizeof(*entries));
		if (entries == NULL) {
			reply_error(id, ERR_INTERNAL, "out of memory");
			return;
		}
		first = dbg_trace_read(entries, (uint32_t) max, &count);

		reply_begin(id);
		json_out_printf(&out,
		    "{\"recorded\":%llu,\"first\":%llu,\"running\":%s,\"entries\":[",
		    (unsigned long long) dbg_trace_total(),
		    (unsigned long long) first,
		    dbg_trace_running() ? "true" : "false");
		for (i = 0; i < count; i++) {
			json_out_printf(&out,
			    "%s{\"pc\":%u,\"opcode\":%u,\"mode\":%u,\"n\":%llu",
			    (i != 0) ? "," : "", (unsigned) entries[i].pc,
			    (unsigned) entries[i].opcode, (unsigned) entries[i].mode,
			    (unsigned long long) entries[i].instruction);
			write_symbol_of(entries[i].pc);
			json_out_raw(&out, "}");
		}
		json_out_raw(&out, "]}");
		reply_end();
		free(entries);

	} else if (strcmp(method, "stack.read") == 0) {
		const uint32_t sp = (uint32_t)
		    json_int(json_member(&doc, params, "sp"), (long long) arm.reg[13]);
		long long words = json_int(json_member(&doc, params, "words"), 16);
		DbgStackWord stack[256];
		uint32_t got, i;

		if (words < 1 || words > 256) {
			words = 16;
		}
		got = dbg_stack_read(sp, stack, (uint32_t) words);

		reply_begin(id);
		json_out_printf(&out, "{\"sp\":%u,\"words\":[", (unsigned) sp);
		for (i = 0; i < got; i++) {
			json_out_printf(&out, "%s{\"addr\":%u,\"value\":%u",
			    (i != 0) ? "," : "", (unsigned) stack[i].addr,
			    (unsigned) stack[i].value);
			if (stack[i].sym != NULL) {
				json_out_raw(&out, ",\"sym\":");
				json_out_string(&out, stack[i].sym);
				json_out_printf(&out, ",\"sym_offset\":%u",
				                (unsigned) stack[i].sym_offset);
			}
			json_out_raw(&out, "}");
		}
		json_out_raw(&out, "]}");
		reply_end();

	} else if (strcmp(method, "stack.backtrace") == 0) {
		const uint32_t sp = (uint32_t)
		    json_int(json_member(&doc, params, "sp"), (long long) arm.reg[13]);
		long long depth = json_int(json_member(&doc, params, "depth"), 128);
		DbgStackWord frames[64];
		uint32_t got, i;

		if (depth < 1 || depth > 4096) {
			depth = 128;
		}
		got = dbg_stack_backtrace(sp, frames, 64, (uint32_t) depth);

		reply_begin(id);
		json_out_printf(&out,
		    "{\"sp\":%u,\"note\":\"candidates found by scanning; ARM code "
		    "without a frame pointer cannot be unwound exactly\",\"frames\":[",
		    (unsigned) sp);
		for (i = 0; i < got; i++) {
			json_out_printf(&out,
			    "%s{\"at\":%u,\"addr\":%u,\"sym\":",
			    (i != 0) ? "," : "", (unsigned) frames[i].addr,
			    (unsigned) frames[i].value);
			json_out_string(&out, frames[i].sym);
			json_out_printf(&out, ",\"sym_offset\":%u}",
			                (unsigned) frames[i].sym_offset);
		}
		json_out_raw(&out, "]}");
		reply_end();

	} else if (strcmp(method, "heap.stats") == 0) {
		DbgHeapStats h;

		dbg_heap_get_stats(&h);

		reply_begin(id);
		json_out_printf(&out,
		    "{\"calls\":%llu,\"initialises\":%llu,\"allocations\":%llu,"
		    "\"frees\":%llu,\"resizes\":%llu,\"bytes_requested\":%llu,"
		    "\"live_blocks\":%llu,\"peak_live_blocks\":%llu,\"last_heap\":%u}",
		    (unsigned long long) h.calls,
		    (unsigned long long) h.initialises,
		    (unsigned long long) h.allocations,
		    (unsigned long long) h.frees,
		    (unsigned long long) h.resizes,
		    (unsigned long long) h.bytes_requested,
		    (unsigned long long) h.live_blocks,
		    (unsigned long long) h.peak_live_blocks,
		    (unsigned) h.last_heap);
		reply_end();

	} else if (strcmp(method, "heap.describe") == 0) {
		DbgHeapStats h;
		DbgHeapDescriptor d;
		uint32_t addr;

		dbg_heap_get_stats(&h);
		addr = (uint32_t)
		    json_int(json_member(&doc, params, "addr"), (long long) h.last_heap);

		dbg_heap_describe(addr, &d);

		reply_begin(id);
		json_out_printf(&out,
		    "{\"addr\":%u,\"valid\":%s,\"magic\":%u",
		    (unsigned) d.addr, d.valid ? "true" : "false",
		    (unsigned) d.magic);
		if (d.valid) {
			json_out_printf(&out,
			    ",\"free_offset\":%u,\"base_offset\":%u,"
			    "\"end_offset\":%u,\"note\":\"header words as stored; "
			    "the block chain is not walked\"",
			    (unsigned) d.free_offset, (unsigned) d.base_offset,
			    (unsigned) d.end_offset);
		}
		json_out_raw(&out, "}");
		reply_end();

	} else if (strcmp(method, "quit") == 0) {
		rpc_should_quit = 1;
		reply_begin(id);
		json_out_raw(&out, "{\"quitting\":true}");
		reply_end();

	} else {
		reply_error(id, ERR_NO_METHOD, method);
	}
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

void
dbg_rpc_start(void)
{
	InitializeCriticalSection(&queue_lock);
	json_out_init(&out);

	rpc_enabled = 1;
	rpc_should_quit = 0;
	reader_eof = 0;

	reader_thread = CreateThread(NULL, 0, reader_thread_runner, NULL, 0, NULL);
	if (reader_thread == NULL) {
		fprintf(stderr, "rpcemu: could not start the control reader\n");
		rpc_enabled = 0;
		return;
	}

	/* Announce readiness, so a client knows the machine exists before it
	   sends anything. */
	notify_begin("event/ready");
	json_out_printf(&out, "{\"version\":\"%s\"}", VERSION);
	json_out_raw(&out, "}");
	emit_line();
}

int
dbg_rpc_active(void)
{
	return rpc_enabled;
}

int
dbg_rpc_quit_requested(void)
{
	return rpc_should_quit;
}

/**
 * Drain pending requests and publish events.
 *
 * Called from the emulator loop at a point where the machine is consistent.
 */
void
dbg_rpc_poll(void)
{
	DbgStopReason reason;
	uint32_t pc;
	char *line;

	if (!rpc_enabled) {
		return;
	}

	while ((line = queue_take()) != NULL) {
		handle_request(line);
		free(line);
	}

	if (dbg_cpu_take_stop_event(&reason, &pc)) {
		notify_begin("event/stopped");
		json_out_printf(&out,
		    "{\"reason\":\"%s\",\"pc\":%u,\"instructions\":%llu",
		    dbg_stop_reason_name(reason), (unsigned) pc,
		    (unsigned long long) headless_instructions());
		write_symbol_of(pc);
		json_out_raw(&out, "}}");
		emit_line();
	}

	/* Surface RISC OS errors as they happen, so a client waiting on a
	   command learns it failed without polling. */
	if (dbg_vdu_error_seq() != reported_error_seq) {
		reported_error_seq = dbg_vdu_error_seq();

		notify_begin("event/error");
		json_out_raw(&out, "{\"message\":");
		json_out_string(&out, dbg_vdu_last_error());
		json_out_raw(&out, "}}");
		emit_line();
	}
}

void
dbg_rpc_stop(void)
{
	if (!rpc_enabled) {
		return;
	}

	rpc_enabled = 0;

	/* The reader thread is blocked inside fgets(), holding the CRT's lock
	   on stdin. Calling fclose() here would block trying to take that same
	   lock, and joining the thread would wait for a read that will never
	   return. Leave it: the thread owns nothing that outlives the process,
	   and this is the last thing before exit.

	   The queue and output buffer are left alone for the same reason —
	   freeing them while the reader may still touch the queue would be a
	   race for no benefit. */
	if (reader_thread != NULL) {
		CloseHandle(reader_thread);
		reader_thread = NULL;
	}
}
