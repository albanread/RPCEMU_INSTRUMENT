"""Exercise the watchpoint, trace, stack and heap instruments.

Builds tools/testprog.s into a RISC OS Absolute image, runs it inside the
machine, and checks each instrument against what the program provably does:
it writes 1..5 to a fixed address in a loop, then calls a subroutine that
saves its return address and executes an undefined instruction.
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_client import Machine, CWD

HERE = os.path.dirname(os.path.abspath(__file__))
LLVM = r"C:\Program Files\LLVM\bin"

BASE = 0x8000
PROG_END = 0x8080
WATCHED = 0x9000            # the address the program writes to
GUEST_PATH = "HostFS::HostFS.$.crash"


def build():
    """Assemble and link the test program, leaving the ELF for symbols."""
    src = os.path.join(HERE, "testprog.s")
    obj = os.path.join(CWD, "testprog.o")
    elf = os.path.join(CWD, "testprog.elf")
    img = os.path.join(CWD, "hostfs", "crash,ff8")

    def run(*args):
        subprocess.run(args, check=True, capture_output=True)

    run(os.path.join(LLVM, "clang.exe"), "--target=arm-none-eabi",
        "-mcpu=strongarm110", "-c", src, "-o", obj)
    run(os.path.join(LLVM, "ld.lld.exe"), "--image-base", "0x8000",
        "--section-start", ".text=0x8000", "-o", elf, obj)
    run(os.path.join(LLVM, "llvm-objcopy.exe"), "-O", "binary", elf, img)

    return elf


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


failures = 0


def check(label, got, want):
    global failures
    ok = got == want
    if not ok:
        failures += 1
    print("%-38s %-20s %s" % (label, got, "ok" if ok else "EXPECTED %s" % (want,)))


def main():
    elf = build()
    print("built", elf)

    m = Machine("--seconds", "120")
    m.events.get(timeout=10)
    time.sleep(9)

    print("symbols:", m.call("sym.load", path=elf))

    # Take the addresses from the symbol table rather than hardcoding them,
    # so editing the test program cannot silently invalidate the checks.
    loop = m.call("sym.lookup", name="counting_loop")["addr"]
    store_pc = loop + 4                 # the str is the second instruction
    crashing = m.call("sym.lookup", name="crashing")["addr"]
    caller_return = crashing - 4        # the instruction after the bl

    # Trace only the program. Unfiltered, the ring would be entirely OS.
    print("trace:", m.call("trace.start", entries=4096,
                           **{"from": BASE, "to": PROG_END}))
    # Catch every kind: a data abort here would mean the program is wrong,
    # and silently missing it would waste the run.
    m.call("catch.set", undefined=True, data_abort=True, prefetch_abort=True,
           **{"from": BASE, "to": PROG_END})
    print("watch:", m.call("wp.set", addr=WATCHED, len=4, on="w"))

    m.call("type", text="Run %s\\n" % GUEST_PATH)

    # --- watchpoint ---------------------------------------------------
    ev = wait_event(m, "event/stopped", 30)
    if ev is None:
        print("FAILED: the watchpoint never fired")
        return 1

    check("stopped on the watchpoint", ev["params"]["reason"], "watchpoint")

    hit = m.call("wp.last")
    check("  the store that did it", hit["pc"], store_pc)
    check("  named by symbol", hit.get("sym"), "counting_loop")
    check("  address written", hit["addr"], WATCHED)
    check("  it was a write", hit["write"], True)
    check("  value written", hit["new"], 1)

    # Let the rest of the loop run without stopping five more times.
    m.call("wp.clear", id=0)
    m.call("continue")

    # --- fault, trace and stack ---------------------------------------
    ev = wait_event(m, "event/stopped", 30)
    if ev is None:
        print("FAILED: the fault was not caught")
        return 1

    check("stopped on the fault", ev["params"]["reason"], "fault")
    check("  it was the undefined instruction",
          m.call("fault.info")["kind"], "undefined_instruction")
    check("  inside the subroutine", ev["params"].get("sym"), "crashing")

    # Taking the exception switched the CPU to undefined mode, which banks
    # r13: the live stack pointer is no longer the program's. The stack to
    # look at is the one the faulting instruction had.
    fault = m.call("fault.info")
    sp = fault["r"][13]

    tr = m.call("trace.read", max=200)
    pcs = [e["pc"] for e in tr["entries"]]
    check("  trace recorded the whole run", tr["recorded"], len(pcs))
    check("  trace starts at the entry point", pcs[0], BASE)
    check("  the loop body repeats 5 times", pcs.count(store_pc), 5)
    check("  last instruction is the fault", pcs[-1], ev["params"]["pc"])

    syms = [e.get("sym") for e in tr["entries"]]
    check("  trace is symbolised", syms[0], "_start")

    bt = m.call("stack.backtrace", sp=sp, depth=64)
    check("  backtrace finds a frame", len(bt["frames"]) > 0, True)
    if bt["frames"]:
        f0 = bt["frames"][0]
        print("%-38s &%08X in %s+%d" % ("  top frame", f0["addr"],
                                        f0["sym"], f0["sym_offset"]))
        check("  it is the return address into the caller",
              f0["addr"], caller_return)

    st = m.call("stack.read", sp=sp, words=4)
    check("  stack top is that return address",
          st["words"][0]["value"], caller_return)

    # --- heap ----------------------------------------------------------
    h = m.call("heap.stats")
    print("heap: %d OS_Heap calls, %d allocations, %d bytes asked for, "
          "peak %d live blocks" % (h["calls"], h["allocations"],
                                   h["bytes_requested"], h["peak_live_blocks"]))
    check("  the OS used a heap", h["calls"] > 0, True)

    if h["last_heap"]:
        d = m.call("heap.describe")
        print("heap descriptor at &%08X: valid=%s magic=&%08X"
              % (d["addr"], d["valid"], d["magic"]))
        if d["valid"]:
            print("   header words: free=&%X base=&%X end=&%X"
                  % (d["free_offset"], d["base_offset"], d["end_offset"]))

    m.call("continue")
    m.call("quit")
    m.proc.wait(timeout=20)

    print()
    print("FAILURES: %d" % failures if failures else "all checks passed")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
