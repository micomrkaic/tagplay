#!/usr/bin/env python3
"""Minimal ICY server replaying a captured stream for decoder tests.
Usage: radioserver.py PORT FILE [content-type]"""
import socket, sys, threading, time
port, path = int(sys.argv[1]), sys.argv[2]
ctype = sys.argv[3] if len(sys.argv) > 3 else "audio/aac"
DATA = open(path, 'rb').read()
def client(c):
    try:
        c.recv(4096)
        c.sendall(("HTTP/1.0 200 OK\r\nicy-name:Replay\r\nContent-Type: %s\r\n\r\n"
                   % ctype).encode())
        off = 0
        while True:
            c.sendall(DATA[off:off + 2048])
            off = (off + 2048) % max(1, len(DATA) - 4096)
            time.sleep(0.07)   # ~29 KB/s: forces buffer-starve events
    except Exception:
        pass
    finally:
        c.close()
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', port))
srv.listen(4)
print("ready", flush=True)
while True:
    c, _ = srv.accept()
    threading.Thread(target=client, args=(c,), daemon=True).start()
