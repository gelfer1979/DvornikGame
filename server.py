from http.server import HTTPServer, SimpleHTTPRequestHandler

class MyHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        # These two headers are often REQUIRED for Wasm games to run
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def guess_type(self, path):
        # Force the correct MIME type for Wasm
        if path.endswith(".wasm"):
            return "application/wasm"
        return super().guess_type(path)

print("Starting server on http://localhost:8000 (with COOP/COEP)...")
HTTPServer(("", 8000), MyHandler).serve_forever()