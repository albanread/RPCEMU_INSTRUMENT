/*
  RPCEmu - An Acorn system emulator

  Headless main loop.

  The same shape as the Qt frontend's Emulator::mainemuloop(), minus the
  event pump and the window. One extra concept: the CPU can be halted
  independently of the rest of the machine.

  Halting stops the CPU only. The VIDC scan-out thread keeps running, so the
  display stays live and a screenshot taken while stopped is a true picture
  of the machine. Guest-visible time does not advance while the CPU is
  halted — the periodic IOMD and video interrupts are resynchronised on
  resume rather than delivered as a backlog — so the guest cannot tell it
  was stopped. That is what a hardware debugger does, and it is what makes
  breakpoints safe to use on a running RISC OS.

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
#include "vidc20.h"
#include "headless.h"
#include "shmem.h"
#include "dbg.h"

/** Where human-readable messages go. Moves to stderr when the control
    channel takes over stdout. */
static FILE *status_out;
#define say(...)	fprintf(status_out, __VA_ARGS__)

extern void headless_timers_poll(void);
extern void headless_timers_reset(void);
extern int headless_save_config;

/* The instruction total lives in the platform layer so frames can be stamped
   with it; the run state lives in the debug core so the control channel can
   drive it. */
#define total_instructions	headless_instruction_total

static void
usage(const char *argv0)
{
	say(
	"RPCEmu " VERSION " - headless\n"
	"\n"
	"Usage: %s [options]\n"
	"\n"
	"Runs the emulator with no GUI, reading rpc.cfg, roms/ and hostfs/ from\n"
	"the current directory. Video is scanned out to an in-memory frame that\n"
	"can be captured as a PNG; console output is captured as text by\n"
	"intercepting the OS output SWIs.\n"
	"\n"
	"  --seconds N          Run for N seconds of host time, then exit\n"
	"  --instructions N     Run until N instructions have been retired\n"
	"  --until-quiet N      Exit once the machine has printed nothing for N seconds\n"
	"  --screenshot FILE    Write a PNG of the display before exiting\n"
	"  --screenshot-every N Write FILE.NNNN.png every N seconds\n"
	"  --type TEXT          Type TEXT into the machine, \\n for Return\n"
	"  --type-at N          Wait N seconds before typing (default 10)\n"
	"  --type-delay MS      Milliseconds between key transitions (default 15)\n"
	"  --vdu FILE           Write the machine's console output to FILE\n"
	"  --echo               Mirror the machine's console output to stdout\n"
	"  --window             Show a live view window (does not affect the machine)\n"
	"  --halted             Start with the CPU halted (video still runs)\n"
	"  --rpc                Serve JSON-RPC 2.0 on stdin/stdout, one object per\n"
	"                       line; status messages move to stderr. Send\n"
	"                       {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"describe\"} for\n"
	"                       the command surface.\n"
	"  --frame-history N    Frames of screen history to keep (default 8)\n"
	"  --save-config        Allow rpc.cfg to be rewritten on exit\n"
	"  --quiet              Do not mirror errors to stderr\n"
	"  --help               This message\n"
	"\n"
	"With no limit given, runs until interrupted with Ctrl-C.\n",
	argv0);
}

static BOOL WINAPI
console_ctrl_handler(DWORD type)
{
	switch (type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
		fprintf(stderr, "\nrpcemu: stopping\n");
		quited = 1;
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * Take a numbered screenshot, for --screenshot-every.
 */
static void
screenshot_numbered(const char *base, unsigned index)
{
	char path[600];
	const char *dot = strrchr(base, '.');
	const size_t stem = (dot != NULL) ? (size_t) (dot - base) : strlen(base);

	snprintf(path, sizeof(path), "%.*s.%04u.png", (int) stem, base, index);

	if (headless_screenshot(path) == 0) {
		say("rpcemu: wrote %s\n", path);
		fflush(status_out);
	}
}

int
main(int argc, char **argv)
{
	const char *screenshot_path = NULL;
	const char *type_text = NULL;
	const char *vdu_path = NULL;
	FILE *vdu_file = NULL;
	double limit_seconds = 0.0;
	double quiet_seconds = 0.0;
	double type_at = 10.0;
	double screenshot_every = 0.0;
	uint64_t limit_instructions = 0;
	uint64_t next_screenshot_ns = 0;
	uint64_t last_output_ns = 0;
	uint64_t last_output_total = 0;
	unsigned screenshot_index = 0;
	int typed = 0;
	int show_window = 0;
	int start_halted = 0;
	int use_rpc = 0;
	int frame_history = 0;
	int echo_output = 0;
	int virtual_clock = 0;
	uint64_t ns_per_instruction = 0;
	int i;

	status_out = stdout;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else if (strcmp(arg, "--seconds") == 0 && i + 1 < argc) {
			limit_seconds = atof(argv[++i]);
		} else if (strcmp(arg, "--instructions") == 0 && i + 1 < argc) {
			limit_instructions = strtoull(argv[++i], NULL, 0);
		} else if (strcmp(arg, "--until-quiet") == 0 && i + 1 < argc) {
			quiet_seconds = atof(argv[++i]);
		} else if (strcmp(arg, "--screenshot") == 0 && i + 1 < argc) {
			screenshot_path = argv[++i];
		} else if (strcmp(arg, "--screenshot-every") == 0 && i + 1 < argc) {
			screenshot_every = atof(argv[++i]);
		} else if (strcmp(arg, "--type") == 0 && i + 1 < argc) {
			type_text = argv[++i];
		} else if (strcmp(arg, "--type-at") == 0 && i + 1 < argc) {
			type_at = atof(argv[++i]);
		} else if (strcmp(arg, "--type-delay") == 0 && i + 1 < argc) {
			headless_type_set_interval((unsigned) atoi(argv[++i]));
		} else if (strcmp(arg, "--vdu") == 0 && i + 1 < argc) {
			vdu_path = argv[++i];
		} else if (strcmp(arg, "--echo") == 0) {
			echo_output = 1;
		} else if (strcmp(arg, "--window") == 0) {
			show_window = 1;
		} else if (strcmp(arg, "--halted") == 0) {
			start_halted = 1;
		} else if (strcmp(arg, "--deterministic") == 0) {
			virtual_clock = 1;
		} else if (strcmp(arg, "--ns-per-instruction") == 0 && i + 1 < argc) {
			ns_per_instruction = strtoull(argv[++i], NULL, 0);
			virtual_clock = 1;
		} else if (strcmp(arg, "--rpc") == 0) {
			use_rpc = 1;
		} else if (strcmp(arg, "--frame-history") == 0 && i + 1 < argc) {
			frame_history = atoi(argv[++i]);
		} else if (strcmp(arg, "--save-config") == 0) {
			headless_save_config = 1;
		} else if (strcmp(arg, "--quiet") == 0) {
			headless_log_to_stderr(0);
		} else {
			fprintf(stderr, "rpcemu: unknown option '%s'\n", arg);
			usage(argv[0]);
			return 1;
		}
	}

	SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

	headless_plt_init();

	/* Best effort: a machine that cannot publish its frames still runs, and
	   clients fall back to frames.data over the channel. */
	shmem_init();
	dbg_vdu_init();
	dbg_cpu_init();
	dbg_watch_init();
	dbg_trace_init();
	dbg_heap_init();
	dbg_portal_init();

	if (frame_history > 0) {
		headless_frames_set_depth(frame_history);
	}

	/* stdout carries the protocol once the control channel is open, so
	   nothing else may be written to it. */
	if (use_rpc) {
		status_out = stderr;
	}
	if (echo_output) {
		dbg_vdu_set_echo(status_out);
	}

	if (vdu_path != NULL) {
		vdu_file = fopen(vdu_path, "wb");
		if (vdu_file == NULL) {
			fprintf(stderr, "rpcemu: cannot write '%s'\n", vdu_path);
			return 1;
		}
		dbg_vdu_set_file(vdu_file);
	}

	/* Load configuration and log the environment */
	rpcemu_prestart();

	/* Build the machine: memory, ROM, CPU, devices, video thread */
	rpcemu_start();

	headless_timers_reset();

	if (virtual_clock) {
		headless_clock_set_virtual(1, ns_per_instruction);
		say("rpcemu: virtual clock, %llu ns per instruction\n",
		    (unsigned long long) headless_clock_ns_per_instruction());
	}

	if (show_window) {
		headless_window_open();
	}
	if (use_rpc) {
		dbg_rpc_start();
	}

	say("rpcemu: %s, %uMB RAM, %uMB VRAM, headless\n",
	       models[machine.model].name_gui,
	       config.mem_size, config.vram_size);
	if (start_halted) {
		dbg_cpu_halt(DBG_STOP_REQUEST);
		say("rpcemu: CPU halted at reset\n");
	}
	fflush(status_out);

	if (screenshot_every > 0.0) {
		next_screenshot_ns = (uint64_t) (screenshot_every * 1e9);
	}

	while (!quited) {
		uint64_t now;

		if (dbg_cpu_running()) {
			execrpcemu();

			/* inscount is a 32-bit counter the core keeps
			   incrementing; fold it into a wide total the same way
			   the Qt frontend feeds its GUI counter. */
			if (inscount >= 0x20000) {
				total_instructions += inscount & ~0xffffu;
				inscount &= 0xffff;
			}
		} else {
			/* CPU halted: the machine is still alive. Keep pixels
			   flowing so the display and any screenshot stay
			   truthful, and do not burn a core spinning. */
			drawscre++;
			drawscr();
			Sleep(5);
		}

		headless_timers_poll();

		/* Host time: run limits, typing and periodic screenshots are
		   the host's business and must keep their pace whatever the
		   guest's clock is doing. */
		now = headless_host_nsec();

		/* Requests are parsed on the reader thread but acted on here,
		   where the machine is between instructions. */
		dbg_rpc_poll();
		if (dbg_rpc_quit_requested()) {
			quited = 1;
		}

		headless_type_poll(now);

		if (type_text != NULL && !typed && now >= (uint64_t) (type_at * 1e9)) {
			typed = 1;
			say("rpcemu: typing \"%s\"\n", type_text);
			fflush(status_out);
			if (headless_type_string(type_text) < 0) {
				fprintf(stderr,
				        "rpcemu: some characters could not be typed\n");
			}
		}

		if (limit_seconds > 0.0 && now >= (uint64_t) (limit_seconds * 1e9)) {
			quited = 1;
		}

		if (limit_instructions != 0 &&
		    total_instructions + inscount >= limit_instructions)
		{
			quited = 1;
		}

		/* "Quiet" means the machine has stopped printing and has
		   nothing left to type. It is the cheapest reliable signal that
		   a command has finished. */
		if (quiet_seconds > 0.0) {
			if (dbg_vdu_total() != last_output_total ||
			    headless_type_busy() || !typed)
			{
				last_output_total = dbg_vdu_total();
				last_output_ns = now;
			} else if (now - last_output_ns >=
			           (uint64_t) (quiet_seconds * 1e9))
			{
				quited = 1;
			}
		}

		if (screenshot_every > 0.0 && now >= next_screenshot_ns) {
			screenshot_numbered(
			    screenshot_path != NULL ? screenshot_path : "frame.png",
			    screenshot_index++);
			next_screenshot_ns = now + (uint64_t) (screenshot_every * 1e9);
		}
	}

	total_instructions += inscount;

	if (screenshot_path != NULL && screenshot_every <= 0.0) {
		if (headless_screenshot(screenshot_path) == 0) {
			say("rpcemu: wrote %s\n", screenshot_path);
		} else {
			fprintf(stderr, "rpcemu: could not write %s\n", screenshot_path);
		}
	}

	say("rpcemu: %llu instructions retired, PC &%08X\n",
	       (unsigned long long) total_instructions, (unsigned) PC);
	say("rpcemu: %llu bytes of console output, %llu input waits\n",
	       (unsigned long long) dbg_vdu_total(),
	       (unsigned long long) dbg_vdu_input_waits());
	if (type_text != NULL) {
		say("rpcemu: typing sent %u key transitions, %u unechoed\n",
		       headless_type_events_sent(),
		       headless_type_echo_timeouts());
	}
	if (dbg_vdu_last_command() != NULL) {
		say("rpcemu: last command: %s\n", dbg_vdu_last_command());
	}
	if (dbg_vdu_last_error() != NULL) {
		say("rpcemu: last error: %s\n", dbg_vdu_last_error());
	}
	fflush(status_out);

	dbg_vdu_close();
	if (vdu_file != NULL) {
		fclose(vdu_file);
	}

	dbg_rpc_stop();
	headless_window_close();
	endrpcemu();
	shmem_close();
	headless_plt_close();

	return 0;
}
