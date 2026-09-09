"""Check that a run from a snapshot is reproducible.

A failing test is only worth having if it fails the same way twice. Without
the virtual clock the IOMD and video interrupts land wherever the host
happened to be, so two runs from the same state diverge; with it they land at
fixed instruction counts and the runs are identical.

The test does the same thing both ways round and shows the difference,
because a determinism claim nobody has watched fail is not worth much.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_client import Machine, CWD

SNAP = os.path.join(CWD, "determinism.state")
INSTRUCTIONS = 400000

failures = 0


def check(label, got, want):
    global failures
    ok = got == want
    if not ok:
        failures += 1
    print("%-44s %-14s %s" % (label, got, "ok" if ok else "EXPECTED %s" % (want,)))


def run_from_snapshot(m):
    """Restore, run an exact number of instructions, return the trace.

    Exact matters: stopping on a polled "have we done enough yet" would end
    the two runs at slightly different points, and the tails would differ for
    reasons that have nothing to do with the clock.
    """
    m.call("snapshot.load", path=SNAP)

    # Trace everything. The point is to catch interrupts landing in different
    # places, and those are in the operating system.
    m.call("trace.start", entries=8192, **{"from": 0, "to": 0xFFFFFFFF})
    m.call("step", count=INSTRUCTIONS)

    deadline = time.time() + 120
    while time.time() < deadline:
        if m.call("status")["state"] == "stopped":
            break
        time.sleep(0.05)

    m.call("trace.stop")

    tr = m.call("trace.read", max=2048)
    return (tr["recorded"], [(e["pc"], e["opcode"]) for e in tr["entries"]])


def main():
    if os.path.exists(SNAP):
        os.remove(SNAP)

    m = Machine("--seconds", "180")
    m.events.get(timeout=10)
    time.sleep(9)

    m.call("halt")
    m.call("snapshot.save", path=SNAP)
    print("snapshot taken\n")

    # --- host clock: runs are expected to diverge -----------------------
    m.call("clock.set", virtual=False)
    a = run_from_snapshot(m)
    b = run_from_snapshot(m)
    same_host = (a == b)
    print("host clock      : two runs identical = %s  (%d instructions each)"
          % (same_host, a[0]))

    # --- virtual clock: runs must be identical --------------------------
    m.call("clock.set", virtual=True)
    print("virtual clock   :", m.call("clock.set", virtual=True))

    c = run_from_snapshot(m)
    d = run_from_snapshot(m)
    print("virtual clock   : %d instructions each" % c[0])
    print()
    check("both runs retired the same count", c[0], d[0])
    check("two runs from one snapshot are identical", c[1] == d[1], True)
    check("  over a window with timer interrupts in it",
          c[0] >= INSTRUCTIONS, True)

    if c[1] != d[1]:
        for i, (x, y) in enumerate(zip(c[1], d[1])):
            if x != y:
                print("   first divergence at entry %d: &%08X vs &%08X"
                      % (i, x[0], y[0]))
                break

    # Worth knowing whether the host clock was genuinely the problem, rather
    # than the runs being trivially identical either way.
    print()
    if same_host:
        print("note: the host-clock runs also matched this time. That can")
        print("      happen on a quiet machine; it does not mean the host")
        print("      clock is reproducible.")
    else:
        print("note: the host-clock runs diverged, as expected - which is")
        print("      what the virtual clock is for.")

    m.call("quit")
    m.proc.wait(timeout=20)

    print()
    print("FAILURES: %d" % failures if failures else "all checks passed")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
