"""Read-only health checks ("doctor") for a mounted Kobo. Nothing in this module writes to the device."""

from __future__ import annotations

import json
import shutil
import sqlite3
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from .device import KoboDevice
from .qtini import QtIni
from .txn import is_appledouble

CONF_REL = ".kobo/Kobo/Kobo eReader.conf"
# Raw text as it must appear in the file (QSettings doubles every backslash).
EXCLUDE_SYNC_RAW = r"(\\.(?!kobo|adobe).+|([^.][^/]*/)+\\..+)"
BOOK_EXTS = {".epub", ".kepub", ".pdf", ".cbz", ".cbr", ".mobi", ".txt", ".rtf", ".html", ".htm", ".fb2", ".djvu"}
SKIP_DIRS = {".Spotlight-V100", ".Trashes", ".TemporaryItems", ".fseventsd", "System Volume Information"}


@dataclass
class Finding:
    id: str
    severity: str        # ok | info | warn | critical
    title: str
    detail: str = ""
    fix: str = ""        # module/action id that fixes it, if any
    data: dict = field(default_factory=dict)

    def to_dict(self) -> dict:
        return self.__dict__.copy()


def _walk_files(root: Path):
    stack = [root]
    while stack:
        d = stack.pop()
        try:
            entries = list(d.iterdir())
        except OSError:
            continue
        for e in entries:
            if e.name in SKIP_DIRS:
                continue
            try:
                if e.is_dir():
                    stack.append(e)
                elif e.is_file():
                    yield e
            except OSError:
                continue


def scan_volume(root: Path) -> dict:
    books, junk = [], []
    for f in _walk_files(root):
        rel = f.relative_to(root)
        if f.name.startswith("._"):
            if is_appledouble(f):
                junk.append(str(rel))
            continue
        if f.name == ".DS_Store":
            junk.append(str(rel))
            continue
        if rel.parts[0].startswith("."):
            continue  # hidden system/mod trees are not library content
        if f.suffix.lower() in BOOK_EXTS or f.name.lower().endswith(".kepub.epub"):
            books.append(str(rel))
    return {"books": sorted(books), "junk": sorted(junk)}


def check_exclude_sync(ini: QtIni) -> Finding:
    raw = ini.get_raw("FeatureSettings", "ExcludeSyncFolders")
    if raw is None:
        return Finding("exclude_sync", "warn", "ExcludeSyncFolders no está definido",
                       "Nickel indexará carpetas ocultas como .adds (ROMs, scripts, imágenes).", "conf.exclude_sync")
    if raw == EXCLUDE_SYNC_RAW:
        return Finding("exclude_sync", "ok", "ExcludeSyncFolders correcto")
    if "\\" not in raw or "\\\\" not in raw:
        return Finding("exclude_sync", "critical", "ExcludeSyncFolders corrupto",
                       f"Valor actual: {raw}. Sin las barras invertidas dobles Nickel oculta todas las subcarpetas "
                       "y saca esos libros de la biblioteca.", "conf.exclude_sync", {"raw": raw})
    return Finding("exclude_sync", "info", "ExcludeSyncFolders personalizado", f"Valor actual: {raw}",
                   "conf.exclude_sync", {"raw": raw})


def check_library(root: Path, scan: dict) -> list[Finding]:
    out: list[Finding] = []
    meta = root / "metadata.calibre"
    if meta.is_file():
        try:
            entries = json.loads(meta.read_text(encoding="utf-8", errors="replace"))
            expected = {e.get("lpath", "") for e in entries if e.get("lpath")}
            present = set(scan["books"])
            missing = sorted(expected - present)
            if missing:
                out.append(Finding("calibre_missing", "critical" if len(missing) > 3 else "warn",
                                   f"{len(missing)} libro(s) de Calibre no están en el Kobo",
                                   "metadata.calibre los registra pero el archivo no existe. No escribas en el "
                                   "Kobo antes de intentar recuperarlos (tools/recover_kobo_fat32.py).",
                                   data={"missing": missing}))
            else:
                out.append(Finding("calibre_missing", "ok", f"Los {len(expected)} libros de Calibre están presentes"))
        except (ValueError, OSError) as exc:
            out.append(Finding("calibre_missing", "warn", "metadata.calibre ilegible", str(exc)))
    return out


def check_database(root: Path, scan: dict) -> list[Finding]:
    db = root / ".kobo" / "KoboReader.sqlite"
    if not db.is_file():
        return [Finding("db", "warn", "No se encontró KoboReader.sqlite")]
    out: list[Finding] = []
    with tempfile.TemporaryDirectory() as tmp:
        # Work on a copy (with WAL/SHM) so the device database is never opened for writing.
        for suffix in ("", "-wal", "-shm"):
            src = db.with_name(db.name + suffix)
            if src.is_file():
                shutil.copyfile(src, Path(tmp) / (db.name + suffix))
        conn = sqlite3.connect(f"file:{Path(tmp) / db.name}?mode=ro", uri=True)
        try:
            integrity = conn.execute("PRAGMA quick_check").fetchone()[0]
            sideloaded = conn.execute(
                "SELECT count(*) FROM content WHERE ContentType=6 AND ContentID LIKE 'file:///mnt/onboard/%'"
            ).fetchone()[0]
        except sqlite3.DatabaseError as exc:
            return [Finding("db", "critical", "KoboReader.sqlite dañada", str(exc))]
        finally:
            conn.close()
    size_mb = db.stat().st_size / (1024 * 1024)
    out.append(Finding("db_integrity", "ok" if integrity == "ok" else "critical",
                       "Base de datos íntegra" if integrity == "ok" else "Base de datos con errores",
                       f"quick_check={integrity}, tamaño {size_mb:.1f} MB"))
    on_disk = len(scan["books"])
    if on_disk and sideloaded < on_disk:
        out.append(Finding("db_sideloaded", "info", f"Nickel indexó {sideloaded} de {on_disk} libros en disco",
                           "Es normal justo después de copiar libros: se importan al expulsar el Kobo."))
    return out


def check_junk(scan: dict, root: Path) -> list[Finding]:
    out = []
    n = len(scan["junk"])
    out.append(Finding("macos_junk", "ok" if n == 0 else "warn",
                       "Sin archivos basura de macOS" if n == 0 else f"{n} archivo(s) basura de macOS (._*, .DS_Store)",
                       "" if n == 0 else "No dañan libros, pero ensucian el volumen y pueden confundir a otros lectores.",
                       "maint.clean_macos" if n else ""))
    spot = (root / ".Spotlight-V100").exists() and not (root / ".metadata_never_index").exists()
    out.append(Finding("spotlight", "warn" if spot else "ok",
                       "Spotlight indexa el Kobo" if spot else "Spotlight desactivado para el Kobo",
                       "macOS escribe índices en el lector en cada conexión." if spot else "",
                       "maint.no_spotlight" if spot else ""))
    return out


def check_space(root: Path) -> Finding:
    usage = shutil.disk_usage(root)
    free_gb = usage.free / 1024**3
    sev = "ok" if free_gb > 1 else ("warn" if free_gb > 0.3 else "critical")
    return Finding("space", sev, f"Espacio libre: {free_gb:.1f} GB de {usage.total / 1024**3:.1f} GB")


def check_mods(root: Path) -> list[Finding]:
    out = []
    adds = root / ".adds"
    mods = sorted(p.name for p in adds.iterdir() if p.is_dir()) if adds.is_dir() else []
    out.append(Finding("mods", "info", "Mods detectados: " + (", ".join(mods) if mods else "ninguno"),
                       data={"mods": mods}))
    if (root / ".kobo" / "KoboRoot.tgz").is_file():
        out.append(Finding("koboroot_pending", "info", "Hay un KoboRoot.tgz pendiente",
                           "Se instalará al expulsar el Kobo y reiniciarse."))
    nm_dir = adds / "nm"
    if nm_dir.is_dir() and not (nm_dir / "doc").is_file():
        out.append(Finding("nickelmenu", "warn", "Hay configuración de NickelMenu pero NickelMenu no parece instalado",
                           "NickelMenu crea .adds/nm/doc en su primer arranque.", "mod.nickelmenu"))
    return out


def run_doctor(device: KoboDevice) -> dict:
    root = device.root
    findings: list[Finding] = []
    if not device.is_known:
        findings.append(Finding("device", "warn", f"Modelo no reconocido ({device.model_id})",
                                "Las funciones específicas del modelo se desactivan."))
    else:
        findings.append(Finding("device", "ok", f"{device.name} con firmware {device.firmware or '?'}"))
    scan = scan_volume(root)
    findings.append(check_exclude_sync(QtIni.load(root / CONF_REL)))
    findings += check_library(root, scan)
    findings += check_database(root, scan)
    findings += check_junk(scan, root)
    findings.append(check_space(root))
    findings += check_mods(root)
    order = {"critical": 0, "warn": 1, "info": 2, "ok": 3}
    findings.sort(key=lambda f: order.get(f.severity, 9))
    return {
        "device": device.to_dict(),
        "books_on_disk": len(scan["books"]),
        "findings": [f.to_dict() for f in findings],
        "summary": {s: sum(1 for f in findings if f.severity == s) for s in order},
    }
