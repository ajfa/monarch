#!/usr/bin/env python3
"""Replace the contents of an existing file inside a CP/M IBM-3740 8" SSSD image,
in place, reusing its allocated blocks. New data must fit the existing allocation.

usage: cpmput.py <image.dsk> <NAME.EXT> <hostfile>
"""
import sys, os
SKEW = [1,7,13,19,25,5,11,17,23,3,9,15,21,2,8,14,20,26,6,12,18,24,4,10,16,22]
RES, SPT = 2, 26

def secoff(n):
    track = RES + n // SPT
    phys  = SKEW[n % SPT] - 1
    return (track*SPT + phys)*128

def main(img_path, cpmname, host):
    img = bytearray(open(img_path,'rb').read())
    data = open(host,'rb').read()
    name, _, ext = cpmname.upper().partition('.')
    name = name.ljust(8); ext = ext.ljust(3)
    # collect extents
    exts = []
    for i in range(64):
        o = secoff(i//4) + (i%4)*32
        rec = img[o:o+32]
        if rec[0] == 0xE5: continue
        n = "".join(chr(b&0x7f) for b in rec[1:9])
        e = "".join(chr(b&0x7f) for b in rec[9:12])
        if n == name and e == ext:
            exts.append((rec[12], o))
    if not exts:
        sys.exit("not found: %s" % cpmname)
    exts.sort()
    # total capacity
    blocks = []
    for ex, o in exts:
        blocks += [b for b in img[o+16:o+32] if b]
    cap = len(blocks)*1024
    if len(data) > cap:
        sys.exit("new data %d B exceeds allocated %d B (%d blocks)" % (len(data), cap, len(blocks)))
    # pad to 128-byte records, fill rest with 0x1A
    recs = (len(data) + 127) // 128
    data = data + b'\x1a' * (recs*128 - len(data))
    # write blocks
    for bi, b in enumerate(blocks):
        chunk = data[bi*1024:(bi+1)*1024]
        if not chunk: break
        chunk = chunk + b'\x1a' * (1024 - len(chunk))
        for s in range(8):
            o = secoff(b*8 + s)
            img[o:o+128] = chunk[s*128:(s+1)*128]
    # fix record counts per extent (128 records max per extent)
    left = recs
    for ex, o in exts:
        nb = len([x for x in img[o+16:o+32] if x])
        rc = min(left, 128)
        img[o+15] = rc
        left -= rc
    open(img_path,'wb').write(bytes(img))
    print("wrote %d bytes (%d records, %d blocks) into %s:%s" % (len(data), recs, len(blocks), os.path.basename(img_path), cpmname))

main(*sys.argv[1:4])
