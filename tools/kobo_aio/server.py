"""Local helper: serves the web UI and a JSON API on 127.0.0.1 only.

The same UI is published on GitHub Pages; from there it talks to this helper through CORS. Protections:
  * binds 127.0.0.1 only; Host header must be 127.0.0.1/localhost (blocks DNS rebinding)
  * cross-origin requests only from ALLOWED_ORIGINS; state-changing calls need the X-Kobo-AIO header,
    which a foreign page cannot send without passing the CORS preflight
  * uploads go to ~/Kobo-AIO/inbox with sanitized names and a size cap
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import threading
import time
import traceback
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from . import __version__, engine
from .checks import run_doctor
from .core import AioError
from .device import candidate_mounts, find_device, parse_version_file
from .nickelmenu import lint_dir
from .txn import list_backups, load_manifest, rollback

# The desktop app (PyInstaller) unpacks web/ next to the code; from the repository it is two levels up.
WEB_DIR = (Path(getattr(sys, "_MEIPASS", "")) / "web") if getattr(sys, "frozen", False) else \
    Path(__file__).resolve().parents[2] / "web"
INBOX = Path.home() / "Kobo-AIO" / "inbox"
MAX_UPLOAD = 4 * 1024 ** 3
ALLOWED_ORIGINS = {"https://parzval-net.github.io"} | {
    o.strip() for o in __import__("os").environ.get("KOBO_AIO_EXTRA_ORIGINS", "").split(",") if o.strip()}
JOBS: dict[str, dict] = {}
LOCK = threading.Lock()   # one write job at a time


def _device(root: str | None):
    dev = find_device(Path(root)) if root else find_device()
    if dev is None:
        raise AioError("No hay un Kobo conectado. Conéctalo por USB y toca «Conectar».")
    return dev


def eject_volume(root: Path) -> tuple[bool, str]:
    """Safely eject the Kobo on macOS, Linux or Windows. Returns (ok, message for the user)."""
    if sys.platform == "darwin":
        cmds = [["diskutil", "eject", str(root)]]
    elif sys.platform == "win32":
        drive = str(root)[:2]   # "E:"
        script = f"(New-Object -ComObject Shell.Application).Namespace(17).ParseName('{drive}').InvokeVerb('Eject')"
        cmds = [["powershell", "-NoProfile", "-NonInteractive", "-Command", script]]
    else:
        cmds = [["gio", "mount", "-u", str(root)], ["umount", str(root)]]
    out = ""
    for cmd in cmds:
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired) as exc:
            out = str(exc)
            continue
        out = (proc.stdout + proc.stderr).strip()
        if proc.returncode == 0:
            if sys.platform == "win32" and (root / ".kobo").exists():
                return False, "Windows no pudo expulsarlo: usa «Quitar hardware de forma segura» en la barra de tareas."
            return True, out
    return False, out or "No se pudo expulsar: hazlo desde tu sistema antes de desconectar el cable."


def _check_file_options(selection: dict) -> None:
    """Paths sent by the web UI must live in the user's home and never inside hidden folders (~/.ssh...)."""
    from .core import REGISTRY
    home = Path.home().resolve()
    for mid, opts in (selection or {}).items():
        mod = REGISTRY.get(mid)
        if not mod or not isinstance(opts, dict):
            continue
        resolved = mod.resolved_options(opts)  # same normalisation the engine applies (splits on commas)
        for o in mod.options:
            if o.type != "files":
                continue
            raw = opts.get(o.id) or []
            values = set(resolved.get(o.id) or []) | set([raw] if isinstance(raw, str) else raw)
            for v in values:
                rp = Path(str(v)).expanduser().resolve()
                rel = rp.relative_to(home) if rp.is_relative_to(home) else None
                if rel is None or any(part.startswith(".") for part in rel.parts):
                    raise AioError(f"Ruta no permitida desde la web: {v}. Usa archivos de tu carpeta de usuario.")


def _run_job(kind: str, fn) -> str:
    jid = uuid.uuid4().hex[:12]
    job = {"id": jid, "kind": kind, "state": "running", "log": [], "result": None, "error": None,
           "started": time.time()}
    JOBS[jid] = job

    def progress(msg: str):
        job["log"].append(msg)

    def worker():
        if not LOCK.acquire(blocking=False):
            job.update(state="error", error="Ya hay una operación en curso. Espera a que termine.")
            return
        try:
            job["result"] = fn(progress)
            job["state"] = "done"
        except AioError as exc:
            job.update(state="error", error=str(exc))
        except Exception as exc:  # noqa: BLE001
            job.update(state="error", error=f"{type(exc).__name__}: {exc}")
            job["log"].append(traceback.format_exc(limit=3))
        finally:
            LOCK.release()

    threading.Thread(target=worker, daemon=True).start()
    return jid


class Handler(BaseHTTPRequestHandler):
    server_version = f"KoboAIO/{__version__}"

    def log_message(self, fmt, *args):  # keep the terminal quiet (and free of device serials)
        pass

    # -- security --------------------------------------------------------
    def _origin_ok(self) -> bool:
        host = (self.headers.get("Host") or "").split(":")[0]
        if host not in ("127.0.0.1", "localhost"):
            return False
        origin = self.headers.get("Origin")
        if origin is None:
            return True
        port = self.server.server_address[1]
        local = {f"http://127.0.0.1:{port}", f"http://localhost:{port}"}
        return origin in local or origin in ALLOWED_ORIGINS

    def _cors(self):
        origin = self.headers.get("Origin")
        if origin and self._origin_ok():
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
            self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Kobo-AIO, X-Filename")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Private-Network", "true")

    def _json(self, code: int, obj) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self._cors()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        if not self._origin_ok():
            self.send_error(403)
            return
        self.send_response(204)
        self._cors()
        self.end_headers()

    # -- routes ----------------------------------------------------------
    def do_GET(self):
        if not self._origin_ok():
            self.send_error(403, "Origen no permitido")
            return
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        try:
            if url.path in ("/", "/index.html"):
                return self._static("index.html")
            if url.path.startswith("/assets/"):
                return self._static(url.path[1:])
            if url.path == "/api/status":
                return self._json(200, {"ok": True, "version": __version__, "platform": sys.platform})
            if url.path == "/api/devices":
                return self._json(200, [parse_version_file(m).to_dict() for m in candidate_mounts()])
            if url.path == "/api/catalog":
                dev = find_device(Path(q["root"])) if q.get("root") else find_device()
                return self._json(200, {"device": dev.to_dict() if dev else None, "modules": engine.catalog(dev),
                                        "installed": engine.installed_modules(dev.root) if dev else {}})
            if url.path == "/api/doctor":
                dev = _device(q.get("root"))
                rep = run_doctor(dev)
                rep["nickelmenu_lint"] = lint_dir(dev.root / ".adds" / "nm")
                return self._json(200, rep)
            if url.path == "/api/backups":
                dev = _device(q.get("root"))
                out = []
                for m in list_backups(dev.serial)[:30]:
                    try:
                        data = load_manifest(m)
                    except (OSError, ValueError):
                        continue
                    out.append({"path": str(m), "time": data.get("time", m.parent.name), "label": data.get("label"),
                                "complete": data.get("complete", True), "changes": len(data.get("changes", []))})
                return self._json(200, out)
            if url.path.startswith("/api/jobs/"):
                job = JOBS.get(url.path.rsplit("/", 1)[1])
                return self._json(200 if job else 404, job or {"error": "no existe"})
            self.send_error(404)
        except AioError as exc:
            self._json(409, {"error": str(exc)})

    def do_POST(self):
        if not self._origin_ok() or self.headers.get("X-Kobo-AIO") != "1":
            self.send_error(403, "Solicitud no autorizada")
            return
        url = urlparse(self.path)
        try:
            if url.path == "/api/upload":
                return self._upload()
            length = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(length) or b"{}") if length else {}
            root = body.get("root")
            if url.path in ("/api/plan", "/api/apply"):
                dev = _device(root)
                sel, unin = body.get("selection", {}), body.get("uninstall", [])
                engine.catalog()  # make sure the registry is loaded
                _check_file_options(sel)
                fn = engine.plan if url.path == "/api/plan" else engine.apply
                jid = _run_job(url.path[5:], lambda p: fn(dev, sel, unin, offline=bool(body.get("offline")), progress=p))
                return self._json(202, {"job": jid})
            if url.path == "/api/rollback":
                dev = _device(root)
                from .txn import HOST_BACKUP_ROOT
                manifest = Path(body["manifest"]).resolve()
                if not manifest.is_relative_to((HOST_BACKUP_ROOT / (dev.serial or "unknown")).resolve()):
                    raise AioError("Ese respaldo no pertenece al Kobo conectado")
                jid = _run_job("rollback", lambda p: {"actions": rollback(manifest, dev.root, bool(body.get("dry_run")),
                                                                         expected_serial=dev.serial)})
                return self._json(202, {"job": jid})
            if url.path == "/api/eject":
                dev = _device(root)
                ok, output = eject_volume(dev.root)
                return self._json(200 if ok else 500, {"ok": ok, "output": output})
            self.send_error(404)
        except AioError as exc:
            self._json(409, {"error": str(exc)})
        except (ValueError, KeyError) as exc:
            self._json(400, {"error": f"Solicitud inválida: {exc}"})

    def _upload(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_UPLOAD:
            return self._json(413, {"error": "Archivo vacío o demasiado grande (máx. 4 GB)"})
        raw = self.headers.get("X-Filename") or "archivo"
        try:
            from urllib.parse import unquote
            raw = unquote(raw)
        except Exception:  # noqa: BLE001
            pass
        name = re.sub(r"[^\w.\- ()\[\]áéíóúñÁÉÍÓÚÑ,'&]+", "_", Path(raw).name).strip(" .") or "archivo"
        dest_dir = INBOX / time.strftime("%Y%m%d")
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / name
        n = 2
        while dest.exists():
            dest = dest_dir / f"{Path(name).stem} ({n}){Path(name).suffix}"
            n += 1
        remaining = length
        with open(dest, "wb") as fh:
            while remaining > 0:
                chunk = self.rfile.read(min(1 << 20, remaining))
                if not chunk:
                    break
                fh.write(chunk)
                remaining -= len(chunk)
        if remaining:
            dest.unlink(missing_ok=True)
            return self._json(400, {"error": "Subida incompleta"})
        return self._json(200, {"path": str(dest), "name": dest.name, "size": length})

    def _static(self, rel: str):
        path = (WEB_DIR / rel).resolve()
        if not path.is_relative_to(WEB_DIR.resolve()) or not path.is_file():
            self.send_error(404)
            return
        ctype = {".html": "text/html; charset=utf-8", ".js": "text/javascript", ".css": "text/css",
                 ".svg": "image/svg+xml", ".png": "image/png", ".json": "application/json"}.get(path.suffix,
                                                                                               "application/octet-stream")
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def _already_running(port: int) -> bool:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/status", timeout=2) as r:
            return "version" in json.loads(r.read() or b"{}")
    except Exception:  # noqa: BLE001
        return False


def serve(port: int = 8765, open_browser: bool = True) -> None:
    url = f"http://127.0.0.1:{port}/"
    try:
        httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    except OSError:
        if _already_running(port):   # double-clicked twice: just bring the UI back
            print(f"Kobo AIO ya está abierto en {url}", flush=True)
            if open_browser:
                import webbrowser
                webbrowser.open(url)
            return
        raise
    print(f"Kobo AIO {__version__} listo en {url}", flush=True)
    print("Deja esta ventana abierta mientras lo usas. Para salir, ciérrala (o Ctrl+C).", flush=True)
    if open_browser:
        try:
            import webbrowser
            webbrowser.open(url)
        except Exception:  # noqa: BLE001
            pass
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
