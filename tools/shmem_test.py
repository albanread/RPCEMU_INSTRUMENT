import ctypes, ctypes.wintypes as w, json, struct, subprocess, time, zlib, base64, sys

EXE = r"F:\RISCOSDEV\rpcemu\src\build\rpcemu-headless.exe"
CWD = r"F:\RISCOSDEV\rpcemu\win32\RPCEmu"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenFileMappingW.restype = w.HANDLE
k32.OpenFileMappingW.argtypes = [w.DWORD, w.BOOL, w.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, ctypes.c_size_t]
k32.OpenEventW.restype = w.HANDLE
k32.OpenEventW.argtypes = [w.DWORD, w.BOOL, w.LPCWSTR]
k32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
k32.ResetEvent.argtypes = [w.HANDLE]
FILE_MAP_READ = 0x0004
SYNCHRONIZE = 0x00100000
EVENT_MODIFY_STATE = 0x0002

HDR = "<6I Q QQQ QQQQ II"          # through ram_bank_bytes
SLOT = "<II QQQQ iiiiii II iiii"

p = subprocess.Popen([EXE, "--rpc"], cwd=CWD, stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
nid = [0]
def call(m, params=None):
    nid[0] += 1
    r = {"jsonrpc":"2.0","id":nid[0],"method":m}
    if params: r["params"] = params
    p.stdin.write(json.dumps(r)+"\n"); p.stdin.flush()
    while True:
        l = p.stdout.readline()
        if not l: return None
        try: m2 = json.loads(l)
        except: continue
        if m2.get("id") == nid[0]: return m2

while True:
    l = p.stdout.readline()
    if not l: break
    try:
        if json.loads(l).get("method") == "event/ready": break
    except: pass
time.sleep(6)

info = call("shmem.info")["result"]
print("shmem.info:", json.dumps(info))
assert info["available"], "section not available"
assert info["section"].startswith("Local" + chr(92)), (
    "name lost its namespace prefix: %r" % info["section"])

h = k32.OpenFileMappingW(FILE_MAP_READ, False, info["section"])
assert h, "OpenFileMapping failed %d" % ctypes.get_last_error()
view = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0)
assert view, "MapViewOfFile failed %d" % ctypes.get_last_error()
ev = k32.OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, False, info["event"])
assert ev, "OpenEvent failed %d" % ctypes.get_last_error()

def rd(off, n):
    return ctypes.string_at(view + off, n)

magic, version, header_bytes, pid, slot_count, slot_bytes, slots_offset, \
    newest, newest_index, writes, ram_off, ram_bytes, vram_off, vram_bytes, \
    ram_banks, ram_bank_bytes = struct.unpack(HDR, rd(0, struct.calcsize(HDR)))
print("magic %r version %d header_bytes %d slots %d x %d bytes at %d" % (
    struct.pack("<I", magic), version, header_bytes, slot_count, slot_bytes, slots_offset))
assert struct.pack("<I", magic) == b"RPCS"
SLOTSZ = struct.calcsize(SLOT)
print("slot struct %d bytes; header says slots array = %d" % (SLOTSZ, header_bytes - 96))
assert (header_bytes - 96) == SLOTSZ * slot_count, "layout mismatch"

def read_slot(i):
    return struct.unpack(SLOT, rd(96 + i*SLOTSZ, SLOTSZ))

def grab():
    """Seqlock read of the newest frame."""
    for _ in range(8):
        idx = struct.unpack("<Q", rd(40, 8))[0]
        s1 = read_slot(idx)
        if s1[0] & 1: continue
        n = s1[12]                      # byte_length
        px = rd(slots_offset + idx*slot_bytes, n)
        s2 = read_slot(idx)
        if s1[0] == s2[0]:
            return s1, px
    raise RuntimeError("could not get a stable frame")

slot, px = grab()
(seq, _res, serial, when_ns, instr, mode_serial,
 xs, ys, hxs, hys, dbl, bpp, blen, border, cx, cy, ch, _pad) = slot
print("frame serial %d  %dx%d host %dx%d doublesize %d bpp %d  %d bytes" % (
    serial, xs, ys, hxs, hys, dbl, bpp, blen))
assert blen == xs*ys*4, "byte_length %d != %d" % (blen, xs*ys*4)

# Compare against what the pipe would deliver for the same frame.
r = call("frames.data", {"age": 0})["result"]
assert r["serial"] == serial, "serials differ: shm %d rpc %d" % (serial, r["serial"])
png = base64.b64decode(r["data"])
pos, idat = 8, b""
while pos < len(png):
    ln = struct.unpack(">I", png[pos:pos+4])[0]
    typ = png[pos+4:pos+8]
    if typ == b"IDAT": idat += png[pos+8:pos+8+ln]
    pos += 12 + ln
raw = zlib.decompress(idat)
rowb = xs*3 + 1
mismatch = 0
for y in range(ys):
    row = raw[y*rowb+1 : y*rowb+1+xs*3]
    for x in range(xs):
        b, g, rr = px[(y*xs+x)*4], px[(y*xs+x)*4+1], px[(y*xs+x)*4+2]
        if (row[x*3], row[x*3+1], row[x*3+2]) != (rr, g, b):
            mismatch += 1
print("pixels compared %d, mismatches %d" % (xs*ys, mismatch))

# Timing: blocking wait for the next frame vs a frames.data round trip.
k32.ResetEvent(ev)
t = time.perf_counter(); k32.WaitForSingleObject(ev, 2000)
_, px2 = grab(); shm_ms = (time.perf_counter()-t)*1000
t = time.perf_counter(); call("frames.data", {"age":0}); rpc_ms = (time.perf_counter()-t)*1000
print("next frame via shared memory: %.2f ms (includes waiting for it)" % shm_ms)
print("one frame via frames.data:    %.2f ms" % rpc_ms)

t = time.perf_counter()
for _ in range(50): grab()
print("50 shared-memory grabs: %.2f ms total (%.3f ms each)" % (
    (time.perf_counter()-t)*1000, (time.perf_counter()-t)*1000/50))

call("quit"); p.wait(timeout=10)
print("OK" if mismatch == 0 else "PIXEL MISMATCH")
