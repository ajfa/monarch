import sys, glob, os
SKEW = [1,7,13,19,25,5,11,17,23,3,9,15,21,2,8,14,20,26,6,12,18,24,4,10,16,22]
RES, SPT, BLS = 2, 26, 1024   # reserved tracks, sectors/track, block size

def logsec(img, n):           # n = logical sector index over the whole data area
    track = RES + n // SPT
    phys  = SKEW[n % SPT] - 1
    o = (track*SPT + phys)*128
    return img[o:o+128]

def block(img, b):            # 1K block = 8 logical sectors
    return b"".join(logsec(img, b*8 + i) for i in range(8))

def extract(path, outdir):
    img = open(path,'rb').read()
    os.makedirs(outdir, exist_ok=True)
    files = {}
    for i in range(64):
        rec = logsec(img, i//4)[(i%4)*32:(i%4)*32+32]
        if rec[0] == 0xE5: continue
        name = "".join(chr(b&0x7f) for b in rec[1:9]).rstrip()
        ext  = "".join(chr(b&0x7f) for b in rec[9:12]).rstrip()
        ex, rc = rec[12], rec[15]
        blocks = [b for b in rec[16:32] if b]     # 8-bit block numbers (small disk)
        files.setdefault((name,ext), {})[ex] = (rc, blocks)
    for (n,e), exts in files.items():
        data = b""
        for ex in sorted(exts):
            rc, blocks = exts[ex]
            d = b"".join(block(img,b) for b in blocks)
            data += d[:rc*128]
        fn = os.path.join(outdir, "%s.%s" % (n,e))
        open(fn,'wb').write(data)
    print("%s: %d files -> %s" % (os.path.basename(path), len(files), outdir))

for f in sorted(glob.glob(sys.argv[1])):
    extract(f, os.path.join(os.path.dirname(f) or ".", "files", os.path.basename(f)[:28].replace(" ","_")))
