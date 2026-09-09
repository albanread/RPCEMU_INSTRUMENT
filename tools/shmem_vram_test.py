import ctypes, ctypes.wintypes as w, json, struct, subprocess, time

EXE = r"F:\RISCOSDEV\rpcemu\src\build\rpcemu-headless.exe"
CWD = r"F:\RISCOSDEV\rpcemu\win32\RPCEmu"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenFileMappingW.restype = w.HANDLE
k32.OpenFileMappingW.argtypes = [w.DWORD, w.BOOL, w.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, ctypes.c_size_t]
FILE_MAP_READ = 0x0004

HDR   = "<6I Q QQQ QQQQ II"
VIDEO = "<4I 4I 4i 4i Q 256I 3I I"
SLOT  = "<II QQQQ iiiiii II iiii"

p = subprocess.Popen([EXE, "--rpc"], cwd=CWD, stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
nid=[0]
def call(m, params=None):
    nid[0]+=1
    r={"jsonrpc":"2.0","id":nid[0],"method":m}
    if params: r["params"]=params
    p.stdin.write(json.dumps(r)+"\n"); p.stdin.flush()
    while True:
        l=p.stdout.readline()
        if not l: return None
        try: m2=json.loads(l)
        except: continue
        if m2.get("id")==nid[0]: return m2
while True:
    l=p.stdout.readline()
    if not l: break
    try:
        if json.loads(l).get("method")=="event/ready": break
    except: pass
time.sleep(7)

info = call("shmem.info")["result"]
h = k32.OpenFileMappingW(FILE_MAP_READ, False, info["section"])
view = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0)
assert view, "map failed"
def rd(off, n): return ctypes.string_at(view + off, n)

hdr = struct.unpack(HDR, rd(0, struct.calcsize(HDR)))
slots_offset, slot_bytes = hdr[6], hdr[5]
vram_off, vram_bytes = hdr[12], hdr[13]
print("VRAM in section: offset %d, %d bytes" % (vram_off, vram_bytes))
assert vram_bytes > 0, "VRAM is not shared"

VIDEO_OFF = 96                       # after ram_bank_bytes
SLOTSZ = struct.calcsize(SLOT)
VIDSZ  = struct.calcsize(VIDEO)
assert SLOTSZ == info["slot_struct_bytes"], (
    "slot layout: we think %d bytes, the emulator says %d"
    % (SLOTSZ, info["slot_struct_bytes"]))
print("video block %d bytes, slots start at header+%d" % (VIDSZ, VIDEO_OFF + VIDSZ))
assert hdr[2] == VIDEO_OFF + VIDSZ + SLOTSZ*hdr[4], (
    "layout: header_bytes %d, computed %d" % (hdr[2], VIDEO_OFF + VIDSZ + SLOTSZ*hdr[4]))

# Halt so VRAM cannot change under us, then let a frame or two scan out.
call("halt"); time.sleep(0.4)

def read_video():
    v = struct.unpack(VIDEO, rd(VIDEO_OFF, VIDSZ))
    return v
def read_slot(i):
    return struct.unpack(SLOT, rd(VIDEO_OFF + VIDSZ + i*SLOTSZ, SLOTSZ))

idx = struct.unpack("<Q", rd(40, 8))[0]
slot = read_slot(idx)
seq, _r, serial, when_ns, instr, mode_serial, xs, ys, hxs, hys, dbl, bpp, blen, border, cx, cy, ch, _p = slot
frame = rd(slots_offset + idx*slot_bytes, blen)

v = read_video()
(vseq, bpp_code, bits, in_dram, fb_offset, fb_bytes, vxs, vys,
 vhxs, vhys, vdbl, vborder, vcx, vcy, vch, _pad) = v[:16]
palette = v[17:17+256]
print("video: bpp_code %d (%d bpp) %dx%d dram=%d fb_offset 0x%x fb_bytes %d serial %d" % (
    bpp_code, bits, vxs, vys, in_dram, fb_offset, fb_bytes, v[16]))
assert in_dram == 0, "framebuffer is in DRAM; this test covers the VRAM case"
assert (vseq & 1) == 0, "video block was mid-write"

vram = rd(vram_off + fb_offset, fb_bytes)

# Expand exactly as a shader would: unpack bits, index the palette.
mismatch = 0
if bits == 1:
    for y in range(vys):
        rowbase = y * (vxs // 8)
        for xb in range(vxs // 8):
            byte = vram[rowbase + xb]
            for b in range(8):
                want = palette[(byte >> b) & 1] & 0xffffff
                o = (y*vxs + xb*8 + b) * 4
                got = (frame[o] | (frame[o+1] << 8) | (frame[o+2] << 16))
                if want != got: mismatch += 1
    print("expanded %d pixels from %d bytes of VRAM, mismatches %d" % (
        vxs*vys, fb_bytes, mismatch))
    print("bandwidth: %d bytes packed vs %d bytes expanded (%.1fx less)" % (
        fb_bytes, blen, blen/fb_bytes))
else:
    print("mode is %d bpp; expansion check written for 1 bpp only" % bits)

call("continue"); call("quit"); p.wait(timeout=10)
print("OK" if mismatch == 0 else "MISMATCH")
