/*
  RPCEmu - An Acorn system emulator

  Snapshots: save the whole machine, put it back later.

  This is what makes an edit-run-inspect loop quick. Booting RISC OS takes
  about nine seconds; restoring a booted machine takes milliseconds, so an
  agent can afford hundreds of build-run-check cycles instead of a handful.

  Each device module writes its own state (see snapshot.h), because each is
  the only place that knows what its state is. What this file adds is the
  container, the memory, and the ordering.

  ROM is not saved. It is reloaded from the same file, and a hash of it goes
  in the header so a snapshot taken against a different ROM is refused rather
  than resumed into nonsense.

  Derived state is rebuilt rather than stored: the MMU translation caches,
  the video thread's cached copy of the VIDC registers, and the instruction
  fetch cache all follow from state that is saved, and storing them would
  only create a second place for them to be wrong.

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
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "iomd.h"
#include "vidc20.h"
#include "snapshot.h"
#include "dbg.h"

#define SNAPSHOT_MAGIC		0x53504352u	/* 'RCPS' little-endian */
#define SNAPSHOT_VERSION	1

/**
 * Snapshot file header.
 *
 * The machine shape is recorded so a snapshot cannot be restored into a
 * differently configured emulator, where it would appear to work and then
 * misbehave.
 */
typedef struct {
	uint32_t	magic;
	uint32_t	version;
	uint32_t	model;
	uint32_t	mem_size;	/**< megabytes */
	uint32_t	vram_size;	/**< megabytes */
	uint32_t	rom_hash;
	uint64_t	instructions;
} SnapshotHeader;

typedef struct {
	FILE	*f;
	int	error;
} StateIO;

static void
io_write(void *ctx, const void *data, size_t len)
{
	StateIO *io = ctx;

	if (io->error) {
		return;
	}
	if (fwrite(data, 1, len, io->f) != len) {
		io->error = 1;
	}
}

static void
io_read(void *ctx, void *data, size_t len)
{
	StateIO *io = ctx;

	if (io->error) {
		memset(data, 0, len);
		return;
	}
	if (fread(data, 1, len, io->f) != len) {
		io->error = 1;
		memset(data, 0, len);
	}
}

/**
 * A cheap hash of the ROM, to catch a snapshot taken against a different one.
 */
static uint32_t
rom_hash(void)
{
	const uint32_t *p = rom;
	uint32_t h = 2166136261u;
	size_t i;

	if (p == NULL) {
		return 0;
	}

	/* Every 256th word is plenty to tell two ROM images apart, and keeps
	   this off the critical path of a fast restore. */
	for (i = 0; i < ROMSIZE / 4; i += 256) {
		h = (h ^ p[i]) * 16777619u;
	}

	return h;
}

/**
 * Save and load the parts of the machine that live in no single module.
 */
static void
machine_state(SnapshotWrite w, SnapshotRead r, void *ctx)
{
	if (w != NULL) {
		w(ctx, &iomd, sizeof(iomd));
	} else {
		r(ctx, &iomd, sizeof(iomd));
	}
}

/**
 * Bytes in each of the two SIMM 0 banks.
 *
 * mem_rammask is derived from the configured size by mem_reset(), so it is
 * the authority on how much of a bank is real.
 */
static size_t
bank_bytes(void)
{
	return (size_t) mem_rammask + 1;
}

static size_t
vram_bytes(void)
{
	return (size_t) mem_vrammask + 1;
}

/**
 * Bytes in the second SIMM, which only exists on a 256MB machine.
 *
 * mem_reset() allocates it at a fixed 128MB and frees it otherwise, so it is
 * NULL far more often than not.
 */
static size_t
ram1_bytes(void)
{
	return (ram1 != NULL) ? (128 * 1024 * 1024) : 0;
}

#define PAGE_BYTES	4096

/**
 * Write a memory region, skipping pages that are entirely zero.
 *
 * A 128MB machine is mostly empty, and a snapshot has to be quick to be
 * worth having: writing every byte turns a millisecond restore into a
 * multi-second one and the file into something nobody wants to keep. One
 * flag byte per page costs 0.02%.
 */
static void
write_sparse(StateIO *io, const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t offset;

	for (offset = 0; offset < len; offset += PAGE_BYTES) {
		const size_t n = (len - offset < PAGE_BYTES) ?
		                 (len - offset) : PAGE_BYTES;
		uint8_t present = 0;
		size_t i;

		for (i = 0; i < n; i++) {
			if (p[offset + i] != 0) {
				present = 1;
				break;
			}
		}

		io_write(io, &present, 1);
		if (present) {
			io_write(io, p + offset, n);
		}
	}
}

static void
read_sparse(StateIO *io, void *data, size_t len)
{
	uint8_t *p = data;
	size_t offset;

	for (offset = 0; offset < len; offset += PAGE_BYTES) {
		const size_t n = (len - offset < PAGE_BYTES) ?
		                 (len - offset) : PAGE_BYTES;
		uint8_t present = 0;

		io_read(io, &present, 1);
		if (present) {
			io_read(io, p + offset, n);
		} else {
			/* Absent means the page was all zero when saved, and it
			   has to be zeroed now: whatever is there is from the
			   machine's later life. */
			memset(p + offset, 0, n);
		}
	}
}

int
dbg_state_save(const char *path, uint64_t instructions, const char **error)
{
	SnapshotHeader hdr;
	StateIO io;

	*error = NULL;

	io.f = fopen(path, "wb");
	io.error = 0;
	if (io.f == NULL) {
		*error = "cannot create the file";
		return 1;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = SNAPSHOT_MAGIC;
	hdr.version = SNAPSHOT_VERSION;
	hdr.model = (uint32_t) machine.model;
	hdr.mem_size = config.mem_size;
	hdr.vram_size = config.vram_size;
	hdr.rom_hash = rom_hash();
	hdr.instructions = instructions;

	io_write(&io, &hdr, sizeof(hdr));

	/* Memory first: it is almost all of the file, and writing it before the
	   device state keeps the layout simple to reason about. The second
	   SIMM only exists on a 256MB machine and is a null pointer otherwise,
	   so its length carries whether it is there at all. */
	write_sparse(&io, ram00, bank_bytes());
	write_sparse(&io, ram01, bank_bytes());
	{
		const uint64_t n = ram1_bytes();

		io_write(&io, &n, sizeof(n));
		if (n != 0) {
			write_sparse(&io, ram1, (size_t) n);
		}
	}
	write_sparse(&io, vram, vram_bytes());

	arm_state_save(io_write, &io);
	cp15_state_save(io_write, &io);
	machine_state(io_write, NULL, &io);
	vidc_state_save(io_write, &io);
	keyboard_state_save(io_write, &io);
	i8042_state_save(io_write, &io);
	superio_state_save(io_write, &io);
	cmos_state_save(io_write, &io);
	rpcemu_state_save(io_write, &io);

	if (fclose(io.f) != 0) {
		io.error = 1;
	}

	if (io.error) {
		*error = "write failed";
		return 1;
	}

	return 0;
}

int
dbg_state_load(const char *path, uint64_t *instructions, const char **error)
{
	SnapshotHeader hdr;
	StateIO io;

	*error = NULL;

	io.f = fopen(path, "rb");
	io.error = 0;
	if (io.f == NULL) {
		*error = "cannot open the file";
		return 1;
	}

	io_read(&io, &hdr, sizeof(hdr));

	if (io.error || hdr.magic != SNAPSHOT_MAGIC) {
		fclose(io.f);
		*error = "not a snapshot file";
		return 1;
	}
	if (hdr.version != SNAPSHOT_VERSION) {
		fclose(io.f);
		*error = "snapshot is from a different version";
		return 1;
	}
	if (hdr.model != (uint32_t) machine.model ||
	    hdr.mem_size != config.mem_size ||
	    hdr.vram_size != config.vram_size)
	{
		fclose(io.f);
		*error = "snapshot is of a differently configured machine";
		return 1;
	}
	if (hdr.rom_hash != rom_hash()) {
		fclose(io.f);
		*error = "snapshot was taken against a different ROM";
		return 1;
	}

	read_sparse(&io, ram00, bank_bytes());
	read_sparse(&io, ram01, bank_bytes());
	{
		uint64_t n = 0;

		io_read(&io, &n, sizeof(n));
		if (n != ram1_bytes()) {
			fclose(io.f);
			*error = "snapshot has a different second memory bank";
			return 1;
		}
		if (n != 0) {
			read_sparse(&io, ram1, (size_t) n);
		}
	}
	read_sparse(&io, vram, vram_bytes());

	arm_state_load(io_read, &io);
	cp15_state_load(io_read, &io);
	machine_state(NULL, io_read, &io);
	vidc_state_load(io_read, &io);
	keyboard_state_load(io_read, &io);
	i8042_state_load(io_read, &io);
	superio_state_load(io_read, &io);
	cmos_state_load(io_read, &io);
	rpcemu_state_load(io_read, &io);

	fclose(io.f);

	if (io.error) {
		*error = "snapshot is truncated";
		return 1;
	}

	/* Rebuild everything derived from what was just restored. */
	clearmemcache();
	resetcodeblocks();

	if (instructions != NULL) {
		*instructions = hdr.instructions;
	}

	return 0;
}
