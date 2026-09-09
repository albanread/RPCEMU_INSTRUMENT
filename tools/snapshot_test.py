"""Check that a snapshot restores the machine exactly, and that it still runs.

Two things have to hold. Restoring must put every saved register and byte
back as it was - a missed field is the classic snapshot bug, and it shows up
as a machine that resumes happily and misbehaves later. And the machine has
to keep working afterwards: state that restores byte-perfect but leaves the
guest confused is no better.

So this snapshots, lets the machine run on, restores, checks the state came
back identical, and then makes the restored machine do real work.
"""

import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_client import Machine, CWD

SNAP = os.path.join(CWD, "booted.state")

# Regions sampled to tell one machine state from another: the application
# slot, some OS workspace, and the top of RAM.
PROBES = [0x8000, 0x1000, 0x4000, 0x100000, 0x200000]

failures = 0


def check(label, got, want):
    global failures
    ok = got == want
    if not ok:
        failures += 1
    print("%-40s %-18s %s" % (label, got, "ok" if ok else "EXPECTED %s" % (want,)))


def screen_hash(m):
    """Hash the display, once a frame newer than the current one has arrived.

    VIDC is output only, so nothing the guest computes depends on it - but
    the picture still has to come back, or a restored machine is a machine
    you cannot look at.
    """
    before = m.call("frames.info")["serial"]
    deadline = time.time() + 5
    while m.call("frames.info")["serial"] == before and time.time() < deadline:
        time.sleep(0.1)

    path = os.path.join(CWD, "snapcheck.png")
    m.call("screenshot", path=path)
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()[:16]


def fingerprint(m):
    """A hash of the registers and several regions of memory."""
    h = hashlib.sha256()
    r = m.call("regs.read")
    h.update(repr(r["r"]).encode())
    h.update(repr(r["cpsr"]).encode())
    for addr in PROBES:
        h.update(m.call("mem.read", addr=addr, len=1024)["hex"].encode())
    return h.hexdigest()[:16]


def main():
    if os.path.exists(SNAP):
        os.remove(SNAP)

    m = Machine("--seconds", "150")
    m.events.get(timeout=10)
    time.sleep(9)

    # Halt so the machine cannot move between the snapshot and the
    # fingerprint; a running machine would differ for honest reasons.
    m.call("halt")

    t0 = time.time()
    saved = m.call("snapshot.save", path=SNAP)
    save_time = time.time() - t0
    size = os.path.getsize(SNAP)
    print("saved %.1f MB in %.2f s at %d instructions"
          % (size / 1e6, save_time, saved["instructions"]))

    before = fingerprint(m)
    screen_before = screen_hash(m)
    pc_before = m.call("regs.read")["pc"]

    # Let the machine run on, so it is demonstrably somewhere else.
    m.call("continue")
    m.call("type", text="Help Modules\n")
    time.sleep(7)
    m.call("halt")
    check("the machine moved on", fingerprint(m) != before, True)
    check("  and so did the screen", screen_hash(m) != screen_before, True)

    t0 = time.time()
    m.call("snapshot.load", path=SNAP)
    load_time = time.time() - t0
    print("restored in %.3f s (a boot is about 9 s)" % load_time)

    after = fingerprint(m)
    check("state came back identical", after, before)
    check("  including the program counter",
          m.call("regs.read")["pc"], pc_before)
    check("  and the instruction count",
          m.call("status")["instructions"], saved["instructions"])

    # The framebuffer bytes are restored as memory and the VIDC registers as
    # a small struct, so the picture rebuilds itself from both. Nothing has
    # to be papered over with the previous frame.
    check("  and the display, pixel for pixel", screen_hash(m), screen_before)

    # A restored machine that cannot work is no use, however faithful.
    m.call("continue")
    base = m.call("status")["vdu_bytes"]
    m.call("type", text="Help Modules\\n")
    time.sleep(7)

    text = m.call("vdu.read", **{"from": base, "max": 4096})["text"]
    check("the restored machine still runs commands",
          "BBC BASIC" in text, True)
    check("  and its console output is captured",
          m.call("status")["last_command"], "Help Modules")

    # Restoring a second time from the same file must work too.
    m.call("halt")
    m.call("snapshot.load", path=SNAP)
    check("a second restore also works", fingerprint(m), before)

    m.call("continue")
    m.call("quit")
    m.proc.wait(timeout=20)

    print()
    print("FAILURES: %d" % failures if failures else "all checks passed")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
