#!/usr/bin/env python3
"""Serve the DOSBox-X wasm build with the cross-origin isolation headers
required for SharedArrayBuffer (the pthread build runs the emulator in a
Web Worker)."""
import http.server
import os

class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

os.chdir(os.path.dirname(os.path.abspath(__file__)))
print('Open http://localhost:8000/')
http.server.ThreadingHTTPServer(('', 8000), Handler).serve_forever()
