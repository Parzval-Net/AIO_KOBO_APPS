#!/usr/bin/env python3
"""
Local HTTP server for the Ink-boy Kobo Web Installer (installer/index.html).
Serves:
  - GET /                  -> installer/index.html
  - GET /dist/inkboy       -> dist/Kobo-InkBoy/.adds/inkboy/inkboy
  - GET /dist/inkboy.sh    -> dist/Kobo-InkBoy/.adds/inkboy/inkboy.sh
  - GET /dist/roms/<name>  -> dist/Kobo-InkBoy/.adds/inkboy/roms/<name>
  - POST /api/auto-install -> Invokes tools/kobo_auto_installer.py automatically
"""
import json
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DIST_DIR = PROJECT_ROOT / "dist" / "Kobo-InkBoy" / ".adds" / "inkboy"


class InstallerHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        clean_path = self.path.split("?", 1)[0]
        if clean_path in ("/", "/index.html", "/installer", "/installer/", "/installer/index.html"):
            html = (PROJECT_ROOT / "installer" / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(html)
        elif clean_path == "/favicon.ico":
            self.send_response(204)
            self.end_headers()
        elif clean_path in ("/dist/inkboy", "/dist/inkboy.sh") and (DIST_DIR / clean_path[6:]).is_file():
            data = (DIST_DIR / clean_path[6:]).read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)
        elif self.path.startswith("/dist/roms/"):
            rom_name = Path(self.path).name
            rom_path = DIST_DIR / "roms" / rom_name
            if rom_path.is_file():
                data = rom_path.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.end_headers()
                self.wfile.write(data)
                return
            self.send_error(404)
        else:
            self.send_error(404)

    def do_POST(self):
        # Only the installer page itself may trigger a write to the Kobo: reject cross-site requests
        origin = self.headers.get("Origin")
        host = self.headers.get("Host", "")
        if origin is not None and origin not in (f"http://{host}", f"https://{host}"):
            self.send_error(403, "Cross-origin request rejected")
            return
        if self.path == "/api/auto-install":
            proc = subprocess.run(
                [sys.executable, str(PROJECT_ROOT / "tools" / "kobo_auto_installer.py")],
                capture_output=True,
                text=True,
            )
            payload = {
                "ok": proc.returncode == 0,
                "returncode": proc.returncode,
                "log": (proc.stdout + "\n" + proc.stderr).strip(),
            }
            body = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)


def main():
    import webbrowser
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    url = f"http://127.0.0.1:{port}/"
    print(f"=== Ink-boy Kobo Web Installer running at {url} ===", flush=True)
    server = HTTPServer(("127.0.0.1", port), InstallerHandler)
    try:
        webbrowser.open(url)
    except Exception:
        pass
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
