"""Pin a local AIO Audio build (KoboRoot.tgz) into ART["aioaudio"] of tools/kobo_aio/modules/_util.py.

Usage: python3 tools/pin_artifact.py path/to/KoboRoot.tgz 0.3.0 [--dry-run]

Checks that the archive holds exactly the two expected members with safe paths, then rewrites the entry with
the release URL (aio-audio-v<version>), SHA-256 and size. Upload that same file to the release afterwards.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import tarfile
from pathlib import Path

UTIL = Path(__file__).resolve().parent / "kobo_aio" / "modules" / "_util.py"
RELEASE_BASE = "https://github.com/Parzval-Net/AIO_KOBO_APPS/releases/download"
EXPECTED = {"usr/local/Kobo/imageformats/libaioaudio.so", "mnt/onboard/.adds/aio-audio/LEEME.txt"}
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?$")
ENTRY_RE = re.compile(r'"aioaudio":\s*Artifact\(\s*"[^"]*",\s*"[0-9a-f]*",\s*"[^"]*",\s*\d+\s*\)')


def validate_tgz(path: Path) -> None:
    """Raise ValueError unless the archive holds exactly the expected regular files under safe paths."""
    try:
        with tarfile.open(path, "r:gz") as tf:
            members = tf.getmembers()
    except (tarfile.TarError, OSError) as e:
        raise ValueError(f"no es un .tgz válido: {e}") from e
    files = set()
    for m in members:
        name = m.name
        if name.startswith("/") or ".." in Path(name).parts:
            raise ValueError(f"ruta insegura en el paquete: {name}")
        rel = name
        while rel.startswith("./"):
            rel = rel[2:]
        rel = rel.rstrip("/")
        if rel in ("", "."):
            continue
        if m.isdir():
            if not any(e.startswith(rel + "/") for e in EXPECTED):
                raise ValueError(f"carpeta inesperada en el paquete: {name}")
            continue
        if not m.isfile():
            raise ValueError(f"entrada no permitida (enlace o dispositivo): {name}")
        if rel in files:
            raise ValueError(f"entrada duplicada: {name}")
        files.add(rel)
    if files != EXPECTED:
        raise ValueError(f"contenido inesperado: sobran {sorted(files - EXPECTED)}, faltan {sorted(EXPECTED - files)}")


def render_entry(version: str, sha256: str, size: int) -> str:
    url = f"{RELEASE_BASE}/aio-audio-v{version}/KoboRoot.tgz"
    return f'"aioaudio": Artifact("{url}",\n                         "{sha256}", "KoboRoot.tgz", {size})'


def rewrite_util(text: str, version: str, sha256: str, size: int) -> str:
    """Return the _util.py text with ART["aioaudio"] replaced; raise ValueError if the entry is not found once."""
    if not VERSION_RE.match(version):
        raise ValueError(f"versión no válida: {version} (usa 0.3.0)")
    if not re.fullmatch(r"[0-9a-f]{64}", sha256) or size <= 0:
        raise ValueError("SHA-256 o tamaño no válidos")
    new, n = ENTRY_RE.subn(lambda _m: render_entry(version, sha256, size), text)
    if n != 1:
        raise ValueError(f"se esperaba una entrada ART['aioaudio'] y se encontraron {n}")
    return new


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Fija un build de AIO Audio en ART['aioaudio']")
    ap.add_argument("tgz", type=Path, help="KoboRoot.tgz descargado del artifact de CI o de la release")
    ap.add_argument("version", help="versión sin la v, por ejemplo 0.3.0")
    ap.add_argument("--util", type=Path, default=UTIL, help=argparse.SUPPRESS)
    ap.add_argument("--dry-run", action="store_true", help="muestra la entrada sin escribir")
    a = ap.parse_args(argv)
    try:
        validate_tgz(a.tgz)
        data = a.tgz.read_bytes()
        sha, size = hashlib.sha256(data).hexdigest(), len(data)
        text = a.util.read_text(encoding="utf-8")
        new = rewrite_util(text, a.version, sha, size)
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(render_entry(a.version, sha, size))
    if not a.dry_run:
        a.util.write_text(new, encoding="utf-8")
        print(f"actualizado {a.util}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
