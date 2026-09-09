/*
  RPCEmu - An Acorn system emulator

  Headless platform layer.

  Supplies everything the emulator core expects a frontend to provide, with
  no GUI toolkit: logging, the VIDC scan-out thread, the frame store, host
  timing, and stubs for the audio path.

  Two deliberate departures from the Qt frontend:

  1. The VIDC scan-out thread is a real thread here too, using Win32
     primitives rather than pthreads. VIDC is separate hardware from the
     CPU; keeping it on its own thread means a halted CPU still has a
     display, and pixel conversion overlaps emulation when running free.

  2. Frames are copied into a frame store rather than handed to a GUI. That
     copy is what makes a screenshot possible at any moment, including while
     the CPU is stopped at a breakpoint.

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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "vidc20.h"
#include "sound.h"
#include "iomd.h"
#include "headless.h"
#include "shmem.h"

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

static int log_to_stderr = 1;

void
headless_log_to_stderr(int enable)
{
	log_to_stderr = enable;
}

/**
 * Report a non-fatal problem.
 *
 * The Qt frontend puts this in a message box; here it goes to the log and
 * to stderr, where an agent driving the process will actually see it.
 */
void
error(const char *format, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	rpclog("ERROR: %s", buf);
	if (log_to_stderr) {
		fprintf(stderr, "rpcemu: error: %s", buf);
		fflush(stderr);
	}
}

/**
 * Report an unrecoverable problem and exit.
 */
void
fatal(const char *format, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	rpclog("FATAL: %s", buf);
	fprintf(stderr, "rpcemu: fatal: %s", buf);
	fflush(stderr);

	exit(EXIT_FAILURE);
}

void
rpcemu_log_platform(void)
{
	OSVERSIONINFOA info;
	SYSTEM_INFO sysinfo;

	memset(&info, 0, sizeof(info));
	info.dwOSVersionInfoSize = sizeof(info);

	GetSystemInfo(&sysinfo);

	rpclog("Platform: Windows (headless frontend), %u processors\n",
	       (unsigned) sysinfo.dwNumberOfProcessors);
}

/* ------------------------------------------------------------------ */
/* Host timing                                                        */
/* ------------------------------------------------------------------ */

static LARGE_INTEGER perf_freq;
static LARGE_INTEGER perf_origin;

/* The virtual clock. When on, time as the guest sees it is a function of
   instructions retired rather than of the host, which is what makes a run
   reproducible: the IOMD timer and the video frame interrupt then land at
   fixed instruction counts instead of wherever the host happened to be.

   It also makes halting exact. A stopped CPU retires nothing, so guest time
   simply does not advance, and the guest cannot tell it was stopped - no
   resynchronisation needed, because there is nothing to resynchronise. */
static int virtual_clock;
static uint64_t ns_per_instruction = 10;	/* 100 MIPS, StrongARM-ish */
static uint64_t virtual_offset;			/* advanced when the CPU idles */

/**
 * Nanoseconds since the emulator started, on the host's clock.
 *
 * For pacing things that belong to the host - typing, run limits, periodic
 * screenshots - which must keep working at whatever rate the guest runs.
 */
uint64_t
headless_host_nsec(void)
{
	LARGE_INTEGER now;

	QueryPerformanceCounter(&now);

	return (uint64_t) ((now.QuadPart - perf_origin.QuadPart) *
	                   1000000000ULL / (uint64_t) perf_freq.QuadPart);
}

/**
 * Nanoseconds as the emulated machine sees them.
 *
 * This is the only clock the guest can observe: IOMD's counters are driven
 * from it, and so are the periodic interrupts. Everything about determinism
 * turns on what this returns.
 */
uint64_t
rpcemu_nsec_timer_ticks(void)
{
	if (virtual_clock) {
		return virtual_offset +
		       headless_instructions() * ns_per_instruction;
	}

	return headless_host_nsec();
}

void
headless_clock_set_virtual(int enable, uint64_t ns_per_insn)
{
	virtual_clock = enable;
	if (ns_per_insn != 0) {
		ns_per_instruction = ns_per_insn;
	}
	virtual_offset = 0;
	headless_timers_reset();
}

int
headless_clock_is_virtual(void)
{
	return virtual_clock;
}

uint64_t
headless_clock_ns_per_instruction(void)
{
	return ns_per_instruction;
}

/* Scheduling state for the two periodic machine events. Shared between the
   main loop and rpcemu_idle_process_events(), which the core calls from
   inside its idle loop. */

#define IOMD_TIMER_INTERVAL_NS	2000000		/* 2 ms, 500 Hz */

static uint64_t iomd_timer_next;
static uint64_t video_timer_next;
static uint64_t video_timer_interval;

uint64_t headless_iomd_timer_count;
uint64_t headless_video_timer_count;

/**
 * Fire the IOMD and video periodic events if their time has come.
 *
 * Called both from the main loop and, via rpcemu_idle_process_events(),
 * from the core's idle path.
 */
void
headless_timers_poll(void)
{
	const uint64_t elapsed = rpcemu_nsec_timer_ticks();

	if (elapsed >= iomd_timer_next) {
		headless_iomd_timer_count++;
		gentimerirq((int64_t) elapsed);
		iomd_timer_next += IOMD_TIMER_INTERVAL_NS;

		/* If we have fallen a long way behind (host hiccup, or the CPU
		   was halted in the debugger) resynchronise rather than firing
		   a burst of catch-up interrupts at the guest. */
		if (elapsed > iomd_timer_next + IOMD_TIMER_INTERVAL_NS * 16) {
			iomd_timer_next = elapsed + IOMD_TIMER_INTERVAL_NS;
		}
	}

	if (elapsed >= video_timer_next) {
		headless_video_timer_count++;
		vblupdate();
		video_timer_next += video_timer_interval;

		if (elapsed > video_timer_next + video_timer_interval * 4) {
			video_timer_next = elapsed + video_timer_interval;
		}
	}
}

/**
 * Reset the periodic event schedule to start from now.
 *
 * Called after a long pause (startup, or resuming a halted CPU) so the guest
 * does not see a backlog of timer interrupts.
 */
void
headless_timers_reset(void)
{
	const uint64_t now = rpcemu_nsec_timer_ticks();

	video_timer_interval = 1000000000ULL / (uint64_t) (config.refresh > 0 ? config.refresh : 60);
	iomd_timer_next  = now + IOMD_TIMER_INTERVAL_NS;
	video_timer_next = now + video_timer_interval;

	NOT_USED(now);
}

void
rpcemu_idle_process_events(void)
{
	/* On the virtual clock an idle CPU retires no instructions, so time
	   would never reach the next interrupt and the machine would wedge.
	   Idling is therefore a jump straight to the next scheduled event,
	   which is both correct and free: waiting costs nothing when the
	   clock is ours. */
	if (virtual_clock) {
		const uint64_t now = rpcemu_nsec_timer_ticks();
		const uint64_t next = (iomd_timer_next < video_timer_next) ?
		                      iomd_timer_next : video_timer_next;

		if (next > now) {
			virtual_offset += next - now;
		}
	}

	headless_timers_poll();
}

/* ------------------------------------------------------------------ */
/* Frame store                                                        */
/* ------------------------------------------------------------------ */

static CRITICAL_SECTION frame_lock;

/** One slot of the rotating frame history. */
typedef struct {
	HeadlessFrame	frame;
	size_t		capacity;	/**< Words allocated in frame.pixels */
} FrameSlot;

static FrameSlot ring[HEADLESS_FRAME_HISTORY_MAX];
static int ring_depth = 8;	/**< Slots in use */
static int ring_next;		/**< Slot the next frame goes into */
static int ring_count;		/**< Slots holding a frame, up to ring_depth */
static uint64_t frame_serial;	/**< Serial of the newest frame */
static uint64_t mode_serial;	/**< Increments only when the video mode changes */
static VidcFrameState last_mode;

uint64_t headless_instruction_total;

/**
 * Instructions retired, exactly.
 *
 * headless_instruction_total only moves when the main loop folds inscount
 * into it, in units of 65536; adding the unfolded remainder makes single
 * steps visible.
 */
uint64_t
headless_instructions(void)
{
	return headless_instruction_total + inscount;
}

uint64_t
headless_frame_serial(void)
{
	uint64_t serial;

	EnterCriticalSection(&frame_lock);
	serial = frame_serial;
	LeaveCriticalSection(&frame_lock);

	return serial;
}

/**
 * Set how many frames of history to keep.
 *
 * Frames already held beyond the new depth are forgotten.
 */
void
headless_frames_set_depth(int frames)
{
	if (frames < 1) {
		frames = 1;
	}
	if (frames > HEADLESS_FRAME_HISTORY_MAX) {
		frames = HEADLESS_FRAME_HISTORY_MAX;
	}

	EnterCriticalSection(&frame_lock);
	ring_depth = frames;
	ring_next = 0;
	ring_count = 0;
	LeaveCriticalSection(&frame_lock);
}

int
headless_frames_available(void)
{
	int n;

	EnterCriticalSection(&frame_lock);
	n = ring_count;
	LeaveCriticalSection(&frame_lock);

	return n;
}

int
headless_frame_copy_at(int age, HeadlessFrame *out)
{
	const FrameSlot *slot;
	size_t words;
	int index;

	EnterCriticalSection(&frame_lock);

	if (age < 0 || age >= ring_count) {
		LeaveCriticalSection(&frame_lock);
		return 1;
	}

	/* ring_next points past the newest, so the newest is at -1 */
	index = ((ring_next - 1 - age) % ring_depth + ring_depth) % ring_depth;
	slot = &ring[index];

	if (slot->frame.pixels == NULL ||
	    slot->frame.xsize <= 0 || slot->frame.ysize <= 0)
	{
		LeaveCriticalSection(&frame_lock);
		return 1;
	}

	*out = slot->frame;
	words = (size_t) slot->frame.xsize * (size_t) slot->frame.ysize;
	out->pixels = malloc(words * sizeof(uint32_t));
	if (out->pixels == NULL) {
		LeaveCriticalSection(&frame_lock);
		return 1;
	}
	memcpy(out->pixels, slot->frame.pixels, words * sizeof(uint32_t));

	LeaveCriticalSection(&frame_lock);

	return 0;
}

/**
 * Take a private copy of the latest frame.
 *
 * The caller owns out->pixels and must release it with
 * headless_frame_free().
 *
 * @return 0 on success, non-zero if no frame has been produced yet
 */
int
headless_frame_copy(HeadlessFrame *out)
{
	return headless_frame_copy_at(0, out);
}

void
headless_frame_free(HeadlessFrame *f)
{
	if (f != NULL) {
		free(f->pixels);
		f->pixels = NULL;
	}
}

int
headless_screenshot(const char *path)
{
	HeadlessFrame f;
	int ret;

	if (headless_frame_copy(&f) != 0) {
		error("screenshot: no frame available yet\n");
		return 1;
	}

	ret = png_write_xrgb(path, f.pixels, f.xsize, f.ysize, f.xsize);
	headless_frame_free(&f);

	return ret;
}

int
headless_frames_save(const char *prefix)
{
	const int held = headless_frames_available();
	int written = 0;
	int age;

	/* Oldest first, so the numbering runs in playback order. */
	for (age = held - 1; age >= 0; age--) {
		HeadlessFrame f;
		char path[600];

		if (headless_frame_copy_at(age, &f) != 0) {
			continue;
		}

		snprintf(path, sizeof(path), "%s.%04d.png", prefix, written);

		if (png_write_xrgb(path, f.pixels, f.xsize, f.ysize, f.xsize) == 0) {
			written++;
		}
		headless_frame_free(&f);
	}

	return written;
}

/**
 * Receive a completed frame from the VIDC scan-out thread.
 *
 * thread: video
 *
 * The yl/yh dirty range is ignored: we keep a whole-frame copy so that a
 * screenshot is always a complete image, whatever happened to be dirty.
 */
void
rpcemu_video_update(const uint32_t *buffer, int xsize, int ysize,
                    int yl, int yh, int double_size,
                    int host_xsize, int host_ysize)
{
	size_t words;

	NOT_USED(yl);
	NOT_USED(yh);

	if (buffer == NULL || xsize <= 0 || ysize <= 0) {
		return;
	}

	words = (size_t) xsize * (size_t) ysize;

	EnterCriticalSection(&frame_lock);

	{
		FrameSlot *slot = &ring[ring_next];

		if (words > slot->capacity) {
			uint32_t *p = realloc(slot->frame.pixels,
			                      words * sizeof(uint32_t));

			if (p == NULL) {
				LeaveCriticalSection(&frame_lock);
				return;
			}
			slot->frame.pixels = p;
			slot->capacity = words;
		}

		/* Ask the video hardware what it was doing for this frame, and
		   advance the mode serial only when the answer changes: a
		   per-frame serial would say nothing, one that moves only on a
		   real change points straight at the frame it happened on. */
		vidc_frame_state(&slot->frame.video);
		if (memcmp(&slot->frame.video, &last_mode, sizeof(last_mode)) != 0) {
			last_mode = slot->frame.video;
			mode_serial++;
		}
		slot->frame.mode_serial = mode_serial;

		memcpy(slot->frame.pixels, buffer, words * sizeof(uint32_t));
		slot->frame.xsize        = xsize;
		slot->frame.ysize        = ysize;
		slot->frame.host_xsize   = host_xsize;
		slot->frame.host_ysize   = host_ysize;
		slot->frame.doublesize   = double_size;
		slot->frame.serial       = ++frame_serial;
		slot->frame.when_ns      = headless_host_nsec();
		slot->frame.instructions = headless_instruction_total;

		ring_next = (ring_next + 1) % ring_depth;
		if (ring_count < ring_depth) {
			ring_count++;
		}

		/* Publish the same frame where a viewer on this machine can
		   read it directly. One extra memcpy on the video thread buys
		   a client the whole PNG, base64 and pipe path back. */
		if (shmem_active()) {
			ShmemSlot meta;

			memset(&meta, 0, sizeof(meta));
			meta.serial        = slot->frame.serial;
			meta.when_ns       = slot->frame.when_ns;
			meta.instructions  = slot->frame.instructions;
			meta.mode_serial   = slot->frame.mode_serial;
			meta.xsize         = xsize;
			meta.ysize         = ysize;
			meta.host_xsize    = host_xsize;
			meta.host_ysize    = host_ysize;
			meta.doublesize    = double_size;
			meta.bpp           = (int32_t) slot->frame.video.bits_per_pixel;
			meta.border        = slot->frame.video.border_colour;
			meta.cursor_x      = slot->frame.video.cursorx;
			meta.cursor_y      = slot->frame.video.cursory;
			meta.cursor_height = slot->frame.video.cursorheight;

			shmem_publish_frame(slot->frame.pixels,
			                    (uint32_t) (words * sizeof(uint32_t)),
			                    &meta);
		}
	}

	LeaveCriticalSection(&frame_lock);
}

/* ------------------------------------------------------------------ */
/* VIDC scan-out thread                                               */
/* ------------------------------------------------------------------ */

/* The contract, unchanged from the Qt frontend:
     - the machine thread calls vidctrymutex() and only touches the cached
       video state if it succeeds, so it never blocks on the video thread;
     - vidcwakeupthread() signals work is pending;
     - the video thread holds the mutex for the whole of vidcthread(). */

static CRITICAL_SECTION video_mutex;
static CONDITION_VARIABLE video_cond;
static HANDLE video_thread_handle;
static volatile LONG video_thread_quit;

static DWORD WINAPI
video_thread_runner(LPVOID param)
{
	NOT_USED(param);

	EnterCriticalSection(&video_mutex);

	while (!video_thread_quit) {
		/* Releases the section while waiting, reacquires on wake.
		   Spurious wakeups are filtered by vidcthread()'s own
		   threadpending check. */
		SleepConditionVariableCS(&video_cond, &video_mutex, 100);

		if (!video_thread_quit) {
			vidcthread();
		}
	}

	LeaveCriticalSection(&video_mutex);

	return 0;
}

void
vidcstartthread(void)
{
	video_thread_quit = 0;

	video_thread_handle = CreateThread(NULL, 0, video_thread_runner,
	                                   NULL, 0, NULL);
	if (video_thread_handle == NULL) {
		fatal("Couldn't create vidc thread\n");
	}
}

void
vidcendthread(void)
{
	if (video_thread_handle == NULL) {
		return;
	}

	InterlockedExchange(&video_thread_quit, 1);
	WakeConditionVariable(&video_cond);

	if (WaitForSingleObject(video_thread_handle, 2000) == WAIT_TIMEOUT) {
		rpclog("vidcendthread: video thread did not stop, abandoning it\n");
	}

	CloseHandle(video_thread_handle);
	video_thread_handle = NULL;
}

void
vidcwakeupthread(void)
{
	WakeConditionVariable(&video_cond);
}

int
vidctrymutex(void)
{
	return TryEnterCriticalSection(&video_mutex) ? 1 : 0;
}

void
vidcreleasemutex(void)
{
	LeaveCriticalSection(&video_mutex);
}

/**
 * Video flyback: schedule a scan-out pass.
 *
 * Guest-visible flyback interrupts come from iomd_flyback(), driven by the
 * machine's own timing; this only asks for pixels to be converted.
 */
void
vblupdate(void)
{
	drawscre++;
}

/* ------------------------------------------------------------------ */
/* Input (host pointer feedback)                                      */
/* ------------------------------------------------------------------ */

/**
 * Move the host mouse pointer to follow the emulated one.
 *
 * Meaningless without a window; the emulated pointer position is still
 * tracked by the core, so mousehack continues to work.
 */
void
rpcemu_move_host_mouse(uint16_t x, uint16_t y)
{
	NOT_USED(x);
	NOT_USED(y);
}

/* ------------------------------------------------------------------ */
/* Sound                                                              */
/* ------------------------------------------------------------------ */

/* No audio output. sound_enabled should be 0 in the config; these exist so
   the core links and behaves if it is not. */

void
sound_thread_start(void)
{
}

void
sound_thread_wakeup(void)
{
}

void
sound_thread_close(void)
{
}

void
plt_sound_init(uint32_t bufferlen)
{
	NOT_USED(bufferlen);
}

void
plt_sound_restart(void)
{
}

void
plt_sound_pause(void)
{
}

int32_t
plt_sound_buffer_free(void)
{
	/* Claim the buffer is always full so the core never queues samples. */
	return 0;
}

void
plt_sound_buffer_play(uint32_t samplerate, const char *buffer, uint32_t length)
{
	NOT_USED(samplerate);
	NOT_USED(buffer);
	NOT_USED(length);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

void
headless_plt_init(void)
{
	QueryPerformanceFrequency(&perf_freq);
	QueryPerformanceCounter(&perf_origin);

	InitializeCriticalSection(&frame_lock);
	InitializeCriticalSection(&video_mutex);
	InitializeConditionVariable(&video_cond);

	memset(ring, 0, sizeof(ring));
	ring_next = 0;
	ring_count = 0;
	frame_serial = 0;
	mode_serial = 0;
	memset(&last_mode, 0, sizeof(last_mode));
}

void
headless_plt_close(void)
{
	int i;

	EnterCriticalSection(&frame_lock);
	for (i = 0; i < HEADLESS_FRAME_HISTORY_MAX; i++) {
		free(ring[i].frame.pixels);
		ring[i].frame.pixels = NULL;
		ring[i].capacity = 0;
	}
	ring_count = 0;
	LeaveCriticalSection(&frame_lock);

	DeleteCriticalSection(&frame_lock);
	DeleteCriticalSection(&video_mutex);
}
