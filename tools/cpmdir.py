import sys, glob, os
# IBM 3740 SSSD: 77x26x128, 2 reserved tracks, BLS=1024, DRM=63, skew 6
SKEW = [1,7,13,19,25,5,11,17,23,3,9,15,21,2,8,14,20,26,6,12,18,24,4,10,16,22]

def logsec(img, track, lsec):          # lsec 0..25 logical -> physical via skew
    phys = SKEW[lsec] - 1
    return img[(track*26 + phys)*128:(track*26+phys)*128+128]

def dirents(img):
    ents = []
    for i in range(64):               # 2 dir blocks = 16 sectors... DRM=63 -> 64 entries = 2048 B = 16 sectors
        s = i // 4
        e = img and logsec(img, 2 + s//26, s % 26)[(i%4)*32:(i%4)*32+32]
        ents.append(e)
    return ents

def show(path):
    img = open(path,'rb').read()
    print("="*72); print(os.path.basename(path))
    files = {}
    for i in range(64):
        s = i // 4
        rec = logsec(img, 2 + s//26, s % 26)[(i%4)*32:(i%4)*32+32]
        st = rec[0]
        if st == 0xE5: continue
        name = "".join(chr(b & 0x7f) for b in rec[1:9]).rstrip()
        ext  = "".join(chr(b & 0x7f) for b in rec[9:12]).rstrip()
        attr = ""
        if rec[9] & 0x80: attr += "R/O "
        if rec[10] & 0x80: attr += "SYS "
        ex = rec[12]; rc = rec[15]
        key = (st, name, ext)
        files.setdefault(key, [0, attr])
        files[key][0] += rc*128
    for (u,n,e),(sz,a) in sorted(files.items(), key=lambda x:(x[0][0],x[0][1])):
        print("  [%d] %-8s.%-3s %7d  %s" % (u, n, e, sz, a))
    print("  files:", len(files))

for f in sorted(glob.glob(sys.argv[1])):
    show(f)
