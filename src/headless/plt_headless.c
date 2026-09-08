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

/**
 * Nanoseconds since the emulator started.
 *
 * This is the single point at which the machine reads host time. Replacing
 * it with a function of the instruction count is what turns the emulator
 * deterministic (see RPCEMU-AGENT.md, "Virtual clock").
 */
uint64_t
rpcemu_nsec_timer_ticks(void)
{
	LARGE_INTEGER now;

	QueryPerformanceCounter(&now);

	return (uint64_t) ((now.QuadPart - perf_origin.QuadPart) *
	                   1000000000ULL / (uint64_t) perf_freq.QuadPart);
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
}

void
rpcemu_idle_process_events(void)
{
	headless_timers_poll();
}

/* ------------------------------------------------------------------ */
/* Frame store                                                        */
/* ------------------------------------------------------------------ */

static CRITICAL_SECTION frame_lock;
static HeadlessFrame frame;
static size_t frame_capacity;		/* words allocated in frame.pixels */

uint64_t
headless_frame_serial(void)
{
	uint64_t serial;

	EnterCriticalSection(&frame_lock);
	serial = frame.serial;
	LeaveCriticalSection(&frame_lock);

	return serial;
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
	size_t words;

	EnterCriticalSection(&frame_lock);

	if (frame.pixels == NULL || frame.xsize <= 0 || frame.ysize <= 0) {
		LeaveCriticalSection(&frame_lock);
		return 1;
	}

	*out = frame;
	words = (size_t) frame.xsize * (size_t) frame.ysize;
	out->pixels = malloc(words * sizeof(uint32_t));
	if (out->pixels == NULL) {
		LeaveCriticalSection(&frame_lock);
		return 1;
	}
	memcpy(out->pixels, frame.pixels, words * sizeof(uint32_t));

	LeaveCriticalSection(&frame_lock);

	return 0;
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

	if (words > frame_capacity) {
		uint32_t *p = realloc(frame.pixels, words * sizeof(uint32_t));

		if (p == NULL) {
			LeaveCriticalSection(&frame_lock);
			return;
		}
		frame.pixels = p;
		frame_capacity = words;
	}

	memcpy(frame.pixels, buffer, words * sizeof(uint32_t));
	frame.xsize      = xsize;
	frame.ysize      = ysize;
	frame.host_xsize = host_xsize;
	frame.host_ysize = host_ysize;
	frame.doublesize = double_size;
	frame.serial++;

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

	memset(&frame, 0, sizeof(frame));
	frame_capacity = 0;
}

void
headless_plt_close(void)
{
	EnterCriticalSection(&frame_lock);
	free(frame.pixels);
	frame.pixels = NULL;
	frame_capacity = 0;
	LeaveCriticalSection(&frame_lock);

	DeleteCriticalSection(&frame_lock);
	DeleteCriticalSection(&video_mutex);
}
