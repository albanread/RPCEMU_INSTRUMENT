"""Drive the RISC OS desktop with the synthetic pointer.

Everything else in this directory tests a machine at a command prompt. The
desktop is the other half of the system and it answers to a pointer, so this
starts it and clicks things, checking after each click that the screen
actually changed. A click that changes nothing is indistinguishable from a
click that never arrived, which is how a broken pointer went unnoticed.

Two coordinate traps are worth knowing, because both cost time:

  - The pointer speaks the pixels a frame is published in. In a mode that
    doubles height the core's own pointer is in doubled pixels, so a caller
    working from the picture could once only reach the top half of the
    screen - the half the icon bar is not in.

  - *Configure WimpMode wants a mode NUMBER on this ROM. Given a mode string
    it says "Numeric parameter needed" quietly enough that the desktop comes
    up in mode 0 and you wonder why.
"""
import base64
import hashlib
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_client import Machine                      # noqa: E402


def picture(m):
    """A hash of the screen, for telling whether anything happened."""
    r = m.call("frames.data", age=0)
    if not r or "data" not in r:
        return None
    return hashlib.sha1(base64.b64decode(r["data"])).hexdigest()[:12]


def settle_typing(m):
    for _ in range(80):
        time.sleep(0.25)
        if not m.call("status")["typing"]:
            return
    return


def typed(m, text, settle=2.0):
    m.call("type", text=text)
    settle_typing(m)
    time.sleep(settle)


def click(m, x, y, button, what, checks):
    before = picture(m)

    m.call("mouse.click", x=x, y=y, button=button)
    for _ in range(40):
        time.sleep(0.25)
        if not m.call("mouse.status")["busy"]:
            break
    time.sleep(2.5)

    after = picture(m)
    changed = before != after

    print("  %-34s %-14s %s" % (what, "at %d,%d" % (x, y),
                                "ok" if changed else "NOTHING HAPPENED"))
    checks.append(changed)

    return changed


def main():
    checks = []

    m = Machine()
    m.events.get(timeout=15)
    time.sleep(9)

    typed(m, "Desktop\n", settle=16)

    f = m.call("frames.list")["frames"][-1]
    width, height, bpp = f["width"], f["height"], f["bpp"]
    print("desktop up: %dx%d %dbpp, doublesize %d"
          % (width, height, bpp, f["doublesize"]))

    # Where the pointer is asked to go is where the guest should think it is.
    # Everything below rests on this.
    target = (width // 3, height // 3)
    m.call("mouse.move", x=target[0], y=target[1])
    time.sleep(0.5)
    st = m.call("mouse.status")
    ok = (st["x"], st["y"]) == target
    print("  %-34s %-14s %s"
          % ("pointer reads back where put", "at %d,%d" % target,
             "ok" if ok else "REPORTS %d,%d" % (st["x"], st["y"])))
    checks.append(ok)

    # Order matters. Menu over the bare pinboard opens a menu and a click
    # away closes it again, both without knowing where anything is. Do those
    # while the desktop is still clear, because the icon bar raises a modal
    # error box on a machine with no !Boot - and a modal box ignores Menu,
    # which looks exactly like a pointer that does not work.
    click(m, width // 2, height // 3, "menu", "Menu over the pinboard", checks)
    click(m, width - 30, 20, "select", "Select away, to dismiss", checks)

    # The icon bar runs along the bottom whatever the mode, and HostFS is the
    # leftmost icon on it.
    click(m, 28, height - 14, "select", "Select on the HostFS icon", checks)

    m.call("screenshot", path=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "wimp_test.png"))

    st = m.call("mouse.status")
    print("\n  %d moves and %d clicks delivered; the desktop asked about the "
          "mouse %d times" % (st["moves"], st["clicks"], st["asked_osmouse"]))

    try:
        m.call("quit", timeout=5)
    except Exception:
        pass
    try:
        m.proc.wait(timeout=10)
    except Exception:
        m.proc.kill()

    print()
    if all(checks):
        print("all checks passed")
        return 0

    print("FAILED: %d of %d checks" % (checks.count(False), len(checks)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
