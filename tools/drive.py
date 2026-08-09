#!/usr/bin/env python3
"""Drive a console program under a pty: wait for prompts, send keys, log all output.

usage: drive.py <logfile> <timeout> -- <cmd> [args...]   with a script on stdin:
  script lines:  E <regex>      expect regex (re.S), fail if timeout
                 S <text>       send text (\\r \\n \\e \\xNN escapes) -- no CR added
                 L <text>       send text + CR
                 W <seconds>    wait
"""
import os, sys, pty, re, select, time, signal

log_path = sys.argv[1]
GLOBAL_TO = float(sys.argv[2])
cmd = sys.argv[sys.argv.index('--')+1:]

script = []
for line in sys.stdin.read().splitlines():
    if not line.strip() or line.lstrip().startswith('#'):
        continue
    op, _, arg = line.partition(' ')
    script.append((op, arg))

def unesc(s):
    return (s.replace('\\r','\r').replace('\\n','\n').replace('\\e','\x1b')
             .replace('\\t','\t').replace('\\z','\x1a').replace('\\\\','\\'))

pid, fd = pty.fork()
if pid == 0:
    os.execvp(cmd[0], cmd)

buf = b''
log = open(log_path, 'wb')
start = time.time()

def pump(deadline):
    global buf
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if fd in r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                return False
            if not d:
                return False
            buf += d; log.write(d); log.flush()
        else:
            return True
    return True

ok = True
for op, arg in script:
    if time.time() - start > GLOBAL_TO:
        print("GLOBAL TIMEOUT", file=sys.stderr); ok = False; break
    if op == 'E':
        pat = re.compile(arg.encode(), re.S)
        dl = time.time() + 45
        while time.time() < dl:
            if pat.search(buf):
                break
            if not pump(time.time() + 1.0):
                break
        if not pat.search(buf):
            print("EXPECT FAILED: %s" % arg, file=sys.stderr); ok = False; break
        buf = buf[pat.search(buf).end():]
    elif op in ('S', 'L'):
        t = unesc(arg) + ('\r' if op == 'L' else '')
        for ch in t.encode():
            os.write(fd, bytes([ch])); time.sleep(0.01)
        pump(time.time() + 0.4)
    elif op == 'W':
        pump(time.time() + float(arg))

pump(time.time() + 2.0)
log.close()
try:
    os.kill(pid, signal.SIGKILL); os.waitpid(pid, 0)
except Exception:
    pass
sys.exit(0 if ok else 1)
