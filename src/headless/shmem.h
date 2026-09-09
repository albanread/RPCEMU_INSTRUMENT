/*
  RPCEmu - An Acorn system emulator

  Shared memory: the machine's video, visible to another process without
  copying it through a pipe.

  The control channel is line-delimited JSON, which is exactly right for
  commands and exactly wrong for a framebuffer. A 640x256 frame is 640KB;
  putting it through PNG, base64, a pipe and a decoder costs about five
  milliseconds and six copies to deliver bytes that were already sitting in
  memory. This publishes the frame ring into a named section instead, so a
  viewer on the same machine reads the pixels where the video thread wrote
  them.

  The channel stays the control plane. Nothing here replaces a method; a
  client that cannot open the section, or is not on this machine, still has
  frames.data and always will.

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

#ifndef HEADLESS_SHMEM_H
#define HEADLESS_SHMEM_H

#include <stdint.h>

#include "vidc20.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHMEM_MAGIC		0x53435052u	/* 'RPCS' */
#define SHMEM_VERSION		2

/** Frames the shared ring holds. */
#define SHMEM_SLOTS		8

/*
  Bytes reserved per slot.

  A frame is host_xsize * host_ysize * 4 whatever the guest's bit depth, so
  the bound is the largest mode VIDC20 will scan out: 1600x1200 is 7.68MB.
  Eight megabytes covers it with room to spare, and a slot that can always
  hold the frame means the publisher never has to decide to skip one.
*/
#define SHMEM_SLOT_BYTES	(8u * 1024u * 1024u)

/*
  Room set aside inside the section for the emulator's own allocations.

  Eight megabytes, which is exactly the RiscPC's maximum VRAM: putting the
  framebuffer here rather than in a private malloc is what lets a viewer draw
  from the guest's own pixels instead of from a copy of them.
*/
#define SHMEM_RESERVE_BYTES	(8u * 1024u * 1024u)

/**
 * What the video hardware was doing for one frame, flattened.
 *
 * A copy of the interesting parts of VidcFrameState rather than the struct
 * itself: this layout is read by other programs, so it may not change shape
 * when an internal structure does.
 */
typedef struct {
	uint32_t	seq;		/**< Even: stable. Odd: being written. */
	uint32_t	reserved;

	uint64_t	serial;		/**< Frame serial, matches frames.info */
	uint64_t	when_ns;	/**< Host time the frame was produced */
	uint64_t	instructions;	/**< Instructions retired at that point */
	uint64_t	mode_serial;	/**< Moves only when the video mode does */

	int32_t		xsize;		/**< Guest width in pixels */
	int32_t		ysize;		/**< Guest height in pixels */
	int32_t		host_xsize;	/**< Width after pixel doubling */
	int32_t		host_ysize;	/**< Height after pixel doubling */
	int32_t		doublesize;	/**< Bit 0 doubles width, bit 1 height */
	int32_t		bpp;		/**< Guest bits per pixel */

	uint32_t	byte_length;	/**< Pixel bytes valid in this slot */
	uint32_t	border;		/**< Border colour, xRGB */

	int32_t		cursor_x;
	int32_t		cursor_y;
	int32_t		cursor_height;
	int32_t		pad;
} ShmemSlot;

/**
 * The guest's framebuffer, described well enough to draw from directly.
 *
 * A renderer with a GPU never needs the expanded frames at all: it maps the
 * VRAM this header points at, uploads the packed pixels and this palette,
 * and lets a shader do the depth expansion, the pixel doubling and the
 * scaling in one pass. For a 1bpp mode that is 20KB a frame instead of
 * 640KB, and no CPU touches a pixel on either side.
 *
 * Written under the same kind of sequence lock as a frame slot.
 */
typedef struct {
	uint32_t	seq;		/**< Even: stable. Odd: being written. */
	uint32_t	bpp_code;	/**< VIDC's own encoding */
	uint32_t	bits_per_pixel;	/**< 1, 2, 4, 8, 16 or 32; 0 if invalid */
	uint32_t	video_in_dram;	/**< Framebuffer is in DRAM, not VRAM */

	uint32_t	fb_offset;	/**< Byte offset within that bank */
	uint32_t	fb_bytes;	/**< Bytes the visible area occupies */
	int32_t		xsize;
	int32_t		ysize;

	int32_t		host_xsize;
	int32_t		host_ysize;
	int32_t		doublesize;	/**< Bit 0 doubles width, bit 1 height */
	uint32_t	border;

	int32_t		cursor_x;
	int32_t		cursor_y;
	int32_t		cursor_height;
	uint32_t	pad;

	uint64_t	serial;		/**< Frame serial this describes */

	uint32_t	palette[256];	/**< Host xRGB, ready to index */
	uint32_t	cursor_palette[3];
	uint32_t	pad2;
} ShmemVideo;

/**
 * The section header.
 *
 * Fixed layout, published version, offsets given rather than implied: a
 * reader in another language should be able to work entirely from this
 * without being recompiled alongside the emulator.
 */
typedef struct {
	uint32_t	magic;
	uint32_t	version;
	uint32_t	header_bytes;
	uint32_t	pid;

	uint32_t	slot_count;
	uint32_t	slot_bytes;
	uint64_t	slots_offset;	/**< Pixels start here, slot 0 first */

	/*
	  Published after the slot it names is fully written, so a reader that
	  takes this index and then reads that slot's seq twice cannot be
	  handed a torn frame.
	*/
	volatile uint64_t newest;	/**< Serial of the newest complete frame */
	volatile uint64_t newest_index;	/**< Slot that frame is in */
	volatile uint64_t writes;	/**< Frames published since start */

	/* Guest memory, when it has been mapped here. Zero length means the
	   emulator is not sharing it and mem.read is the way to read memory. */
	uint64_t	ram_offset;
	uint64_t	ram_bytes;
	uint64_t	vram_offset;
	uint64_t	vram_bytes;
	uint32_t	ram_banks;	/**< Contiguous banks within ram_bytes */
	uint32_t	ram_bank_bytes;

	ShmemVideo	video;

	ShmemSlot	slots[SHMEM_SLOTS];
} ShmemHeader;

/**
 * Create the section and the frame event.
 *
 * Failure is not fatal and is not an error: the emulator runs exactly as it
 * did before, and clients fall back to frames.data.
 *
 * @return 0 if the section is available, non-zero if it is not
 */
extern int shmem_init(void);

extern void shmem_close(void);

/** Whether frames are being published. */
extern int shmem_active(void);

/** The section's name, or NULL. Reported over the channel by shmem.info. */
extern const char *shmem_section_name(void);

/** The frame event's name, or NULL. */
extern const char *shmem_event_name(void);

/** Bytes reserved for the section. */
extern uint64_t shmem_section_bytes(void);

/**
 * Publish one frame.
 *
 * Called from the video thread, immediately after the frame lands in the
 * local ring. Writes a slot under a sequence lock and signals the event; it
 * never waits for a reader, because a viewer that stalls must not be able to
 * stall the machine.
 */
extern void shmem_publish_frame(const uint32_t *pixels, uint32_t byte_length,
                                const ShmemSlot *meta);

/**
 * Publish what the video hardware is doing, for a renderer drawing from VRAM.
 *
 * Called from the video thread alongside the frame.
 */
extern void shmem_publish_video(const VidcSharedState *state, uint64_t serial);

/**
 * Reserve a block inside the section for the emulator's own use.
 *
 * Used to put VRAM there, so a viewer sees the guest's framebuffer as the
 * guest writes it rather than a copy taken afterwards. Reservations are
 * permanent and must all be made before the machine starts.
 *
 * @param bytes How much
 * @param offset Receives the byte offset within the section
 * @return A pointer into the section, or NULL if it cannot be satisfied
 */
extern void *shmem_reserve(uint64_t bytes, uint64_t *offset);

/**
 * Whether a pointer lies inside the section.
 *
 * Memory reserved here did not come from the heap, so freeing it is
 * undefined behaviour. Anything that frees a buffer which might have been
 * reserved has to ask first.
 */
extern int shmem_owns(const void *p);

/** Record that a reservation holds VRAM, so clients can find it. */
extern void shmem_set_vram(uint64_t offset, uint64_t bytes);

extern ShmemHeader *shmem_header(void);

#ifdef __cplusplus
}
#endif

#endif /* HEADLESS_SHMEM_H */
