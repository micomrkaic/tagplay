#!/usr/bin/env python3
# This file is part of tagplay.  GPL-3.0-or-later; see COPYING.
# Serve web/ with the COOP/COEP headers SharedArrayBuffer demands:
#   python3 tools/serve_wasm.py   ->  http://localhost:8000/tagplay.html
import http.server, os, sys

class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

os.chdir(os.path.join(os.path.dirname(__file__), "..", "web"))
port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
http.server.ThreadingHTTPServer(("", port), H).serve_forever()
