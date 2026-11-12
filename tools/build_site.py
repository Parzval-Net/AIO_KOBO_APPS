"""Build the GitHub Pages site for Kobo AIO: web/ + everything browser mode needs, all pinned by SHA-256.

    python3 tools/build_site.py [--out _site] [--skip-artifacts]

Layout of the output:
    index.html, catalog.json, aio-worker.js ...   copied from web/
    py/kobo_aio.zip                                the engine, run in the browser by Pyodide
    pyodide/                                       Pyodide runtime + Pillow + sqlite3 (self-hosted, verified)
    artifacts/<sha16>-<filename>                   mods/apps the browser installs (GitHub releases send no CORS)
"""

from __future__ import annotations

import argparse
import hashlib
import io
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

from kobo_aio.core import CACHE_DIR, Artifact, download  # noqa: E402

PYODIDE_VERSION = "0.27.7"
PYODIDE_CDN = f"https://cdn.jsdelivr.net/pyodide/v{PYODIDE_VERSION}/full"
# name -> (sha256, size). The two wheels match the hashes in pyodide-lock.json, which Pyodide checks again.
PYODIDE_FILES = {
    "pyodide.js": ("b4cb23a53aba19c221659b9fb40a2f18281d685691dc06647fb0afd0681baf98", 14913),
    "pyodide.asm.js": ("6b4c90de5b7172873f04f21884d0e9d2274e305fe32116558fe3e4fbe3618d51", 1255688),
    "pyodide.asm.wasm": ("a50dd1843f805a0b7c45b61037ee0d7b26dfe85efe0e18ef95a34ad24e401f5f", 10105545),
    "python_stdlib.zip": ("16611534726e5d8ac2bd8f926410b2dcb8d6f49aa24913463533b457a2115c16", 2360737),
    "pyodide-lock.json": ("9c45b916001a750f4102fc287494f3eab215909c7535c626a20db80fd6333e2c", 112205),
    "pillow-10.2.0-cp312-cp312-pyodide_2024_0_wasm32.whl":
        ("43eab147a9355385a99eeac18fb577d4af2fe4bd0bbd23d3ba3ddb92e72c2815", 987254),
    "sqlite3-1.0.0-cp312-cp312-pyodide_2024_0_wasm32.whl":
        ("43061928762b03482676a1e11dcef99a003148e59fe914b6a3e41180c382cfc3", 563076),
}


def web_artifacts() -> list[Artifact]:
    """Every pinned download a browser-mode module may fetch (engine-only modules are left out)."""
    from kobo_aio.modules import _util, apps, library
    arts = list(_util.ART.values()) + [a for a, _, _ in apps.KOPLUGINS.values()] + [library.SCREENSAVER]
    return list({a.sha256: a for a in arts}.values())


def fetch_pinned(url: str, sha: str, name: str) -> bytes:
    """Download once into the shared Kobo AIO cache, always re-verifying the SHA-256."""
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cached = CACHE_DIR / f"{sha[:16]}-{name}"
    if cached.is_file():
        data = cached.read_bytes()
        if hashlib.sha256(data).hexdigest() == sha:
            return data
    print(f"  descargando {name}", flush=True)
    data = download(url, timeout=600)
    got = hashlib.sha256(data).hexdigest()
    if got != sha:
        raise SystemExit(f"SHA-256 inválido para {name}: esperado {sha}, obtenido {got}")
    cached.write_bytes(data)
    return data


def engine_zip() -> bytes:
    pkg = REPO / "tools" / "kobo_aio"
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in sorted(pkg.rglob("*.py")):
            if "__pycache__" not in f.parts:
                zf.write(f, str(f.relative_to(pkg.parent)))
    return buf.getvalue()


def build(out: Path, skip_artifacts: bool = False) -> None:
    out = out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=f".{out.name}-staging-", dir=out.parent))
    try:
        shutil.copytree(REPO / "web", tmp, dirs_exist_ok=True)
        (tmp / "py").mkdir()
        (tmp / "py" / "kobo_aio.zip").write_bytes(engine_zip())
        print("Pyodide", PYODIDE_VERSION)
        (tmp / "pyodide").mkdir()
        for name, (sha, _size) in PYODIDE_FILES.items():
            (tmp / "pyodide" / name).write_bytes(fetch_pinned(f"{PYODIDE_CDN}/{name}", sha, f"pyodide-{PYODIDE_VERSION}-{name}"))
        (tmp / "artifacts").mkdir()
        if skip_artifacts:
            print("Artefactos omitidos (--skip-artifacts)")
        else:
            total = 0
            for art in web_artifacts():
                data = fetch_pinned(art.url, art.sha256, art.filename)
                (tmp / "artifacts" / f"{art.sha256[:16]}-{art.filename}").write_bytes(data)
                total += len(data)
            print(f"{len(web_artifacts())} artefactos, {total / 1024 ** 2:.0f} MB")

        backup = None
        if out.exists():
            backup = Path(tempfile.mkdtemp(prefix=f".{out.name}-old-", dir=out.parent))
            backup.rmdir()
            out.rename(backup)
        try:
            tmp.rename(out)
        except Exception:
            if backup is not None and backup.exists() and not out.exists():
                backup.rename(out)
            raise
        finally:
            if backup is not None and backup.exists():
                shutil.rmtree(backup, ignore_errors=True)
    finally:
        if tmp.exists():
            shutil.rmtree(tmp, ignore_errors=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=REPO / "_site")
    ap.add_argument("--skip-artifacts", action="store_true", help="solo la web, el motor y Pyodide")
    args = ap.parse_args()
    build(args.out, args.skip_artifacts)
    print(f"Sitio listo en {args.out}")


if __name__ == "__main__":
    main()
