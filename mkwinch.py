#!/usr/bin/env python3
"""Fix the label's drive records: they are 17 bytes, not 15.

The loop at B7CF reads each record as

    LD B,00Fh : INIR          ; fifteen bytes, an ordinary CP/M DPB
    ...
    IN L,(C) : IN H,(C)       ; two more
    ADD HL,DE : EX DE,HL      ; which accumulate

so after the DPB the label carries a sixteen-bit value that the XIOS adds up
as it walks the four records -- it is the size of that drive's allocation
vector, used to carve the buffers. Writing the records at a fifteen byte stride
left everything after the first one misaligned.
"""
import sys

SECSZ, HEADS, SPT = 1024, 16, 256   # a physical sector is 1024 bytes
SIZE = 8 * 1024 * 1024
NDRIVES = 4


def off(cyl, head, sec):
    return ((cyl * HEADS + head) * SPT + sec) * SECSZ


def dpb(spt=64, bsh=4, blm=15, exm=0, dsm=1023, drm=511,
        al0=0xff, al1=0xff, cks=0, offset=0):
    return bytes([
        spt & 0xff, spt >> 8,
        bsh, blm, exm,
        dsm & 0xff, dsm >> 8,
        drm & 0xff, drm >> 8,
        al0, al1,
        cks & 0xff, cks >> 8,
        offset & 0xff, offset >> 8,
    ])


def build(path):
    img = bytearray(b'\xe5' * SIZE)

    DSM, DRM, CKS = 1023, 511, 0
    alv = DSM // 8 + 1                    # allocation vector, one bit a block
    csv = CKS // 4 if CKS else 0          # no check vector on a fixed disk
    record = dpb(dsm=DSM, drm=DRM, cks=CKS) + bytes([(alv + csv) & 0xff,
                                                     (alv + csv) >> 8])
    assert len(record) == 17

    label = bytearray(b'\x00' * 1024)
    label[0:8] = b'DYNABYTE'
    label[8:10] = b'21'
    # the head count is the divisor the XIOS uses; fill every parameter
    # with it until we know which byte it takes
    for i in range(16):
        label[10 + i] = HEADS
    for i in range(NDRIVES):
        label[512 + i * 17:512 + i * 17 + 17] = record

    at = off(0, 0, 9)
    img[at:at + len(label)] = label
    open(path, 'wb').write(bytes(img))

    print("%s: %d MB" % (path, SIZE // 1024 // 1024))
    print("  label at cyl 0 head 0 sector 9 (offset %d)" % at)
    print("  %d drive records of 17 bytes: DPB(15) + %d byte allocation vector"
          % (NDRIVES, alv))
    print("  each logical drive: %d KB" % ((DSM + 1) * 2))


build(sys.argv[1] if len(sys.argv) > 1 else 'disks/winchester.img')
