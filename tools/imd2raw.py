import sys, glob, os
SIZES = [128,256,512,1024,2048,4096,8192]

def conv(path, out):
    d = open(path,'rb').read()
    p = d.index(b'\x1a')+1
    img = bytearray()
    while p < len(d):
        mode, cyl, head, nsec, ssz = d[p], d[p+1], d[p+2], d[p+3], d[p+4]
        p += 5
        hflag = head; head &= 0x3f
        smap = list(d[p:p+nsec]); p += nsec
        if hflag & 0x80: p += nsec
        if hflag & 0x40: p += nsec
        size = SIZES[ssz]
        sec = {}
        for i in range(nsec):
            t = d[p]; p += 1
            if t == 0: sec[smap[i]] = b'\xe5'*size
            elif t in (1,3,5,7): sec[smap[i]] = d[p:p+size]; p += size
            elif t in (2,4,6,8): sec[smap[i]] = bytes([d[p]])*size; p += 1
        for s in range(min(smap), min(smap)+nsec):   # physical order by ID
            img += sec[s]
    open(out,'wb').write(bytes(img))
    print("%s -> %s (%d bytes)" % (os.path.basename(path), os.path.basename(out), len(img)))

for f in sorted(glob.glob(sys.argv[1])):
    conv(f, os.path.splitext(f)[0] + ".raw")
