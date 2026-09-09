/*
  RPCEmu - An Acorn system emulator

  Machine state save and restore.

  Each device module saves and loads its own state, because each module is
  the only place that knows what its state is. A central function reaching
  into every module's internals would go stale the moment any of them gained
  a field, and the failure mode of a missed field is the worst kind: a
  machine that resumes happily and misbehaves ten seconds later.

  The interface is a pair of callbacks rather than a buffer or a FILE, so
  the modules need no knowledge of where the state is going.

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

#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SnapshotWrite)(void *ctx, const void *data, size_t len);
typedef void (*SnapshotRead)(void *ctx, void *data, size_t len);

/* Each module writes and reads its own state, in the same order. */
extern void arm_state_save(SnapshotWrite w, void *ctx);
extern void arm_state_load(SnapshotRead r, void *ctx);

extern void cp15_state_save(SnapshotWrite w, void *ctx);
extern void cp15_state_load(SnapshotRead r, void *ctx);

extern void vidc_state_save(SnapshotWrite w, void *ctx);
extern void vidc_state_load(SnapshotRead r, void *ctx);

extern void keyboard_state_save(SnapshotWrite w, void *ctx);
extern void keyboard_state_load(SnapshotRead r, void *ctx);

extern void i8042_state_save(SnapshotWrite w, void *ctx);
extern void i8042_state_load(SnapshotRead r, void *ctx);

extern void superio_state_save(SnapshotWrite w, void *ctx);
extern void superio_state_load(SnapshotRead r, void *ctx);

extern void cmos_state_save(SnapshotWrite w, void *ctx);
extern void cmos_state_load(SnapshotRead r, void *ctx);

extern void rpcemu_state_save(SnapshotWrite w, void *ctx);
extern void rpcemu_state_load(SnapshotRead r, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_H */
