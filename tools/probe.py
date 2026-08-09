#!/usr/bin/env python3
"""Boot the Monarch, let it stall, then SIGINT into the ICE and look around."""
import os, pty, select, time, signal, sys, re

cmds = sys.argv[1:] or ['?']

pid, fd = pty.fork()
if pid == 0:
    os.chdir(os.path.expanduser('~/monarch/z80pack/monarchsim'))
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

def send(x):
    for ch in x.encode():
        os.write(fd, bytes([ch])); time.sleep(0.01)
    pump(0.4)

def wait(pat, t=30):
    dl = time.time() + t
    while time.time() < dl:
        if re.search(pat.encode(), buf, re.S): return True
        pump(0.4)
    return False

wait(r'>>>', 20)
send('g\r')
pump(18)                       # let it run until it wedges
os.kill(pid, signal.SIGINT)    # z80pack turns SIGINT into a user interrupt
pump(2)
buf = b''
for c in cmds:
    send(c + '\r')
    pump(2.0)
    print("----- %s -----" % c)
    print(buf.decode('latin1'))
    buf = b''

try:
    os.kill(pid, signal.SIGKILL); os.waitpid(pid, 0)
except Exception:
    pass
