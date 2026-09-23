#!/usr/bin/env python3
"""Load the pinned, self-hosted SDK under the actual page CSP; no USB access."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
class Quiet(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), partial(Quiet, directory=str(ROOT)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True, args=['--no-sandbox'])
            page = browser.new_page()
            errors = []
            page.on('pageerror', lambda e: errors.append(str(e)))
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            result = page.evaluate("""async () => {
                const {loadSdk} = await import('./sdk.mjs');
                const sdk = await loadSdk();
                return [typeof sdk.Transport, typeof sdk.ESPLoader];
            }""")
            assert result == ['function', 'function'], result
            assert not errors, errors
            print('Real Chromium + page CSP + local pinned SDK import: PASS (no USB attached)')
            browser.close()
    finally:
        server.shutdown()
        server.server_close()

if __name__ == '__main__':
    main()
