#!/usr/bin/env python3
"""Relocate an MP/M .SPR (page relocatable) module to a given page.

Layout: 256-byte header (byte 0 = 0, bytes 1-2 = code length), then the code,
then a relocation bitmap, one bit per code byte, MSB first. A set bit means
that byte is the high half of an address and the load page must be added.
"""
import sys

def reloc(path, page, out):
    d = open(path, 'rb').read()
    n = d[1] | (d[2] << 8)
    code = bytearray(d[256:256 + n])
    bits = d[256 + n:]
    nfix = 0
    for i in range(n):
        if bits[i >> 3] & (0x80 >> (i & 7)):
            code[i] = (code[i] + page) & 0xff
            nfix += 1
    open(out, 'wb').write(bytes(code))
    print("%s: code %d bytes (%04X), %d relocations, loaded at %02X00H -> %04X..%04X"
          % (path, n, n, nfix, page, page << 8, (page << 8) + n - 1))

reloc(sys.argv[1], int(sys.argv[2], 16), sys.argv[3])
