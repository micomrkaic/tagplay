#!/usr/bin/env python3
"""Frames painted by any TAB view must fit the terminal when :fft is
on; an over-tall frame scrolls the terminal and ghosts the spectrum
(the 'triple FFT' bug). Plays a fixture, enables :fft, cycles TAB
through every focus, and asserts no frame exceeds the window height."""
import os, pty, sys, time, fcntl, termios, struct, re, signal
signal.alarm(120)

ROWS, COLS = 40, 110
lib = sys.argv[1] if len(sys.argv) > 1 else "testlib"
pid, fd = pty.fork()
if pid == 0:
    os.environ["SDL_AUDIODRIVER"] = "dummy"
    os.environ["TERM"] = "xterm-256color"
    os.execvp("./tagplay", ["./tagplay", lib])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
fl = fcntl.fcntl(fd, fcntl.F_GETFL)
fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

def send(s, wait=0.5):
    os.write(fd, s.encode()); time.sleep(wait)

def drain():
    out = b""
    end = time.time() + 2.0
    while time.time() < end:
        try:
            b = os.read(fd, 65536)
            if b: out += b
        except (OSError, BlockingIOError):
            pass
        time.sleep(0.05)
    return out.decode(errors="replace")

time.sleep(2.0)              # scan + first paint
send("\r", 1.5)              # play first track
send("\x15", 0.2)            # Ctrl-U: clean line
send(":fft\r", 1.0)          # spectrum on
drain()
TOK = re.compile(r"\x1b\[(\d*)(?:;(\d+))?([Hsu])|\x1b([78])|(\r\n)")
def scrolls(stream):
    cur, saved, n = 1, 1, 0
    for m in TOK.finditer(stream):
        p1, _, fin, esc, nl = m.groups()
        if nl:
            cur += 1
            if cur > ROWS: n += 1; cur = ROWS
        elif fin == "H": cur = int(p1) if p1 else 1
        elif fin == "s" or esc == "7": saved = cur
        elif fin == "u" or esc == "8": cur = saved
    return n

bad = 0
for i in range(6):           # TAB through all foci twice
    send("\t", 0.8)
    s = scrolls(drain())
    if s:
        bad += 1
        print(f"FAIL tab {i}: terminal scrolled {s}x (frame taller than {ROWS})")
send("q", 0.3); send("q", 0.3)
try: os.kill(pid, 15)
except Exception: pass
if bad == 0:
    print("ok   fft chrome fits every TAB view")
sys.exit(1 if bad else 0)
