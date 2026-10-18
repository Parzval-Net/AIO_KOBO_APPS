"""Module framework: catalog, compatibility rules, downloads with SHA-256 pinning and KoboRoot.tgz merging."""

from __future__ import annotations

import hashlib
import io
import json
import os
import platform
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from .device import KoboDevice
from .txn import AioError, SafetyError, Transaction  # noqa: F401 (re-exported)

PROJECT_ROOT = Path(__file__).resolve().parents[2]
CACHE_DIR = Path.home() / "Kobo-AIO" / "cache"
STATE_REL = ".adds/kobo-aio/state.json"  # .adds is excluded from library sync by ExcludeSyncFolders
USER_AGENT = "Kobo-AIO/1.0 (+local installer)"




@dataclass
class Option:
    id: str
    label: str
    type: str = "bool"            # bool | choice | int
    default: object = False
    choices: list = field(default_factory=list)   # [(value, label)]
    help: str = ""
    icons: dict = field(default_factory=dict)     # choice value -> [svg idle, svg active] (shown as a picker)
    retired: tuple = ()                           # multi values that no longer exist: dropped without error

    def to_dict(self) -> dict:
        d = {"id": self.id, "label": self.label, "type": self.type, "default": self.default,
             "choices": [{"value": v, "label": l} for v, l in self.choices], "help": self.help}
        if self.icons:
            d["icons"] = self.icons
        return d


@dataclass
class Artifact:
    """A pinned third-party download. sha256 must match or the install aborts."""
    url: str
    sha256: str
    filename: str
    size: int = 0

    def to_dict(self) -> dict:
        return {"url": self.url, "sha256": self.sha256, "filename": self.filename, "size": self.size}


@dataclass
class Context:
    device: KoboDevice
    txn: Transaction
    offline: bool = False
    koboroot_parts: list[tuple[str, bytes]] = field(default_factory=list)  # (label, KoboRoot.tgz bytes)
    firmware_parts: list[tuple[str, bytes]] = field(default_factory=list)  # validated by modules.firmware
    state: dict = field(default_factory=dict)
    progress: Callable[[str], None] = lambda msg: None

    # -- downloads -------------------------------------------------------
    def fetch(self, art: Artifact) -> bytes:
        CACHE_DIR.mkdir(parents=True, exist_ok=True)
        cached = CACHE_DIR / f"{art.sha256[:16]}-{art.filename}"
        if cached.is_file():
            data = cached.read_bytes()
            if hashlib.sha256(data).hexdigest() == art.sha256:
                return data
            cached.unlink()
        if self.offline:
            raise AioError(f"Sin conexión y {art.filename} no está en caché ({CACHE_DIR}).")
        self.progress(f"Descargando {art.filename}...")
        data = download(art.url)
        digest = hashlib.sha256(data).hexdigest()
        if digest != art.sha256:
            raise AioError(f"SHA-256 inválido para {art.filename}: esperado {art.sha256}, obtenido {digest}. "
                           "Instalación abortada para proteger el dispositivo.")
        cached.write_bytes(data)
        return data

    # -- KoboRoot.tgz ----------------------------------------------------
    def add_koboroot(self, label: str, data: bytes, extra_allowed: "KoboRootAllowance | None" = None) -> None:
        """Queue a KoboRoot.tgz part. `extra_allowed` widens the mod-tree allowlist for this part only."""
        validate_koboroot(data, label, extra_allowed)
        self.koboroot_parts.append((label, data))

    def add_firmware(self, label: str, data: bytes) -> None:
        """Patched/original Nickel binaries (already size/ELF-validated against the official firmware)."""
        self.firmware_parts.append((label, data))


def host_key() -> str:
    """'<os>-<arch>' of the computer running the engine: darwin|linux|windows - arm64|x86_64."""
    osname = {"darwin": "darwin", "win32": "windows", "cygwin": "windows"}.get(sys.platform, "linux")
    m = platform.machine().lower()
    arch = "arm64" if m in ("arm64", "aarch64", "armv8l", "armv8b") else "x86_64" if m in ("x86_64", "amd64", "x64") else m
    return f"{osname}-{arch}"


def host_os() -> str:
    return host_key().split("-", 1)[0]


INSTALL_HINTS = {
    "unar": {"darwin": "en Terminal: brew install unar", "linux": "en una terminal: sudo apt install unar (o el gestor de tu distribución)",
             "windows": "instala 7-Zip desde https://www.7-zip.org"},
    "ffmpeg": {"darwin": "en Terminal: brew install ffmpeg", "linux": "en una terminal: sudo apt install ffmpeg",
               "windows": "en PowerShell: winget install Gyan.FFmpeg"},
    "pymupdf": {"darwin": "en Terminal: pip3 install pymupdf", "linux": "en una terminal: pip3 install pymupdf",
                "windows": "en PowerShell: py -m pip install pymupdf"},
}


def install_hint(tool: str) -> str:
    return INSTALL_HINTS[tool][host_os()]


def install_tool(ctx: "Context", art: Artifact) -> Path:
    """Put a pinned native tool in ~/Kobo-AIO/bin, executable on this OS, and return its path."""
    data = ctx.fetch(art)
    dst = Path.home() / "Kobo-AIO" / "bin" / art.filename
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists() or dst.read_bytes() != data:
        dst.write_bytes(data)
        if host_os() != "windows":
            dst.chmod(0o755)
        if host_os() == "darwin":
            subprocess.run(["xattr", "-d", "com.apple.quarantine", str(dst)], capture_output=True)
    return dst


def download(url: str, timeout: int = 180) -> bytes:
    """HTTPS GET with full certificate verification.

    python.org builds of Python on macOS ship without root certificates until 'Install Certificates'
    is run, so fall back to certifi and finally to the system curl (macOS keychain trust store).
    Integrity is still enforced by the caller through SHA-256 pinning.
    """
    import ssl

    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    errors = []
    contexts = [ssl.create_default_context()]
    try:
        import certifi  # type: ignore
        contexts.append(ssl.create_default_context(cafile=certifi.where()))
    except ImportError:
        pass
    for ctx in contexts:
        try:
            with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
                return resp.read()
        except Exception as exc:  # noqa: BLE001 - try next trust store
            errors.append(str(exc))
    try:
        proc = subprocess.run(["curl", "-fsSL", "--proto", "=https", "--max-time", str(timeout),
                               "-A", USER_AGENT, url], capture_output=True, check=True)
        return proc.stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        errors.append(f"curl: {getattr(exc, 'stderr', b'') or exc}")
    raise AioError(f"No se pudo descargar {url}: {' | '.join(errors)}")


# Least privilege: Nickel plugins only ever need these two trees.
ALLOWED_KOBOROOT_PREFIXES = ("usr/local/Kobo/imageformats/", "mnt/onboard/.adds/")


def _member_name(m: tarfile.TarInfo) -> str:
    raw = m.name
    if raw.startswith("/") or ".." in raw.replace("\\", "/").split("/"):
        raise AioError(f"Ruta insegura en KoboRoot.tgz: {raw}")
    name = raw[2:] if raw.startswith("./") else raw
    return name.rstrip("/")


@dataclass(frozen=True)
class KoboRootAllowance:
    """Narrow, explicit exception to ALLOWED_KOBOROOT_PREFIXES for one module's KoboRoot.tgz part.

    `paths` are exact file paths, `prefixes` whole trees. A path listed in `pinned` must also have one of the
    given SHA-256 digests, so a system file can only ever be replaced by a known, reviewed content.
    """
    paths: frozenset = frozenset()
    prefixes: tuple = ()
    pinned: dict = field(default_factory=dict)     # path -> frozenset of allowed sha256 hex digests

    def allows(self, name: str) -> bool:
        return name in self.paths or (bool(self.prefixes) and name.startswith(self.prefixes))


def validate_koboroot(data: bytes, label: str, extra_allowed: KoboRootAllowance | None = None) -> list[str]:
    """Reject archives that could touch core system paths. Only regular files/dirs under mod trees
    (plus the exact paths/trees of `extra_allowed`, with pinned contents where it says so)."""
    names = []
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for m in tf.getmembers():
            name = _member_name(m)
            if not name or name == ".":
                continue
            if not (m.isfile() or m.isdir()):
                raise AioError(f"{label}: tipo de entrada no permitido (enlace o dispositivo): {m.name}")
            if m.isfile() and not name.startswith(ALLOWED_KOBOROOT_PREFIXES):
                if extra_allowed is None or not extra_allowed.allows(name):
                    raise AioError(f"{label}: KoboRoot.tgz escribe fuera de rutas de mods ({m.name}). Abortado.")
                if name in extra_allowed.pinned:
                    digest = hashlib.sha256(tf.extractfile(m).read()).hexdigest()
                    if digest not in extra_allowed.pinned[name]:
                        raise AioError(f"{label}: contenido no reconocido para {name}. Abortado.")
            names.append(name)
    return names


def merge_koboroots(parts: list[tuple[str, bytes]], pending: bytes | None = None) -> bytes:
    """Combine several KoboRoot.tgz into one (Nickel processes a single KoboRoot.tgz per boot).

    `pending` is a KoboRoot.tgz already waiting on the device: its files are kept unless a new part
    ships the same path (the newer version wins). Two new parts shipping the same file is a conflict.
    """
    entries: dict[str, tuple[str, tarfile.TarInfo, bytes | None]] = {}
    for label, data in parts:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as src:
            for m in src.getmembers():
                key = _member_name(m)
                if not (m.isfile() or m.isdir()):
                    raise AioError(f"{label}: tipo de entrada no permitido en KoboRoot.tgz: {m.name}")
                if key in entries and not m.isdir() and entries[key][0] != label:
                    raise AioError(f"Conflicto: {label} y {entries[key][0]} instalan el mismo archivo {key}")
                entries.setdefault(key, (label, m, src.extractfile(m).read() if m.isfile() else None))
    if pending:
        with tarfile.open(fileobj=io.BytesIO(pending), mode="r:gz") as src:
            for m in src.getmembers():
                key = _member_name(m)
                if not (m.isfile() or m.isdir()):
                    raise AioError(f"KoboRoot.tgz pendiente con entrada no permitida: {m.name}")
                if key not in entries:
                    entries[key] = ("pendiente", m, src.extractfile(m).read() if m.isfile() else None)
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w:gz", format=tarfile.GNU_FORMAT) as dst:
        for key in sorted(entries, key=lambda k: (entries[k][1].isfile(), k)):  # directories first
            _, m, data = entries[key]
            dst.addfile(m, io.BytesIO(data) if data is not None else None)
    return out.getvalue()


def zip_members(data: bytes, prefix: str) -> list[tuple[str, bytes, int]]:
    """Return (relative_path, bytes, unix_mode) for zip members under prefix."""
    res = []
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        for info in zf.infolist():
            if info.is_dir() or not info.filename.startswith(prefix):
                continue
            rel = info.filename[len(prefix):]
            if not rel or ".." in Path(rel).parts:
                continue
            res.append((rel, zf.read(info), (info.external_attr >> 16) & 0o777))
    return res


@dataclass
class Module:
    id: str
    category: str
    title: str
    description: str
    risk: str = "bajo"                       # bajo | medio | alto
    recommended: bool = False
    requires: list[str] = field(default_factory=list)
    options: list[Option] = field(default_factory=list)
    sources: list[str] = field(default_factory=list)   # upstream URLs (credit + docs)
    license: str = ""
    soc: tuple = ()                          # restrict to SoC families, empty = all
    color_only: bool = False
    buttons_only: bool = False
    needs_bluetooth: bool = False            # Bluetooth audio (audiobooks)
    min_fw: tuple = ()
    max_fw: tuple = ()                       # exclusive upper bound, e.g. (5,) for NickelHook (Qt5) mods
    model_ids: tuple = ()                    # restrict to hardware model ids, empty = all
    exact_firmwares: tuple = ()              # restrict to exact firmware builds (firmware patches)
    reboot: bool = False                     # needs eject + reboot to take effect
    order: int = 50                          # execution priority (backup first, cleanup last)
    persistent: bool = True                  # False = one-shot action (not recorded as installed)
    apply_fn: Callable[["Module", Context, dict], None] | None = None
    uninstall_fn: Callable[["Module", Context], None] | None = None
    status_fn: Callable[["Module", KoboDevice], str] | None = None

    def compatible(self, dev: KoboDevice) -> tuple[bool, str]:
        if self.soc and dev.model.get("soc") not in self.soc:
            return False, "No compatible con el SoC de este modelo"
        if self.color_only and not dev.model.get("color"):
            return False, "Solo para modelos a color"
        if self.buttons_only and not dev.model.get("page_buttons"):
            return False, "Requiere botones físicos de página"
        if self.needs_bluetooth and not dev.model.get("bluetooth"):
            return False, "Tu modelo no tiene Bluetooth para audio"
        if self.model_ids and dev.model_id not in self.model_ids:
            return False, "No disponible para este modelo"
        if self.exact_firmwares and dev.firmware not in self.exact_firmwares:
            return False, f"Disponible solo para firmware {', '.join(self.exact_firmwares)}"
        if (self.max_fw or self.min_fw) and not dev.firmware_tuple:
            return False, "Firmware no reconocido: por seguridad se desactiva"
        if self.max_fw and dev.firmware_tuple >= self.max_fw:
            return False, f"No compatible con firmware {'.'.join(map(str, self.max_fw))}.x o superior (Qt6)"
        if self.min_fw and dev.firmware_tuple < self.min_fw:
            return False, f"Requiere firmware {'.'.join(map(str, self.min_fw))} o superior"
        return True, ""

    def status(self, dev: KoboDevice) -> str:
        return self.status_fn(self, dev) if self.status_fn else "desconocido"

    def resolved_options(self, values: dict | None) -> dict:
        """Merge user values with defaults and coerce them to each option's type (CLI/JSON/UI agnostic)."""
        values = values or {}
        out = {}
        for o in self.options:
            v = values.get(o.id, o.default)
            if o.type in ("multi", "files"):
                if isinstance(v, str):
                    v = [x for x in v.split(",") if x] if o.type == "multi" or "," in v else ([v] if v else [])
                v = list(v or [])
                if o.type == "multi" and o.choices:
                    v = [x for x in v if str(x) not in o.retired]
                    allowed = {str(c[0]) for c in o.choices}
                    bad = [x for x in v if str(x) not in allowed]
                    if bad:
                        raise AioError(f"{self.title}: valor no válido en «{o.label}»: {', '.join(map(str, bad))}")
            elif o.type == "bool":
                v = v if isinstance(v, bool) else str(v).strip().lower() in ("1", "true", "yes", "si", "sí", "on")
            elif o.type == "choice" and o.choices:
                match = [c[0] for c in o.choices if str(c[0]) == str(v)]
                if not match:
                    raise AioError(f"{self.title}: opción no válida para «{o.label}»: {v}")
                v = match[0]
            elif o.type == "text":
                v = "" if v is None else str(v)
            out[o.id] = v
        return out

    def to_dict(self, dev: KoboDevice | None = None) -> dict:
        d = {
            "id": self.id, "category": self.category, "title": self.title, "description": self.description,
            "risk": self.risk, "recommended": self.recommended, "requires": self.requires,
            "options": [o.to_dict() for o in self.options], "sources": self.sources, "license": self.license,
            "reboot": self.reboot, "can_uninstall": self.uninstall_fn is not None,
            "persistent": self.persistent,
        }
        if dev is not None:
            ok, why = self.compatible(dev)
            d.update({"compatible": ok, "incompatible_reason": why, "status": self.status(dev) if ok else "n/a"})
        return d


REGISTRY: dict[str, Module] = {}


def register(mod: Module) -> Module:
    if mod.id in REGISTRY:
        raise ValueError(f"Duplicate module id {mod.id}")
    REGISTRY[mod.id] = mod
    return mod


def load_state(root: Path) -> dict:
    p = root / STATE_REL
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"installed": {}}


def resolve_order(selected: list[str], installed: dict | None = None) -> list[str]:
    """Topological order of the selection plus missing requirements (already installed ones are skipped)."""
    order: list[str] = []
    installed = installed or {}
    explicit = set(selected)

    def visit(mid: str, stack: tuple = ()):
        if mid in order:
            return
        if mid not in explicit and mid in installed:
            return
        if mid in stack:
            raise AioError(f"Dependencia circular en {mid}")
        mod = REGISTRY.get(mid)
        if mod is None:
            raise AioError(f"Módulo desconocido: {mid}")
        for r in mod.requires:
            visit(r, stack + (mid,))
        order.append(mid)

    for m in sorted(selected, key=lambda x: (REGISTRY[x].order if x in REGISTRY else 50)):
        visit(m)
    return order
