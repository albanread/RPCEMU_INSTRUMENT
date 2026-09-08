"""Drive rpcemu-headless over its stdio JSON-RPC channel.

This is the shape an agent uses: spawn the emulator as a subprocess, talk
JSON over the pipes, and never touch a window.
"""

import json
import os
import subprocess
import sys
import threading
import time
import queue

_HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(_HERE, os.pardir, "build", "rpcemu-headless.exe")
CWD = r"F:\RISCOSDEV\rpcemu\win32\RPCEmu"


class Machine:
    def __init__(self, *extra):
        self.proc = subprocess.Popen(
            [EXE, "--rpc", *extra],
            cwd=CWD,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
        self.replies = {}
        self.events = queue.Queue()
        self.next_id = 1
        self.lock = threading.Lock()
        self.cv = threading.Condition(self.lock)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                print("!! unparseable:", line[:200])
                continue
            if "id" in msg:
                with self.cv:
                    self.replies[msg["id"]] = msg
                    self.cv.notify_all()
            else:
                self.events.put(msg)

    def call(self, method, timeout=20.0, **params):
        with self.lock:
            rid = self.next_id
            self.next_id += 1
        req = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params:
            req["params"] = params
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()

        deadline = time.time() + timeout
        with self.cv:
            while rid not in self.replies:
                if not self.cv.wait(timeout=max(0.05, deadline - time.time())):
                    if time.time() > deadline:
                        raise TimeoutError(method)
            msg = self.replies.pop(rid)
        if "error" in msg:
            raise RuntimeError(f"{method}: {msg['error']}")
        return msg["result"]

    def drain_events(self):
        out = []
        while True:
            try:
                out.append(self.events.get_nowait())
            except queue.Empty:
                return out


def main():
    m = Machine("--seconds", "90")

    ev = m.events.get(timeout=10)
    print("ready:", ev["method"], ev["params"])

    d = m.call("describe")
    print(f"describe: {len(d['methods'])} methods, {len(d['events'])} events")

    print("status at boot:", m.call("status"))

    # Let the machine reach the supervisor prompt.
    time.sleep(9)
    print("status after boot:", m.call("status"))

    print("type ->", m.call("type", text="Help Modules\\n"))
    time.sleep(6)

    st = m.call("status")
    print("after typing:", {k: st[k] for k in ("state", "vdu_bytes", "last_command")})

    vdu = m.call("vdu.read", **{"from": 0, "max": 4096})
    tail = vdu["text"].replace("\r", "")
    print("--- console tail ---")
    print("\n".join(tail.splitlines()[-6:]))
    print("--- end ---")

    # Halt the CPU and look around.
    print("halt ->", m.call("halt")["state"])
    r = m.call("regs.read")
    print("PC=&%08X  r0=&%08X  cpsr=&%08X mode=%d"
          % (r["pc"], r["r"][0], r["cpsr"], r["mode"]))

    mem = m.call("mem.read", addr=r["pc"], len=16)
    print("code at PC:", mem["hex"])

    # Stepping must move the PC and nothing else should be running.
    before = m.call("status")["instructions"]
    m.call("step", count=5)
    time.sleep(0.4)
    evs = [e for e in m.drain_events() if e["method"] == "event/stopped"]
    after = m.call("status")
    print("step 5: events=%s  instructions %d -> %d  state=%s"
          % ([e["params"]["reason"] for e in evs], before,
             after["instructions"], after["state"]))

    # Video history survives the halt, because VIDC is not the CPU.
    print("frames:", m.call("frames.info"))
    print("frames.save ->", m.call("frames.save", prefix="seq"))

    m.call("continue")
    print("resumed:", m.call("status")["state"])

    m.call("quit")
    m.proc.wait(timeout=15)
    print("exit code:", m.proc.returncode)


if __name__ == "__main__":
    sys.exit(main())
