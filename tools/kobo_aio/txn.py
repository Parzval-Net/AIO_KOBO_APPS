"""Transactional writes to a Kobo onboard partition with automatic backup, journal and rollback.

Safety rules enforced here (the AIO never bypasses them):
  * Writes are only allowed under an allowlist of mod/config paths. Books, KoboReader.sqlite and anything
    else Nickel owns are never written, moved or deleted. New library files may only be ADDED.
  * Before a file is overwritten or deleted, the original is copied to the host backup folder.
  * Every change is appended to journal.jsonl BEFORE it happens (write-ahead), so an interrupted run
    (crash, Ctrl-C, unplugged cable) can still be rolled back. commit() writes manifest.json.
  * Rollback only touches a file if it is still exactly in the state this run left it in.
  * dry_run=True plans everything and touches nothing.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

# Relative POSIX prefixes on the Kobo that the AIO may create/modify.
WRITE_ALLOWLIST = (
    ".adds/",
    ".kobo/Kobo/Kobo eReader.conf",
    ".kobo/KoboRoot.tgz",
    ".kobo/screensaver/",
    ".kobo/custom-dict/",
    "fonts/",
    ".metadata_never_index",
)
# Paths that must never be written even if they match the allowlist by accident.
WRITE_DENYLIST = (
    ".kobo/KoboReader.sqlite",
    ".kobo/BookReader.sqlite",
    ".kobo/version",
)
DELETE_JUNK_NAMES = (".DS_Store",)
# New books may be ADDED to the library (never overwritten, never deleted except by rolling back).
LIBRARY_EXTS = (".kepub.epub", ".epub", ".cbz", ".pdf", ".mp3z", ".mp3", ".m4b")
AUDIO_EXTS = (".mp3z", ".mp3", ".m4b")
AUDIO_FOLDER = "Audiobooks"   # audio files may only be added under this top-level folder

HOST_BACKUP_ROOT = Path.home() / "Kobo-AIO" / "backups"
# Browser mode (web.py) mirrors the Kobo with empty placeholders and loads a file's real content the first time a
# transaction reads or backs it up. None on the desktop, where the device is a mounted volume.
LAZY_LOADER = None


class AioError(RuntimeError):
    """Expected, user-facing error (shown without a traceback)."""


class SafetyError(AioError):
    """A write was refused by the safety rules."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_appledouble(path: Path) -> bool:
    """True only for real AppleDouble '._*' sidecar files (magic 0x00051607)."""
    if not path.name.startswith("._") or not path.is_file():
        return False
    try:
        with open(path, "rb") as fh:
            return fh.read(4) == b"\x00\x05\x16\x07"
    except OSError:
        return False


def _norm(rel: str) -> str:
    raw = str(rel).replace("\\", "/")
    p = PurePosixPath(raw)
    if p.is_absolute() or ".." in raw.split("/") or not raw.strip():
        raise SafetyError(f"Ruta no permitida: {rel}")
    return str(p)


def check_writable(rel: str) -> str:
    rel = _norm(rel)
    if any(rel == d or rel.startswith(d.rstrip("/") + "/") for d in WRITE_DENYLIST):
        raise SafetyError(f"Ruta protegida (nunca se modifica): {rel}")
    if not any(rel == a.rstrip("/") or rel.startswith(a) for a in WRITE_ALLOWLIST):
        raise SafetyError(f"Ruta fuera de la lista permitida: {rel}")
    return rel


def check_library_path(rel: str) -> str:
    rel = _norm(rel)
    parts = PurePosixPath(rel).parts
    low = rel.lower()
    if any(p.startswith(".") for p in parts) or not low.endswith(LIBRARY_EXTS):
        raise SafetyError(f"Destino de libro no permitido: {rel}")
    if low.endswith(AUDIO_EXTS) and parts[0] != AUDIO_FOLDER:
        raise SafetyError(f"Los audiolibros van en {AUDIO_FOLDER}/: {rel}")
    return rel


def check_rollback_path(rel: str) -> str:
    """Paths a manifest may legitimately reference: mod paths, added books or validated junk."""
    rel = _norm(rel)
    try:
        return check_writable(rel)
    except SafetyError:
        pass
    name = PurePosixPath(rel).name
    if name.startswith("._") or name in DELETE_JUNK_NAMES:
        return rel
    return check_library_path(rel)


def safe_component(text: str, fallback: str) -> str:
    """Folder/file name component: no separators, no leading dots, no '..'."""
    import re
    s = re.sub(r'[\\/:*?"<>|\x00-\x1f]+', " ", str(text or "")).strip()
    s = s.lstrip(". ").replace("..", ".")
    return s[:120].strip() or fallback


@dataclass
class Change:
    op: str            # write | delete
    rel: str
    existed: bool      # did the file exist BEFORE this transaction touched it?
    before_sha: str = ""
    after_sha: str = ""
    size: int = 0
    note: str = ""

    def to_dict(self) -> dict:
        return self.__dict__.copy()


@dataclass
class Transaction:
    root: Path
    device_serial: str = "unknown"
    dry_run: bool = False
    label: str = "apply"
    changes: list[Change] = field(default_factory=list)
    log: list[str] = field(default_factory=list)

    def __post_init__(self):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.backup_dir = HOST_BACKUP_ROOT / (self.device_serial or "unknown") / f"{stamp}-{self.label}"
        n = 2
        while self.backup_dir.exists():
            self.backup_dir = self.backup_dir.with_name(f"{stamp}-{self.label}-{n}")
            n += 1
        self._orig: dict[str, tuple[bool, str]] = {}   # rel -> (existed before txn, sha before txn)
        self.staged: dict[str, bytes | None] = {}       # rel -> pending content (None = deleted), also in dry-run

    # -- helpers ---------------------------------------------------------
    def _abs(self, rel: str) -> Path:
        return self.root / rel

    def _first_touch(self, rel: str) -> tuple[bool, str]:
        """Record (and back up) the pre-transaction state of a path the first time it is touched."""
        if rel not in self._orig:
            if LAZY_LOADER is not None:
                LAZY_LOADER(rel)
            p = self._abs(rel)
            existed = p.is_file()
            sha = sha256_file(p) if existed else ""
            if existed and not self.dry_run:
                dst = self.backup_dir / "files" / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(p, dst)
            self._orig[rel] = (existed, sha)
        return self._orig[rel]

    def read_bytes(self, rel: str) -> bytes | None:
        """Current content including changes staged earlier in this transaction."""
        rel = _norm(rel)
        if rel in self.staged:
            return self.staged[rel]
        if LAZY_LOADER is not None:
            LAZY_LOADER(rel)
        p = self._abs(rel)
        return p.read_bytes() if p.is_file() else None

    def read_text(self, rel: str) -> str | None:
        data = self.read_bytes(rel)
        return None if data is None else data.decode("utf-8", errors="surrogateescape")

    def exists(self, rel: str) -> bool:
        return self.read_bytes(rel) is not None

    def note(self, msg: str) -> None:
        self.log.append(msg)

    def _journal(self, change: Change) -> None:
        """Write-ahead: persist the change before performing it."""
        self.backup_dir.mkdir(parents=True, exist_ok=True)
        jpath = self.backup_dir / "journal.jsonl"
        with open(jpath, "a", encoding="utf-8") as fh:
            if fh.tell() == 0:
                fh.write(json.dumps({"header": {"label": self.label, "device_serial": self.device_serial,
                                                "root": str(self.root)}}, ensure_ascii=False) + "\n")
            fh.write(json.dumps(change.to_dict(), ensure_ascii=False) + "\n")
            fh.flush()
            os.fsync(fh.fileno())

    def _record(self, change: Change) -> None:
        # One record per path: the first-touch state plus the latest intended result.
        self.changes = [c for c in self.changes if c.rel != change.rel]
        self.changes.append(change)
        if not self.dry_run:
            self._journal(change)

    def _atomic_write(self, dst: Path, data: bytes) -> None:
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name(dst.name + ".aio-tmp")
        with open(tmp, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, dst)

    # -- operations ------------------------------------------------------
    def write_bytes(self, rel: str, data: bytes, note: str = "", mode: int | None = None) -> bool:
        rel = check_writable(rel)
        current = self.read_bytes(rel)
        if current is not None and current == data:
            return False
        existed, before = self._first_touch(rel)
        self._record(Change("write", rel, existed, before, sha256_bytes(data), len(data), note))
        self.staged[rel] = data
        self.note(f"{'Actualizar' if existed else 'Crear'} {rel} ({len(data)} bytes){' - ' + note if note else ''}")
        if self.dry_run:
            return True
        dst = self._abs(rel)
        self._atomic_write(dst, data)
        if mode is not None:
            try:
                os.chmod(dst, mode)
            except OSError:
                pass  # FAT32 ignores permissions; Kobo mounts onboard with exec allowed
        return True

    def write_text(self, rel: str, text: str, note: str = "", mode: int | None = None) -> bool:
        return self.write_bytes(rel, text.encode("utf-8", errors="surrogateescape"), note, mode)

    def copy_file(self, src: Path, rel: str, note: str = "", mode: int | None = None) -> bool:
        return self.write_bytes(rel, Path(src).read_bytes(), note, mode)

    def write_if_absent(self, rel: str, data: bytes, note: str = "") -> bool:
        """Create a file only when missing (user configs, save files)."""
        if self.exists(check_writable(rel)):
            self.note(f"Conservar {rel} (ya existe)")
            return False
        return self.write_bytes(rel, data, note)

    def add_book(self, rel: str, data: bytes, note: str = "") -> str:
        """Add a NEW file to the library. Picks a free name instead of overwriting; returns the final path.

        In dry-run it validates the destination and reserves the name, so plans catch bad paths early.
        """
        rel = check_library_path(rel)
        base, ext = rel, ""
        for e in LIBRARY_EXTS:
            if rel.lower().endswith(e):
                base, ext = rel[: -len(e)], rel[-len(e):]
                break
        final, n = rel, 2
        while self.exists(final):
            final = f"{base} ({n}){ext}"
            n += 1
        self._orig[final] = (False, "")
        self._record(Change("write", final, False, "", sha256_bytes(data), len(data), note or "libro nuevo"))
        self.staged[final] = data
        size = f"{len(data) / 1024 ** 2:.1f} MB" if data else "tamaño al convertir"
        self.note(f"Agregar a la biblioteca {final} ({size})")
        if not self.dry_run:
            self._atomic_write(self._abs(final), data)
        return final

    def delete(self, rel: str, note: str = "") -> bool:
        rel = check_writable(rel)
        if not self.exists(rel):
            return False
        existed, before = self._first_touch(rel)
        self.staged[rel] = None
        if not existed:
            # Created earlier in this transaction: the net effect is "nothing happened".
            self.changes = [c for c in self.changes if c.rel != rel]
            if not self.dry_run and self._abs(rel).is_file():
                self._journal(Change("delete", rel, False, "", "", 0, "deshace un archivo creado en esta corrida"))
                self._abs(rel).unlink()
            return True
        self._record(Change("delete", rel, True, before, "", 0, note))
        self.note(f"Eliminar {rel}{' - ' + note if note else ''}")
        if not self.dry_run:
            self._abs(rel).unlink()
        return True

    def delete_junk(self, path: Path) -> bool:
        """Delete validated macOS junk anywhere on the volume (AppleDouble '._*' or .DS_Store)."""
        if not (is_appledouble(path) or (path.name in DELETE_JUNK_NAMES and path.is_file())):
            raise SafetyError(f"No es basura de macOS validada: {path}")
        rel = _norm(str(path.relative_to(self.root)))
        self._record(Change("delete", rel, True, "", "", path.stat().st_size, "macOS junk"))
        if not self.dry_run:
            path.unlink()
        return True

    # -- lifecycle -------------------------------------------------------
    def manifest(self, complete: bool = True) -> dict:
        return {
            "complete": complete,
            "label": self.label,
            "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "device_serial": self.device_serial,
            "root": str(self.root),
            "dry_run": self.dry_run,
            "changes": [c.to_dict() for c in self.changes],
            "log": self.log,
        }

    def commit(self) -> Path | None:
        if self.dry_run or not self.changes:
            return None
        self.backup_dir.mkdir(parents=True, exist_ok=True)
        path = self.backup_dir / "manifest.json"
        tmp = path.with_name("manifest.json.tmp")
        tmp.write_text(json.dumps(self.manifest(), ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(tmp, path)
        if hasattr(os, "sync"):
            os.sync()
        return path


def list_backups(serial: str | None = None) -> list[Path]:
    base = HOST_BACKUP_ROOT / serial if serial else HOST_BACKUP_ROOT
    if not base.exists():
        return []
    found = {p.parent for p in base.rglob("manifest.json")} | {p.parent for p in base.rglob("journal.jsonl")}
    return sorted((d / "manifest.json" for d in found), reverse=True)


def load_manifest(path: Path) -> dict:
    """Read manifest.json, or rebuild it from journal.jsonl when the run was interrupted.

    A truncated last journal line (power loss mid-write) is ignored.
    """
    path = Path(path)
    if path.is_dir():
        path = path / "manifest.json"
    if path.is_file():
        return json.loads(path.read_text(encoding="utf-8"))
    journal = path.parent / "journal.jsonl"
    header: dict = {}
    by_rel: dict[str, dict] = {}
    for line in journal.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if "header" in obj:
            header = obj["header"]
            continue
        prev = by_rel.get(obj["rel"])
        if prev is not None:
            obj["existed"], obj["before_sha"] = prev["existed"], prev["before_sha"]
        by_rel[obj["rel"]] = obj
    return {"complete": False, "label": header.get("label", "interrumpido"),
            "device_serial": header.get("device_serial"), "root": header.get("root"),
            "time": path.parent.name, "changes": list(by_rel.values())}


def rollback(manifest_path: Path, root: Path, dry_run: bool = False, expected_serial: str | None = None) -> list[str]:
    """Undo a transaction (complete or interrupted).

    For each path: only act if the file is still exactly as this run left it; otherwise keep it and report.
    The current state of every file it touches is itself backed up first, so a rollback can be undone.
    """
    manifest_path = Path(manifest_path)
    if manifest_path.is_dir():
        manifest_path = manifest_path / "manifest.json"
    data = load_manifest(manifest_path)
    if expected_serial and data.get("device_serial") and data["device_serial"] != expected_serial:
        raise SafetyError("Este respaldo es de otro Kobo (número de serie distinto)")
    files_dir = manifest_path.parent / "files"
    safety_dir = manifest_path.parent / f"rollback-{time.strftime('%Y%m%d-%H%M%S')}"
    actions: list[str] = []
    for ch in reversed(data["changes"]):
        rel = check_rollback_path(ch["rel"])
        dst = root / rel
        if ch.get("note") == "macOS junk":
            continue  # junk is not restored on purpose
        cur_sha = sha256_file(dst) if dst.is_file() else None
        left_sha = ch.get("after_sha") if ch["op"] == "write" else None   # state this run left behind
        orig_sha = ch.get("before_sha") if ch["existed"] else None        # state before this run
        if cur_sha == orig_sha:
            continue  # already in the original state (or the step never ran)
        if cur_sha != left_sha:
            actions.append(f"Conservar {rel} (cambió después de la instalación)")
            continue
        if not dry_run and dst.is_file():
            keep = safety_dir / rel
            keep.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(dst, keep)
        if ch["existed"]:
            src = files_dir / rel
            if not src.is_file():
                actions.append(f"Sin copia de respaldo para {rel}: se conserva")
                continue
            actions.append(f"Restaurar {rel}")
            if not dry_run:
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dst)
        else:
            actions.append(f"Eliminar {rel} (creado por el AIO)")
            if not dry_run:
                dst.unlink()
    if not dry_run and hasattr(os, "sync"):
        os.sync()
    return actions
