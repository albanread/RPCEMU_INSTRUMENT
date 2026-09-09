"""Can the guest see the pointer at all?

BASIC's MOUSE statement is OS_Mouse with a friendly face, so this isolates
the read path from anything the desktop does with it. If the guest reports
the position we set, input works and the desktop is a separate question.
"""
import io
import json
import subprocess
import time

EXE = r"F:\RISCOSDEV\rpcemu\src\build\rpcemu-headless.exe"
CWD = r"F:\RISCOSDEV\rpcemu\win32\RPCEmu"

p = subprocess.Popen([EXE, "--rpc"], cwd=CWD, stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                     text=True, bufsize=1)
n = [0]


def call(method, params=None):
    n[0] += 1
    req = {"jsonrpc": "2.0", "id": n[0], "method": method}
    if params:
        req["params"] = params
    p.stdin.write(json.dumps(req) + "\n")
    p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line:
            return None
        try:
            msg = json.loads(line)
        except Exception:
            continue
        if msg.get("id") == n[0]:
            if "error" in msg:
                print("  !!", method, msg["error"])
                return None
            return msg.get("result")


while True:
    line = p.stdout.readline()
    if not line:
        break
    try:
        if json.loads(line).get("method") == "event/ready":
            break
    except Exception:
        pass

time.sleep(9)


def typed(text, settle=1.5):
    call("type", {"text": text})
    for _ in range(80):
        time.sleep(0.25)
        if not call("status")["typing"]:
            break
    time.sleep(settle)


typed("BASIC\n")

for x, y in ((100, 50), (400, 200), (600, 30)):
    call("mouse.move", {"x": x, "y": y})
    time.sleep(0.4)

    mark = call("status")["vdu_bytes"]
    typed('MOUSE a,b,c:PRINT "AT ";a;" ";b;" ";c\n')
    said = call("vdu.read", {"from": mark, "max": 1500})["text"].replace("\r", "\n")

    got = [s for s in said.split("\n") if s.strip().startswith("AT ")]
    ours = call("mouse.status")
    print("set (%3d,%3d)  core says (%3d,%3d)  guest says %s"
          % (x, y, ours["x"], ours["y"], got[0].strip() if got else "(nothing)"))

# And with a button held.
call("mouse.down", {"button": "select"})
time.sleep(0.3)
mark = call("status")["vdu_bytes"]
typed('MOUSE a,b,c:PRINT "BUTTONS ";c\n')
said = call("vdu.read", {"from": mark, "max": 1500})["text"].replace("\r", "\n")
got = [s for s in said.split("\n") if s.strip().startswith("BUTTONS")]
print("select held    guest says %s" % (got[0].strip() if got else "(nothing)"))
call("mouse.up", {"button": "select"})

call("quit")
p.wait(timeout=10)
