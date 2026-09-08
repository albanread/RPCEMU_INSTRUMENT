/*
  RPCEmu - An Acorn system emulator

  Headless frontend: interfaces shared between the headless platform layer,
  the frame store and the main loop.

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

#ifndef HEADLESS_H
#define HEADLESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A copy of the most recent frame produced by the VIDC scan-out thread.
 *
 * The video thread writes into this under frame_lock; readers (screenshots,
 * and later the control server) take the same lock. Holding it never blocks
 * the emulator thread, which is why this is a copy rather than a borrowed
 * pointer into vidc20's own bitmap.
 */
typedef struct {
	uint32_t	*pixels;	/**< xRGB8888, xsize * ysize words */
	int		xsize;		/**< Width in pixels of the emulated display */
	int		ysize;		/**< Height in pixels of the emulated display */
	int		host_xsize;	/**< Width after any pixel doubling */
	int		host_ysize;	/**< Height after any pixel doubling */
	int		doublesize;	/**< Doubling flags in force for this frame */
	uint64_t	serial;		/**< Increments once per delivered frame */
} HeadlessFrame;

/* Platform layer lifecycle */
extern void headless_plt_init(void);
extern void headless_plt_close(void);

/* Frame store */
extern uint64_t headless_frame_serial(void);
extern int headless_frame_copy(HeadlessFrame *out);
extern void headless_frame_free(HeadlessFrame *frame);

/**
 * Write the most recent frame to a PNG file.
 *
 * Safe to call while the CPU is halted: the frame store holds the last
 * scanned-out image, which stays valid because a halted CPU cannot change
 * VRAM.
 *
 * @param path Destination file
 * @return 0 on success, non-zero on failure
 */
extern int headless_screenshot(const char *path);

/* Minimal PNG writer (headless/png_write.c) */
extern int png_write_xrgb(const char *path, const uint32_t *pixels,
                          int width, int height, int stride_words);

/* Periodic machine events (IOMD tick, video frame), owned by the platform
   layer because it owns the host clock */
extern void headless_timers_poll(void);
extern void headless_timers_reset(void);

/* Provided by the platform layer, called by the core's video path */
extern void vblupdate(void);

/* Synthetic typing (headless/keyboard_type.c) */
extern int headless_type_string(const char *text);
extern void headless_type_poll(uint64_t now_ns);
extern int headless_type_busy(void);
extern void headless_type_set_interval(unsigned ms);
extern unsigned headless_type_echo_timeouts(void);
extern unsigned headless_type_events_sent(void);

/* Optional live view window (headless/window_win.c) */
extern void headless_window_open(void);
extern void headless_window_close(void);

/* Log destination control, used before the emulator is initialised */
extern void headless_log_to_stderr(int enable);

#ifdef __cplusplus
}
#endif

#endif /* HEADLESS_H */
