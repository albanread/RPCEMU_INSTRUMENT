> [!CAUTION]
> 🛑 **REPOSITORY CLOSED — UNMAINTAINED.**
>
> Do not assume the reliability of any data in this repository. It will be
> archived on **15 October 2026**.

# RPCEmu — Instrument Edition

![NEW EXPERIMENT](https://img.shields.io/badge/NEW_EXPERIMENT-c1121f?style=for-the-badge&labelColor=c1121f)

> [!CAUTION]
> **NEW EXPERIMENT.** An unofficial fork, days old and under active change.
> The instruments work and are covered by tests, but interfaces move without
> warning. Not affiliated with or supported by RPCEmu upstream — report
> nothing about this fork to them.

<https://github.com/albanread/RPCEMU_INSTRUMENT>

RPCEmu 0.9.5 with a headless frontend, a JSON-RPC control channel and a debug
and instrumentation core: an Acorn RiscPC that a test harness or an agent can
**halt, step, breakpoint, watch, trace, screenshot, snapshot and script** —
with no dependence on window focus, screen scraping or human timing.

Upstream RPCEmu is an emulator you sit in front of. This fork is the same
machine with the instruments a compiler author needs when the code under test
is the code being generated: when emitted ARM goes wrong the machine takes a
data abort, and by the time RISC OS has printed an error the registers have
moved on. Catching the fault where it is raised, with the register file
intact, is the difference between a bug report and a guess.

The emulated machine is unchanged. Everything here observes it from outside,
so it works on a stock RISC OS from the first instruction of the boot ROM.

## Licence

**GPL-2.0-or-later.** The full text is in [COPYING](COPYING).

This is not a choice. RPCEmu is distributed under the GNU General Public
License, version 2 or (at your option) any later version; this repository is a
derivative work of it, so it carries the same terms and cannot carry weaker
ones. Every file added by this fork ships the same notice as the files around
it, deliberately — a GPL tree with mixed or missing headers is a tree nobody
can safely reuse.

    RPCEmu
    Copyright (C) 2005-2010 Sarah Walker
    Copyright (C) 2005-2010 Matthew Howkins
    Copyright (C) 1997-1999 Russell King
    and the RPCEmu contributors

    Instrument Edition changes
    Copyright (C) 2026 Alban Read

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    for more details.

Third-party code carried in the tree under its own terms: `src/slirp/` is the
QEMU user-mode network stack, MIT licensed, Copyright (c) 2003-2008 Fabrice
Bellard and Copyright (c) 2009 Red Hat, Inc. MIT is GPL-compatible, and the
headless build does not link it at all — but the notices ship with the source
and must stay.

**What is not covered by this licence**, and is not in this repository: the
RISC OS ROM images, the HardDisc4 boot filesystem, and the RISC OS Open DDE
toolchain. Those are separately licensed by RISC OS Open Ltd and are not
redistributable here. You supply your own, exactly as you do for upstream
RPCEmu.

## Relation to upstream

The first commit of this repository is **pristine RPCEmu 0.9.5**, unmodified.
Everything after it is this fork, so `git diff` against that commit is an
exact and auditable statement of what changed:

```bash
git diff $(git log --format=%H | tail -1)..HEAD --stat
```

Upstream is at <https://www.marutan.net/rpcemu/>. Its own build and usage
notes remain in [readme.txt](readme.txt) and still apply to the Qt build.

Additions are kept in their own directories — `src/debug/` and
`src/headless/` — and touch the upstream sources as little as possible. Where
the Microsoft CRT lacks something MinGW provides, the gap is filled by a
force-included shim in `src/headless/compat/` rather than by patching the
files that need it, so the upstream sources stay clean and changes stay easy
to offer back.

Upstream files that did change, and why:

| File | Change |
| --- | --- |
| `arm.c`, `arm_common.c` | per-instruction halt/step check, fault trap hook |
| `cp15.c`, `mem.h` | memory access hooks for watchpoints |
| `keyboard.c/.h`, `i8042.c` | `keyboard_output_pending()` so typing paces against the guest instead of the host clock |
| `vidc20.c/.h` | video scan-out to a frame history |
| `cmos.c`, `iomd.c`, `superio.c`, `snapshot.h` | whole-machine save and restore |
| `rpcemu.c/.h` | headless entry points |

## What is built

`rpcemu-headless.exe` — the emulator core plus a frontend with no GUI toolkit.
Interpreter only: it is the CPU that can be stepped and breakpointed, and the
recompiler stays with the Qt build.

- **Control channel** — JSON-RPC 2.0 over stdin and stdout (`--rpc`), one
  object per line, shaped like an MCP stdio server on purpose. Halt,
  continue, exact single-step, registers, memory, typing, console text,
  screenshots, frame dumps. No ports, no listening socket. A `describe`
  method reports the command surface.
- **Debugging** — breakpoints by address or symbol, run-until, a bounded
  fault trap that stops on the faulting instruction with the register file as
  it left it, and symbols read from the compiler's own ELF.
- **Instruments** — watchpoints on memory reads and writes, a range-filtered
  instruction trace ring, stack read with a heuristic backtrace (a scan, not
  an unwind — ARM code without a frame pointer leaves no chain to follow, and
  it says so rather than pretending to certainty), and heap accounting taken
  at the SWI.
- **Snapshots** — the whole machine saved and restored. A booted machine
  comes back in 0.020s from a 1.5MB file, where booting takes nine seconds.
- **A virtual clock** — guest time driven from instructions retired rather
  than the host's, so the IOMD timer and the video frame interrupt land at
  fixed instruction counts and a run from a snapshot is reproducible. The two
  clocks are separated rather than replaced: `rpcemu_nsec_timer_ticks()` is
  the only clock the guest can observe and becomes virtual, while run limits,
  typing and periodic screenshots keep the host's — typing paced on guest time
  would crawl or race depending on how fast the guest happened to be running.
  Halting becomes exact as a side effect: a stopped CPU retires nothing, so
  guest time does not advance and there is nothing to resynchronise on resume.
- **The guest portal** — a 504-byte RISC OS module that gives the host what
  observing from outside cannot: a legitimate execution context inside the
  machine. There is no safe moment for the host to call into RISC OS, since it
  would be calling from whatever context the machine happened to be in. The
  module cannot be called, but it can call *out* and act on the answer, and
  that inversion is the whole portal. Autoloading is free — anything of
  filetype `&FFA` dropped into `poduleroms/` is built into the expansion card
  ROM and initialised at every boot, which is how HostFS already arrives.
- **Console capture** — output taken as text through the SWI interface.
- `--window` for a live view when you want to watch.

## Building

Windows, clang, CMake, Ninja — no Qt, no MinGW:

```bash
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang && cmake --build build
```

Run it from a directory holding `rpc.cfg`, `roms/` and `hostfs/`.

### The guest portal module

`tools/rpcagent.s` is built separately, because it is ARM code for the
emulated machine rather than x86 for the host. ARMv4, to run on the StrongARM
the emulator is configured as, and linked at zero because a RISC OS module
header holds offsets from the module's own start:

```bash
clang --target=arm-none-eabi -mcpu=strongarm110 -c tools/rpcagent.s -o rpcagent.o
ld.lld --image-base 0 --section-start .text=0 -o rpcagent.elf rpcagent.o
llvm-objcopy -O binary rpcagent.elf "<runtime>/poduleroms/rpcagent,ffa"
```

`<runtime>` is the directory you run the emulator from — the one with
`rpc.cfg` in it. Not the `poduleroms/` in this repository, which is upstream's
copy of the source tree. RISC OS builds everything of filetype `&FFA` found
there into the expansion card ROM and initialises it at boot, so that is the
whole of the installation step.

Keep `rpcagent.elf`. Modules run wherever RISC OS puts them, so the debugger
cannot find the code by itself; `portal.status` reports the address the module
called from, and `sym.load` with that as the bias puts its symbols in the
right place. Without it the trace and breakpoints have nothing to aim at.

## Tests

Four suites drive the whole surface against real code executing inside
RISC OS. The first two build their own ARMv4 test image, so they need nothing
prepared:

```bash
python tools/debug_test.py        # breakpoint, step, fault trap
python tools/instrument_test.py   # watchpoint, trace, stack, heap
python tools/snapshot_test.py     # save, restore, and that restore still runs
python tools/determinism_test.py  # two runs from one snapshot, instruction-identical
```

`tools/rpc_client.py` is the reference client for the control channel.

## Contributing

Changes to upstream files are meant to be offerable upstream: keep them
minimal, keep new work in `src/debug/` and `src/headless/`, and give every new
file the GPL header its neighbours carry. By contributing you agree your
changes ship under GPL-2.0-or-later.
