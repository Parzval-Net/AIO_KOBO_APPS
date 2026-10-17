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
    tech: str = ""       # technical detail for advanced users (shown collapsed)

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
        return Finding("exclude_sync", "warn", "La biblioteca no está protegida",
                       "El Kobo podría mostrar como libros los archivos internos de tus mods (ROMs, scripts, imágenes).",
                       "conf.exclude_sync", tech="[FeatureSettings] ExcludeSyncFolders no está definido")
    if raw == EXCLUDE_SYNC_RAW:
        return Finding("exclude_sync", "ok", "Biblioteca protegida",
                       tech="ExcludeSyncFolders con el escape correcto de Qt")
    if "\\" not in raw or "\\\\" not in raw:
        return Finding("exclude_sync", "critical", "Tus libros en carpetas pueden desaparecer",
                       "Un ajuste dañado hace que el Kobo esconda los libros guardados en subcarpetas y los saque de "
                       "la biblioteca. Corrígelo antes de copiar libros.", "conf.exclude_sync", {"raw": raw},
                       tech=f"ExcludeSyncFolders mal escapado: {raw}")
    return Finding("exclude_sync", "info", "Protección de biblioteca personalizada",
                   "Tiene un valor distinto al recomendado. Si todo se ve bien, puedes dejarlo.",
                   "conf.exclude_sync", {"raw": raw}, tech=f"ExcludeSyncFolders={raw}")


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
                                   f"Faltan {len(missing)} libro(s) que Calibre envió",
                                   "Calibre los registra en el Kobo, pero los archivos ya no están. Vuelve a enviarlos "
                                   "desde Calibre. Si no tienes copia, no copies nada al Kobo antes de intentar "
                                   "recuperarlos.", data={"missing": missing},
                                   tech="Listados en metadata.calibre sin archivo; recuperación: tools/recover_kobo_fat32.py"))
            else:
                out.append(Finding("calibre_missing", "ok", f"Los {len(expected)} libros de Calibre están en el Kobo"))
        except (ValueError, OSError) as exc:
            out.append(Finding("calibre_missing", "warn", "No se pudo leer el índice de Calibre", tech=str(exc)))
    return out


def check_database(root: Path, scan: dict) -> list[Finding]:
    db = root / ".kobo" / "KoboReader.sqlite"
    if not db.is_file():
        return [Finding("db", "warn", "No se encontró la base de datos del Kobo",
                        "Puede pasar en un Kobo recién reseteado. Enciéndelo y vuelve a conectarlo.",
                        tech=".kobo/KoboReader.sqlite no existe")]
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
            return [Finding("db", "critical", "La base de datos del Kobo está dañada",
                            "Tu progreso de lectura y tus notas podrían perderse. Respalda antes de hacer cambios.",
                            tech=f"KoboReader.sqlite: {exc}")]
        finally:
            conn.close()
    size_mb = db.stat().st_size / (1024 * 1024)
    out.append(Finding("db_integrity", "ok" if integrity == "ok" else "critical",
                       "Progreso y notas en buen estado" if integrity == "ok" else "La base de datos tiene errores",
                       "" if integrity == "ok" else "Respalda antes de hacer cambios y considera revisarla con Kobo Utilities.",
                       tech=f"KoboReader.sqlite quick_check={integrity}, {size_mb:.1f} MB"))
    on_disk = len(scan["books"])
    if on_disk and sideloaded < on_disk:
        out.append(Finding("db_sideloaded", "info", f"{on_disk - sideloaded} libro(s) nuevos por importar",
                           "Es normal después de copiar libros: aparecerán al expulsar el Kobo.",
                           tech=f"Nickel indexó {sideloaded} de {on_disk} archivos de libro"))
    return out


def check_junk(scan: dict, root: Path) -> list[Finding]:
    out = []
    n = len(scan["junk"])
    out.append(Finding("macos_junk", "ok" if n == 0 else "warn",
                       "Sin archivos basura del Mac" if n == 0 else f"{n} archivo(s) basura que dejó tu Mac",
                       "" if n == 0 else "No dañan tus libros, pero ocupan espacio y pueden confundir a otros lectores.",
                       "maint.clean_macos" if n else "", tech="Archivos AppleDouble (._*) y .DS_Store"))
    spot = (root / ".Spotlight-V100").exists() and not (root / ".metadata_never_index").exists()
    out.append(Finding("spotlight", "warn" if spot else "ok",
                       "Tu Mac está indexando el Kobo" if spot else "Tu Mac no indexa el Kobo",
                       "Spotlight escribe archivos en el lector cada vez que lo conectas." if spot else "",
                       "maint.no_spotlight" if spot else "", tech=".Spotlight-V100 sin .metadata_never_index"))
    return out


def check_space(root: Path) -> Finding:
    usage = shutil.disk_usage(root)
    free_gb = usage.free / 1024**3
    sev = "ok" if free_gb > 1 else ("warn" if free_gb > 0.3 else "critical")
    return Finding("space", sev, f"{free_gb:.1f} GB libres de {usage.total / 1024**3:.1f} GB",
                   "" if sev == "ok" else "Libera espacio antes de instalar apps o convertir cómics.")


def check_mods(root: Path) -> list[Finding]:
    out = []
    adds = root / ".adds"
    mods = sorted(p.name for p in adds.iterdir() if p.is_dir()) if adds.is_dir() else []
    out.append(Finding("mods", "ok", f"{len(mods)} extra(s) instalados" if mods else "Sin extras instalados",
                       data={"mods": mods}, tech="Carpetas en .adds: " + (", ".join(mods) if mods else "ninguna")))
    if (root / ".kobo" / "KoboRoot.tgz").is_file():
        out.append(Finding("koboroot_pending", "info", "Hay mejoras esperando instalarse",
                           "Expulsa el Kobo: se instalarán solas y el lector se reiniciará (1 a 2 minutos).",
                           tech=".kobo/KoboRoot.tgz pendiente"))
    nm_dir = adds / "nm"
    if nm_dir.is_dir() and not (nm_dir / "doc").is_file():
        out.append(Finding("nickelmenu", "warn", "El menú de atajos no está instalado",
                           "Hay atajos configurados, pero falta NickelMenu para mostrarlos.", "mod.nickelmenu",
                           tech="Existe .adds/nm pero no .adds/nm/doc"))
    return out


def run_doctor(device: KoboDevice) -> dict:
    root = device.root
    findings: list[Finding] = []
    if not device.is_known:
        findings.append(Finding("device", "warn", "Modelo de Kobo no reconocido",
                                "Las funciones específicas de cada modelo quedan desactivadas por seguridad.",
                                tech=f"ID de hardware {device.model_id}"))
    else:
        findings.append(Finding("device", "ok", f"{device.name} reconocido", tech=f"Firmware {device.firmware or '?'}"))
    if device.firmware_tuple[:2] == (4, 46):
        findings.append(Finding("firmware446", "info", "Firmware 4.46 (reciente)",
                                "Los mods funcionan, pero los ajustes avanzados del sistema aún no están disponibles para esta versión.",
                                tech=f"Firmware {device.firmware}: parches 4.46 preliminares en la comunidad"))
    if device.firmware_tuple and device.firmware_tuple >= (5,):
        findings.append(Finding("firmware5", "warn", "Firmware 5 o superior",
                                "Los mods del menú de Nickel aún no funcionan en esta versión; se desactivan.",
                                tech=f"Firmware {device.firmware} (Qt6)"))
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
