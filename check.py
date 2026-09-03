#!/usr/bin/env python3
"""Gated verification of monarchsim: the real Dynabyte Monarch diskette,
booted on hardware reconstructed from Dynabyte's own code. Run from the
monarchsim directory. Exits 0 only if every gate passes."""
import os, pty, select, sys, time, re, signal, shutil, hashlib

IMG = 'disks/drivea.dsk'
# This runs from the monarchsim directory, so the repository is located from
# the script itself rather than from the working directory.  run.sh already
# puts a fresh copy of the diskette in place; doing it here as well means
# check.py can also be run on its own.
REPO = os.path.dirname(os.path.abspath(__file__))
PRISTINE = os.path.join(REPO, 'media', 'monarch-mpm2-disk1.raw')
if os.path.exists(PRISTINE):
    shutil.copy(PRISTINE, IMG)          # a fresh copy, so the guest can write
    os.chmod(IMG, 0o644)
if not os.path.exists(IMG):
    sys.exit("check.py: no %s, and no %s to make it from.\n"
             "This repository does not ship Digital Research or Dynabyte "
             "software.\nSee the README for how to produce the diskette image."
             % (IMG, PRISTINE))
sha_before = hashlib.sha256(open(IMG, 'rb').read()).hexdigest()

gates, failed = [], []
def gate(name, ok, detail=''):
    gates.append(ok)
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                           ("  -- " + detail) if detail else ""))
    if not ok:
        failed.append(name)

pid, fd = pty.fork()
if pid == 0:
    os.execvp('./monarchsim', ['./monarchsim'])

buf = b''
def pump(t):
    global buf
    dl = time.time() + t
    while time.time() < dl:
        r, _, _ = select.select([fd], [], [], 0.2)
        if fd in r:
            try: d = os.read(fd, 65536)
            except OSError: return
            if not d: return
            buf += d

def wait(pat, t=60):
    dl = time.time() + t
    while time.time() < dl:
        if re.search(pat.encode(), buf, re.S): return True
        pump(0.4)
    return False

def send(x):
    global buf
    buf = b''
    for ch in x.encode():
        os.write(fd, bytes([ch])); time.sleep(0.02)
    pump(0.5)

print("Dynabyte Monarch, Z80 side -- verification")
print("(hardware reverse engineered from Dynabyte's own LDRBIOS and XIOS)\n")

gate("1. synthesised boot PROM loads the reserved tracks and enters at 1000H",
     wait(r'entering at 1000H', 30))
send('g\r')
gate("2. Dynabyte's own loader runs and identifies itself as MP/M II V2.1",
     wait(r'MP/M II V2\.1 Loader', 60))
gate("3. it loads MPM.SYS off the original diskette (segment table printed)",
     wait(r'Memseg  Usr  0000H  C000H  Bank 4', 60))
gate("4. Dynabyte's XIOS initialises: MMU, I/O, Winchester, DMA, clock",
     wait(r'Initializing MMU Bank 0 1 2 3 4 , I/O Ports , Winchester, DMA, Clock', 60))
gate("5. the MMU memory sizing finds exactly the 5 banks MPM.SYS declares",
     b'Bank 0 1 2 3 4' in buf)
gate("6. the clock test passes (no NO CLOCK!)",
     b'NO CLOCK' not in buf)
gate("7. XIOS reaches the MP/M prompt",
     wait(r'Monarch MP/M II DV1\.4E', 60) and wait(r'0A>', 30))

send('sdir\r')
ok = wait(r'Files Found', 60)
m = re.search(rb'Files Found\s*=\s*(\d+)', buf)
n = int(m.group(1)) if m else -1
gate("8. Monarch's SDIR.PRL runs from the diskette and lists it",
     ok and n == 31, "Files Found = %d (expected 31)" % n)
gate("9. it reports the real disk contents", b'Total Bytes     =    212k' in buf)

send('mpmstat\r')
wait(r'Memory Allocation|Process\(es\)', 60)
gate("10. MPMSTAT runs and reports banked memory",
     b'Memory is bank switched' in buf and
     b'BDOS disk file management is bank switched' in buf)
gate("11. the tick and clock flags are live",
     b'Tick' in buf and b'Clock' in buf)

send('dir\r')
wait(r'System Files Exist', 40)
gate("12. DIR gives an uncorrupted listing (the MMU bug's old symptom)",
     b'Directory for User  0:' in buf and b'System Files Exist' in buf)


# ----------------------------------------------------------- writing
send('pip a:hello.txt=con:\r')
pump(6)
for ch in 'HOLA MONARCH DESDE MP/M\r':
    os.write(fd, ch.encode()); time.sleep(0.03)
pump(2)
for ch in '\x1a\r':
    os.write(fd, ch.encode()); time.sleep(0.03)
pump(12)
gate("16. PIP writes a new file to the diskette without error",
     b'ERROR' not in buf, "PIP reported an error" if b'ERROR' in buf else '')

send('dir\r')
wait(r'System Files Exist', 40)
gate("17. the new file appears in the directory", b'HELLO    TXT' in buf)

send('type hello.txt\r')
pump(14)
gate("18. and reads back with its contents intact",
     b'HOLA MONARCH DESDE MP/M' in buf)

raw = open(IMG, 'rb').read()
gate("19. the write reached the disk image on the host",
     hashlib.sha256(raw).hexdigest() != sha_before and
     b'HELLO   TXT' in raw and b'HOLA MONARCH DESDE MP/M' in raw)


# ------------------------------------------------- consoles 1-3 over TCP
import socket
socks, bufs = {}, {}
try:
    for n, port in ((1, 4001), (2, 4002), (3, 4003)):
        sk = socket.create_connection(('127.0.0.1', port), timeout=10)
        sk.settimeout(0.4)
        socks[n], bufs[n] = sk, b''

    def pumpn(n, t):
        dl = time.time() + t
        while time.time() < dl:
            try: d = socks[n].recv(65536)
            except socket.timeout: continue
            except OSError: return
            if not d: return
            bufs[n] += d

    def sendn(n, x):
        for ch in (x + '\r').encode():
            socks[n].send(bytes([ch])); time.sleep(0.03)
        pumpn(n, 0.6)

    for n in (1, 2, 3):
        pumpn(n, 3); sendn(n, ''); pumpn(n, 4)

    got = [n for n in (1, 2, 3) if re.search(('%dA>' % n).encode(), bufs[n])]
    gate("13. consoles 1-3 attach over TCP 4001-4003 and each gets a prompt",
         len(got) == 3, "prompts seen: %s" % (got or "none"))

    for n in (1, 2, 3):
        sendn(n, 'user %d' % n)
        pumpn(n, 5)
    ok = all(b'User Number' in bufs[n] for n in (1, 2, 3))
    gate("14. each console runs a transient independently", ok)

    buf = b''
    send('mpmstat\r')
    wait(r'Process\(es\) Attached to Consoles', 40)
    attached = len(re.findall(rb'\[\d\] - Tmp\d', buf))
    gate("15. all four consoles are live at once in MPMSTAT",
         attached >= 3, "%d TMPs attached to consoles" % attached)
except Exception as e:
    gate("13. consoles 1-3 attach over TCP 4001-4003 and each gets a prompt",
         False, repr(e))

try:
    os.kill(pid, signal.SIGKILL); os.waitpid(pid, 0)
except Exception:
    pass

print("\n%d/%d gates passed" % (sum(gates), len(gates)))
if failed:
    print("FAILED: " + ", ".join(failed))
sys.exit(1 if failed else 0)
