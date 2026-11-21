#!/usr/bin/env python3
# Build a 64 MiB SD card image for QEMU: MBR + one FAT32 partition at LBA 2048,
# optionally with files in it (8.3 lowercase names, one directory level).
# usage: tools/mksdimg.py out.img [fs/path=host/file ...]
#   e.g. tools/mksdimg.py sd.img bin/busybox=/path/to/busybox.static
import struct, sys

SIZE, START, SPC, RES, NFATS = 64 << 20, 2048, 8, 32, 2
BPS, CL = 512, 512 * 8
part = SIZE // BPS - START
fatsz = 1
while True:                                         # smallest FAT that covers every cluster
    clusters = (part - RES - NFATS * fatsz) // SPC
    need = ((clusters + 2) * 4 + BPS - 1) // BPS
    if need <= fatsz:
        break
    fatsz = need
data0 = RES + NFATS * fatsz                         # first data sector (partition-relative)
fat = [0x0FFFFFF8, 0x0FFFFFFF] + [0] * clusters
clus = {}                                           # cluster -> bytes
nxt = [2]

def alloc(data):
    n = max(1, (len(data) + CL - 1) // CL)
    first = nxt[0]
    for i in range(n):
        c = first + i
        fat[c] = c + 1 if i < n - 1 else 0x0FFFFFFF
        clus[c] = data[i * CL:(i + 1) * CL]
    nxt[0] += n
    return first

def name83(name):
    base, _, ext = name.partition(".")
    if not (0 < len(base) <= 8 and len(ext) <= 3) or name != name.lower():
        sys.exit("mksdimg: %r is not an 8.3 lowercase name" % name)
    nt = 0x08 | (0x10 if ext else 0)                # NT flags: lowercase base / extension
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode(), nt

def entry(name, attr, cluster, size):
    raw, nt = name83(name) if name not in (".", "..") else (name.ljust(11).encode(), 0)
    return struct.pack("<11sBBBHHHHHHHI", raw, attr, nt, 0, 0, 0x21, 0x21, cluster >> 16, 0, 0x21,
                       cluster & 0xFFFF, size)

tree = {}                                           # dir name ("" = root) -> [(name, data)]
for arg in sys.argv[2:]:
    path, _, host = arg.partition("=")
    d, _, f = path.rpartition("/")
    tree.setdefault(d, []).append((f, open(host, "rb").read()))
    if d:
        tree.setdefault("", [])

root_entries = []
for d, files in sorted(tree.items()):
    if not d:
        continue
    ents = [(f, alloc(data), len(data)) for f, data in files]
    dc = nxt[0]                                     # directory gets the next cluster
    body = entry(".", 0x10, dc, 0) + entry("..", 0x10, 0, 0) + b"".join(entry(f, 0x20, c, n) for f, c, n in ents)
    assert alloc(body) == dc
    root_entries.append(entry(d, 0x10, dc, 0))
for f, data in tree.get("", []):
    root_entries.append(entry(f, 0x20, alloc(data), len(data)))
root = b"".join(root_entries)
root_first = nxt[0]
alloc(root)

img = bytearray(SIZE)
mbr = bytearray(512)
mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\0\0\0", 0x0C, b"\0\0\0", START, part)
mbr[510:512] = b"\x55\xaa"
img[0:512] = mbr
bs = bytearray(512)
bs[0:3] = b"\xeb\x58\x90"
bs[3:11] = b"PIOS    "
struct.pack_into("<HBHBHHBHHHII", bs, 11, BPS, SPC, RES, NFATS, 0, 0, 0xF8, 0, 32, 64, START, part)
struct.pack_into("<IHHIHH", bs, 36, fatsz, 0, 0, root_first, 1, 6)
struct.pack_into("<BBBI11s8s", bs, 64, 0x80, 0, 0x29, 0x1234ABCD, b"PIOS       ", b"FAT32   ")
bs[510:512] = b"\x55\xaa"
fsinfo = bytearray(512)
struct.pack_into("<I", fsinfo, 0, 0x41615252)
struct.pack_into("<III", fsinfo, 484, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF)
struct.pack_into("<I", fsinfo, 508, 0xAA550000)
p = START * BPS
for off, blk in ((0, bs), (1, fsinfo), (6, bs), (7, fsinfo)):
    img[p + off * BPS:p + (off + 1) * BPS] = blk
fatbytes = struct.pack("<%dI" % len(fat), *fat)
for i in range(NFATS):
    o = p + (RES + i * fatsz) * BPS
    img[o:o + len(fatbytes)] = fatbytes
for c, data in clus.items():
    o = p + (data0 + (c - 2) * SPC) * BPS
    img[o:o + len(data)] = data
open(sys.argv[1], "wb").write(img)
