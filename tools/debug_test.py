"""End-to-end check of the debugger, against real code running in RISC OS.

This is the workflow for testing a code generator: put an image at a known
base, break on entry, look at memory and registers, then let it run into
whatever it does wrong and get back exactly where it died with the register
file as the faulting instruction left it.

Writes its own test program, so it needs nothing prepared.
"""

import struct
import sys
import time
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_client import Machine, CWD

BASE = 0x8000
GUEST_PATH = "HostFS::HostFS.$.crash"

# A minimal RISC OS Absolute image (&FF8): loaded and entered at &8000.
#   mov r0, #1        so the registers at the fault are recognisable
#   mov r1, #0x2a
#   udf               permanently undefined; the interpreter decodes it as such
PROGRAM = [0xE3A00001, 0xE3A0102A, 0xE7F000F0]
FAULT_AT = BASE + 8


def write_program():
    path = os.path.join(CWD, "hostfs", "crash,ff8")
    with open(path, "wb") as f:
        f.write(b"".join(struct.pack("<I", w) for w in PROGRAM))
    return path


def wait_event(m, name, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            ev = m.events.get(timeout=max(0.05, end - time.time()))
        except Exception:
            break
        if ev.get("method") == name:
            return ev
    return None


def check(label, got, want):
    ok = got == want
    print("%-34s %-22s %s" % (label, got, "ok" if ok else "EXPECTED %s" % (want,)))
    return ok


def main():
    print("test program at", write_program())

    m = Machine("--seconds", "90")
    m.events.get(timeout=10)
    time.sleep(9)                       # reach the supervisor prompt

    failures = 0

    # Bound the fault trap to the program. RISC OS takes aborts and executes
    # undefined instructions as a matter of course, so an unbounded trap would
    # stop the machine during boot.
    m.call("catch.set", undefined=True, data_abort=True, prefetch_abort=True,
           **{"from": BASE, "to": BASE + 0x1000})
    m.call("bp.set", addr=BASE)

    m.call("type", text="Run %s\\n" % GUEST_PATH)

    ev = wait_event(m, "event/stopped", 25)
    if ev is None:
        print("FAILED: the program never reached the breakpoint")
        return 1

    failures += not check("stopped on entry", ev["params"]["reason"], "breakpoint")
    failures += not check("  at the load address", ev["params"]["pc"], BASE)
    failures += not check("  CPU is stopped", m.call("status")["state"], "stopped")

    # The image really is in memory where it was meant to be.
    want_hex = "".join("%08x" % int.from_bytes(struct.pack("<I", w), "big")
                       for w in PROGRAM)
    failures += not check("  image loaded intact",
                          m.call("mem.read", addr=BASE, len=12)["hex"], want_hex)

    # A single step moves exactly one instruction.
    before = m.call("status")["instructions"]
    m.call("step", count=1)
    wait_event(m, "event/stopped", 5)
    failures += not check("  step of 1 retires 1",
                          m.call("status")["instructions"] - before, 1)

    m.call("bp.clear", id=0)
    m.call("continue")

    ev = wait_event(m, "event/stopped", 25)
    if ev is None:
        print("FAILED: the fault was not caught")
        return 1

    failures += not check("stopped on the fault", ev["params"]["reason"], "fault")

    f = m.call("fault.info")
    failures += not check("  kind", f["kind"], "undefined_instruction")
    failures += not check("  faulting instruction", f["pc"], FAULT_AT)
    failures += not check("  r0 as the code left it", f["r"][0], 1)
    failures += not check("  r1 as the code left it", f["r"][1], 0x2A)
    failures += not check("  faulted in user mode", f["mode"], 16)

    m.call("continue")
    m.call("quit")
    m.proc.wait(timeout=20)

    print()
    print("FAILURES: %d" % failures if failures else "all checks passed")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
