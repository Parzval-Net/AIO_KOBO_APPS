"""Browser mode: this same engine running inside Pyodide (CPython on WebAssembly) in a Web Worker.

The worker (web/aio-worker.js) mirrors the Kobo into Pyodide's in-memory filesystem: the directory tree with
empty placeholders plus the content of the small files the engine reads directly (configs, NickelMenu entries,
the AIO state). Any other file is read from the device only when a transaction reads or backs it up
(txn.LAZY_LOADER), so a library of gigabytes is never copied into memory.

Planning and applying then run exactly as on the desktop (allowlist, denylist, backups, journal, auto-rollback),
but against the in-memory copy: a failure never reaches the device. Only after a successful run does the worker
save the backup to a folder of the user's computer and write the changed paths back (sync ops), reverting them if
the device write fails. Browsers without write access get every change in one KoboRoot.tgz instead.

Every function the worker calls takes and returns JSON strings, so no Python object crosses into JavaScript.
"""

from __future__ import annotations

import hashlib
import io
import json
import re
import shutil
import time
import zipfile
from pathlib import Path, PurePosixPath

from . import __version__, checks, core, engine, txn
from .core import REGISTRY, AioError, Artifact, Context
from .device import KoboDevice, parse_version_file
from .nickelmenu import lint_dir

ROOT = Path("/kobo")
INBOX = Path.home() / "Kobo-AIO" / "inbox"
AIO_HOME = Path.home() / "Kobo-AIO"

# Modules that need the desktop engine (native tools, huge downloads or the whole library).
ENGINE_ONLY = {
    "fw.kobopatch": "usa kobopatch y el firmware oficial",
    "maint.backup": "copia toda la biblioteca a tu computador",
    "lib.books": "la conversión a KEPUB usa kepubify",
    "app.audiobook": "el paquete pesa 173 MB",
    "app.inkboy": "el emulador se compila en tu computador",
}
# Files read directly (outside a transaction) by checks, status functions and modules: always mirrored.
EAGER_FILES = {".kobo/version", ".kobo/Kobo/Kobo eReader.conf", "metadata.calibre", ".metadata_never_index"}
# Big app trees under .adds are only touched through transactions (lazy is enough).
LAZY_TREES = (".adds/koreader/", ".adds/cadmus/", ".adds/plato/", ".adds/inkboy/")
EAGER_MAX = 1 << 20
JUNK_MAX = 64 << 10


class _Bridge:
    loader = None          # rel -> bytes (JS: FileReaderSync over the device File objects)
    fetcher = None         # url -> bytes (JS: synchronous XHR, allowed in workers)
    progress = None        # str -> None
    artifact_base = ""
    lazy: set[str] = set()


B = _Bridge()


def _bytes(data) -> bytes:
    if hasattr(data, "to_bytes"):      # Pyodide JsProxy of a Uint8Array
        return data.to_bytes()
    return bytes(data)


def _safe_rel(rel: str) -> str:
    raw = str(rel).replace("\\", "/").strip("/")
    if not raw or ".." in raw.split("/") or PurePosixPath(raw).is_absolute():
        raise AioError(f"Ruta inválida en el Kobo: {rel}")
    return raw


def ensure(rel: str) -> None:
    """Replace a placeholder with the real file content from the device (once)."""
    rel = str(rel)
    if rel in B.lazy:
        data = _bytes(B.loader(rel))
        B.lazy.discard(rel)
        dst = ROOT / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)


def is_eager(rel: str, size: int) -> bool:
    name = rel.rsplit("/", 1)[-1]
    if rel in EAGER_FILES:
        return True
    if name.startswith("._") or name == ".DS_Store":
        return size <= JUNK_MAX
    return rel.startswith(".adds/") and size <= EAGER_MAX and not rel.startswith(LAZY_TREES)


def _web_fetch(self: Context, art: Artifact) -> bytes:
    """Context.fetch for the browser: same-origin copy of the pinned artifact, same SHA-256 check."""
    core.CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cached = core.CACHE_DIR / f"{art.sha256[:16]}-{art.filename}"
    if cached.is_file():
        data = cached.read_bytes()
        if hashlib.sha256(data).hexdigest() == art.sha256:
            return data
        cached.unlink()
    self.progress(f"Descargando {art.filename}...")
    try:
        data = _bytes(B.fetcher(f"{B.artifact_base}/{art.sha256[:16]}-{art.filename}"))
    except Exception as exc:  # noqa: BLE001 - network/HTTP errors surface as JS exceptions
        raise AioError(f"No se pudo descargar {art.filename} en el navegador: {exc}") from exc
    digest = hashlib.sha256(data).hexdigest()
    if digest != art.sha256:
        raise AioError(f"SHA-256 inválido para {art.filename}: esperado {art.sha256}, obtenido {digest}. "
                       "Instalación abortada para proteger el dispositivo.")
    cached.write_bytes(data)
    return data


def _no_download(url: str, timeout: int = 180) -> bytes:
    raise AioError("Esta descarga no está disponible en el navegador. Descarga el archivo en tu computador "
                   f"({url}) y súbelo aquí, o usa Kobo AIO en tu computador.")


def _space(root: Path) -> checks.Finding:
    return checks.Finding("space", "ok", "Espacio libre: revísalo en tu computador",
                          tech="El navegador no informa el espacio libre del Kobo")


def _install_hooks() -> None:
    from .modules import library  # noqa: F401  (registers modules; patched below)
    txn.LAZY_LOADER = ensure
    Context.fetch = _web_fetch
    core.download = _no_download
    library.download = _no_download
    checks.check_space = _space


def mount(entries_json: str, loader, fetcher, artifact_base: str, progress=None) -> str:
    """Mirror the device tree. entries: [[rel, size]], size -1 for directories."""
    B.loader, B.fetcher, B.artifact_base = loader, fetcher, artifact_base.rstrip("/")
    B.progress = progress or (lambda m: None)
    if ROOT.exists():
        shutil.rmtree(ROOT)
    ROOT.mkdir(parents=True)
    B.lazy = set()
    eager = []
    for rel, size in json.loads(entries_json):
        rel = _safe_rel(rel)
        if size < 0:
            (ROOT / rel).mkdir(parents=True, exist_ok=True)
            continue
        (ROOT / rel).parent.mkdir(parents=True, exist_ok=True)
        (ROOT / rel).touch()
        B.lazy.add(rel)
        if is_eager(rel, size):
            eager.append(rel)
    for rel in eager:
        ensure(rel)
    _install_hooks()
    if not (ROOT / ".kobo" / "version").is_file():
        raise AioError("Esa carpeta no es un Kobo: elige la unidad KOBOeReader (debe tener la carpeta .kobo).")
    return json.dumps(_device().to_dict(), ensure_ascii=False)


def _device() -> KoboDevice:
    dev = parse_version_file(ROOT)
    if dev is None:
        raise AioError("No se pudo leer el modelo del Kobo (.kobo/version)")
    return dev


def catalog_json() -> str:
    dev = _device()
    items = engine.catalog(dev)
    for it in items:
        why = ENGINE_ONLY.get(it["id"])
        if why and it.get("compatible") is not False:
            it["compatible"] = False
            it["incompatible_reason"] = f"Disponible con Kobo AIO abierto en tu computador: {why}"
            it["engine_only"], it["engine_reason"] = True, why
    return json.dumps({"device": dev.to_dict(), "modules": items, "installed": engine.installed_modules(ROOT),
                       "version": __version__, "web": True}, ensure_ascii=False)


def doctor_json() -> str:
    for suffix in ("", "-wal", "-shm"):
        ensure(f".kobo/KoboReader.sqlite{suffix}")
    dev = _device()
    rep = checks.run_doctor(dev)
    rep["nickelmenu_lint"] = lint_dir(ROOT / ".adds" / "nm")
    return json.dumps(rep, ensure_ascii=False)


def save_upload(name: str, data) -> str:
    clean = re.sub(r"[^\w.\- ()\[\]áéíóúñÁÉÍÓÚÑ,'&]+", "_", PurePosixPath(str(name)).name).strip(" .") or "archivo"
    dest_dir = INBOX / time.strftime("%Y%m%d")
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest, n = dest_dir / clean, 2
    while dest.exists():
        dest = dest_dir / f"{Path(clean).stem} ({n}){Path(clean).suffix}"
        n += 1
    dest.write_bytes(_bytes(data))
    return json.dumps({"path": str(dest)})


def _check_selection(selection: dict) -> None:
    engine.catalog()
    blocked = [m for m in selection if m in ENGINE_ONLY]
    if blocked:
        raise AioError(f"{REGISTRY[blocked[0]].title} necesita Kobo AIO abierto en tu computador: "
                       f"{ENGINE_ONLY[blocked[0]]}.")
    inbox = INBOX.resolve()
    for mid, opts in selection.items():
        mod = REGISTRY.get(mid)
        if not mod or not isinstance(opts, dict):
            continue
        resolved = mod.resolved_options(opts)
        for o in mod.options:
            if o.type == "files":
                for v in resolved.get(o.id) or []:
                    if not Path(str(v)).resolve().is_relative_to(inbox):
                        raise AioError(f"Archivo no permitido: {v}. Súbelo desde esta página.")


def _home_rel(path) -> str:
    return str(Path(path).resolve().relative_to(AIO_HOME.resolve()))


def sync_ops(rels) -> list[dict]:
    """What the device must look like for these paths after the in-memory run."""
    ops = []
    for rel in dict.fromkeys(rels):
        p = ROOT / rel
        ops.append({"rel": rel, "op": "write"} if p.is_file() else {"rel": rel, "op": "delete"})
    return ops


def run_json(op: str, selection_json: str, uninstall_json: str = "[]") -> str:
    selection, uninstall = json.loads(selection_json), json.loads(uninstall_json)
    _check_selection(selection)
    dev = _device()
    progress = B.progress or (lambda m: None)
    fn = engine.apply if op == "apply" else engine.plan
    res = fn(dev, selection, uninstall, offline=False, progress=progress)
    if op == "apply" and res.get("manifest"):
        backup_dir = Path(res["manifest"]).parent
        res["backup_dir"] = str(backup_dir)
        res["backup_rel"] = _home_rel(backup_dir)
        res["sync"] = sync_ops(c["rel"] for c in res["changes"])
    return json.dumps(res, ensure_ascii=False)


def backups_json() -> str:
    dev = _device()
    out = []
    for m in txn.list_backups(dev.serial)[:30]:
        try:
            data = txn.load_manifest(m)
        except (OSError, ValueError):
            continue
        out.append({"path": str(m), "time": data.get("time", m.parent.name), "label": data.get("label"),
                    "complete": data.get("complete", True), "changes": len(data.get("changes", [])),
                    "rel": _home_rel(m.parent)})
    return json.dumps(out, ensure_ascii=False)


def rollback_json(manifest_path: str, dry_run: bool = False) -> str:
    dev = _device()
    manifest = Path(manifest_path).resolve()
    if not manifest.is_relative_to((txn.HOST_BACKUP_ROOT / (dev.serial or "unknown")).resolve()):
        raise AioError("Ese respaldo no pertenece al Kobo conectado")
    data = txn.load_manifest(manifest)
    rels = [txn.check_rollback_path(c["rel"]) for c in data.get("changes", [])]
    for rel in rels:
        ensure(rel)
    before = {rel: (txn.sha256_file(ROOT / rel) if (ROOT / rel).is_file() else None) for rel in rels}
    existing = {p.name for p in manifest.parent.iterdir()}
    actions = txn.rollback(manifest, ROOT, dry_run=bool(dry_run), expected_serial=dev.serial)
    if dry_run:
        return json.dumps({"actions": actions, "sync": [], "persist": []}, ensure_ascii=False)
    changed = [rel for rel in rels if before[rel] != (txn.sha256_file(ROOT / rel) if (ROOT / rel).is_file() else None)]
    new_dirs = [p.name for p in manifest.parent.iterdir() if p.name not in existing]
    return json.dumps({"actions": actions, "sync": sync_ops(changed),
                       "persist": [_home_rel(manifest.parent / d) for d in new_dirs]},
                      ensure_ascii=False)


def export_koboroot(sync_json: str) -> bytes:
    """Read-only browsers: every change packed into ONE KoboRoot.tgz for the user to drop into .kobo/.

    Nickel extracts it over / on the next boot, so files land at mnt/onboard/<path> without replacing whole
    folders (copying a folder in Finder would wipe the other mods inside .adds). Mod packages that were already
    pending in .kobo/KoboRoot.tgz are merged in as they are.
    """
    import tarfile
    ops = json.loads(sync_json)
    if any(o["op"] == "delete" for o in ops):
        raise AioError("Este cambio necesita borrar archivos del Kobo y en este navegador no se puede. "
                       "Usa Chrome o Edge, o Kobo AIO en tu computador.")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.GNU_FORMAT) as out:
        for o in ops:
            rel = o["rel"]
            if rel == ".kobo/KoboRoot.tgz":
                with tarfile.open(ROOT / rel, mode="r:gz") as pending:
                    for m in pending.getmembers():
                        core._member_name(m)   # same path-safety check as validate_koboroot
                        out.addfile(m, pending.extractfile(m) if m.isfile() else None)
                continue
            try:
                txn.check_writable(rel)
            except txn.SafetyError:
                txn.check_library_path(rel)
            data = (ROOT / rel).read_bytes()
            info = tarfile.TarInfo(f"mnt/onboard/{rel}")
            info.size, info.mode, info.mtime = len(data), 0o644, int(time.time())
            out.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def export_backup_zip(backup_dir: str) -> bytes:
    """The originals a ZIP install replaces (kept on the computer, never copied to the Kobo)."""
    files = Path(backup_dir) / "files"
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        if files.is_dir():
            for f in sorted(files.rglob("*")):
                if f.is_file():
                    zf.write(f, str(f.relative_to(files)))
        manifest = Path(backup_dir) / "manifest.json"
        if manifest.is_file():
            zf.write(manifest, "manifest.json")
    return buf.getvalue()
