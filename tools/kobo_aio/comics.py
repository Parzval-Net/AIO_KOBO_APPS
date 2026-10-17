"""Comic/manga converter tuned for Kobo e-ink (KCC-style pipeline, pure Pillow).

Pipeline per page (modelled on KCC's image.py/comic2ebook.py, ISC, github.com/ciromattia/kcc):
  1. read CBZ/ZIP, CBR/RAR/7Z/CB7 (via `unar` or `7z`), PDF (via PyMuPDF if installed) or an image folder
  2. trim uniform margins, 3. split / rotate double-page spreads (RTL order for manga),
  4. fit to the panel resolution (LANCZOS, no upscaling unless asked),
  5. grayscale panels: autocontrast + gamma + 16-level quantization (Carta/Kaleido B/W palette);
     colour panels (Kaleido 3): keep RGB, optional saturation boost (Kaleido washes colours out),
  6. package as fixed-layout KEPUB (fast page turns in Nickel) or CBZ (for KOReader).
If KCC is installed (kcc-c2e), `use_kcc` delegates to it with the matching Kobo profile.
"""

from __future__ import annotations

import io
import os
import re
import shutil
import subprocess
import tempfile
import uuid
import zipfile
from dataclasses import dataclass
from html import escape
from pathlib import Path

from PIL import Image, ImageEnhance, ImageOps

from .core import host_os, install_hint

IMG_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".tif", ".tiff"}
ARCHIVE_EXTS = {".cbz", ".zip", ".cbr", ".rar", ".cb7", ".7z", ".cbt", ".tar", ".pdf", ".epub"}
SUPPORTED_HELP = "CBZ, ZIP, CBR, RAR, CB7, 7Z, CBT, TAR, PDF, EPUB, imágenes sueltas o carpetas"

# KCC profile per Kobo hardware id (README "Profiles"); panel size and colour come from device.KOBO_MODELS.
KCC_PROFILES = {"390": "KoLC", "393": "KoCC", "376": "KoC", "386": "KoC", "391": "KoC", "395": "KoC", "384": "KoL",
                "388": "KoL", "377": "KoF", "380": "KoF", "383": "KoS", "387": "KoE", "389": "KoE", "382": "KoN",
                "371": "KoGHD", "373": "KoAO", "381": "KoAO", "370": "KoAH2O", "350": "KoAHD", "360": "KoA",
                "330": "KoG", "310": "KoMT", "320": "KoMT", "340": "KoMT"}


def _panel(model_id: str):
    from .device import KOBO_MODELS
    m = KOBO_MODELS.get(model_id) or KOBO_MODELS["390"]
    return m["width"], m["height"], m["color"], KCC_PROFILES.get(model_id, "KoL")


class _Panels(dict):
    def get(self, key, default=None):  # backwards compatible PANELS.get(model_id, fallback)
        from .device import KOBO_MODELS
        return _panel(key) if key in KOBO_MODELS else default


PANELS = _Panels()
GRAY16 = [i * 17 for i in range(16)]


@dataclass
class ComicOptions:
    manga: bool = False            # right-to-left reading
    spreads: str = "split"         # split | rotate | both | none
    crop: bool = True
    color: bool = True             # keep colour on colour panels
    saturation: float = 1.25       # Kaleido 3 colour boost (1.0 = none)
    gamma: float = 1.0             # >1 darkens mid-tones (helps faint scans on e-ink)
    upscale: bool = False
    quality: int = 85
    output: str = "kepub"          # kepub | cbz
    use_kcc: bool = False


def natural_key(s: str):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]


def _skip(name: str) -> bool:
    parts = Path(name).parts
    return any(p.startswith((".", "__MACOSX")) for p in parts) or Path(name).suffix.lower() not in IMG_EXTS


def _epub_pages(zf: zipfile.ZipFile) -> list[tuple[str, bytes]]:
    """Images of a (comic) EPUB in reading order: OPF spine -> xhtml pages -> <img>/<image> sources."""
    import posixpath
    import xml.etree.ElementTree as ET
    names = set(zf.namelist())
    try:
        container = ET.fromstring(zf.read("META-INF/container.xml"))
        opf_path = next(e.get("full-path") for e in container.iter() if e.tag.endswith("rootfile"))
        opf = ET.fromstring(zf.read(opf_path))
    except Exception:  # noqa: BLE001 - malformed EPUB: fall back to natural order of images
        imgs = sorted((n for n in names if not _skip(n)), key=natural_key)
        return [(n, zf.read(n)) for n in imgs]
    base = posixpath.dirname(opf_path)
    items = {e.get("id"): e.get("href") for e in opf.iter() if e.tag.endswith("item")}
    order, seen = [], set()
    for ref in (e for e in opf.iter() if e.tag.endswith("itemref")):
        href = items.get(ref.get("idref"))
        if not href:
            continue
        page = posixpath.normpath(posixpath.join(base, href))
        if page not in names:
            continue
        html = zf.read(page).decode("utf-8", errors="replace")
        for m in re.finditer(r'(?:src|xlink:href|href)\s*=\s*["\']([^"\']+\.(?:jpe?g|png|webp|gif|bmp))["\']', html, re.I):
            img = posixpath.normpath(posixpath.join(posixpath.dirname(page), m.group(1)))
            if img in names and img not in seen:
                seen.add(img)
                order.append(img)
    if not order:
        order = sorted((n for n in names if not _skip(n)), key=natural_key)
    return [(n, zf.read(n)) for n in order]


def archive_tool() -> str | None:
    """unar or 7-Zip on PATH, or 7-Zip in its default Windows folder."""
    found = shutil.which("unar") or shutil.which("7z") or shutil.which("7zz")
    if not found and host_os() == "windows":
        for env in ("ProgramFiles", "ProgramFiles(x86)"):
            exe = Path(os.environ.get(env, "")) / "7-Zip" / "7z.exe"
            if os.environ.get(env) and exe.is_file():
                return str(exe)
    return found


def read_pages(src: Path) -> list[tuple[str, bytes]]:
    src = Path(src)
    if src.is_file() and src.suffix.lower() in IMG_EXTS:
        return [(src.name, src.read_bytes())]
    if src.is_dir():
        files = [p for p in src.rglob("*") if p.is_file() and not _skip(str(p.relative_to(src)))]
        return [(str(p.relative_to(src)), p.read_bytes()) for p in sorted(files, key=lambda p: natural_key(str(p)))]
    ext = src.suffix.lower()
    if ext == ".epub":
        with zipfile.ZipFile(src) as zf:
            return _epub_pages(zf)
    if ext in (".cbt", ".tar"):
        import tarfile
        with tarfile.open(src) as tf:
            members = sorted((m for m in tf.getmembers() if m.isfile() and not _skip(m.name)),
                             key=lambda m: natural_key(m.name))
            return [(m.name, tf.extractfile(m).read()) for m in members]
    if ext in (".cbz", ".zip"):
        with zipfile.ZipFile(src) as zf:
            names = sorted((n for n in zf.namelist() if not n.endswith("/") and not _skip(n)), key=natural_key)
            return [(n, zf.read(n)) for n in names]
    if ext == ".pdf":
        try:
            import fitz  # PyMuPDF
        except ImportError as exc:
            raise RuntimeError(f"Para PDF hace falta PyMuPDF (ya viene en la app de escritorio): {install_hint('pymupdf')}") from exc
        doc = fitz.open(src)
        pages = []
        for i, page in enumerate(doc):
            pix = page.get_pixmap(dpi=200)
            pages.append((f"{i:04d}.png", pix.tobytes("png")))
        return pages
    if ext in (".cbr", ".rar", ".cb7", ".7z"):
        tool = archive_tool()
        if not tool:
            raise RuntimeError(f"Para abrir CBR, RAR, CB7 o 7Z hace falta un descompresor: {install_hint('unar')}")
        with tempfile.TemporaryDirectory() as tmp:
            cmd = [tool, "-q", "-o", tmp, str(src)] if Path(tool).stem == "unar" else [tool, "x", "-y", f"-o{tmp}", str(src)]
            subprocess.run(cmd, check=True, capture_output=True)
            return read_pages(Path(tmp))
    raise RuntimeError(f"Formato no soportado: {src.name}. Acepta {SUPPORTED_HELP}.")


def expand_inputs(paths: list[Path]) -> list[Path]:
    """A folder full of comic files is a batch; a folder of images is one comic."""
    out: list[Path] = []
    for p in paths:
        p = Path(p)
        if p.is_dir():
            comics = sorted((c for c in p.iterdir() if c.is_file() and c.suffix.lower() in ARCHIVE_EXTS
                             and not c.name.startswith(".")), key=lambda c: natural_key(c.name))
            out.extend(comics if comics else [p])
        else:
            out.append(p)
    return out


def _is_color(img: Image.Image) -> bool:
    """True when the page has meaningful chroma (not a grayscale scan saved as RGB)."""
    small = img.convert("RGB").resize((64, 64))
    ycc = small.convert("YCbCr")
    _, cb, cr = ycc.split()
    spread = max(cb.getextrema()[1] - cb.getextrema()[0], cr.getextrema()[1] - cr.getextrema()[0])
    return spread > 24


def _trim(img: Image.Image) -> Image.Image:
    """Crop uniform borders (white or black) keeping a 1% safety margin."""
    gray = ImageOps.autocontrast(img.convert("L"), cutoff=1)
    w, h = gray.size
    corners = [gray.getpixel(p) for p in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1))]
    bg = 255 if sum(corners) / 4 > 127 else 0
    mask = gray.point(lambda v: 255 if abs(v - bg) > 40 else 0)
    box = mask.getbbox()
    if not box:
        return img
    l, t, r, b = box
    if (r - l) < w * 0.5 or (b - t) < h * 0.5:
        return img  # mostly empty page (title/blank): keep as-is
    m = int(max(w, h) * 0.01)
    return img.crop((max(0, l - m), max(0, t - m), min(w, r + m), min(h, b + m)))


def _fit(img: Image.Image, W: int, H: int, upscale: bool) -> Image.Image:
    w, h = img.size
    scale = min(W / w, H / h)
    if scale >= 1 and not upscale:
        return img
    return img.resize((max(1, round(w * scale)), max(1, round(h * scale))),
                      Image.LANCZOS if scale < 1 else Image.BICUBIC)


def _tone(img: Image.Image, color_panel: bool, opts: ComicOptions) -> tuple[Image.Image, bool]:
    keep_color = color_panel and opts.color and _is_color(img)
    if keep_color:
        out = img.convert("RGB")
        if opts.saturation and abs(opts.saturation - 1.0) > 0.01:
            out = ImageEnhance.Color(out).enhance(opts.saturation)
        if abs(opts.gamma - 1.0) > 0.01:
            inv = 1.0 / opts.gamma
            out = out.point([round(255 * ((i / 255) ** (1 / inv))) for i in range(256)] * 3)
        return out, True
    g = ImageOps.autocontrast(img.convert("L"), cutoff=1, preserve_tone=True)
    if abs(opts.gamma - 1.0) > 0.01:
        g = g.point([round(255 * ((i / 255) ** opts.gamma)) for i in range(256)])
    # 16-level quantization matches the panel's gray palette and shrinks JPEGs
    g = g.point([GRAY16[min(15, round(i / 17))] for i in range(256)])
    return g, False


def process_pages(pages: list[tuple[str, bytes]], W: int, H: int, color_panel: bool, opts: ComicOptions,
                  progress=lambda m: None) -> list[tuple[bytes, int, int]]:
    out: list[tuple[bytes, int, int]] = []
    for idx, (name, data) in enumerate(pages):
        try:
            img = ImageOps.exif_transpose(Image.open(io.BytesIO(data)))
            img.load()
        except Exception:  # noqa: BLE001 - skip unreadable images, keep converting
            progress(f"Página ignorada (imagen ilegible): {name}")
            continue
        if img.mode not in ("RGB", "L"):
            img = img.convert("RGB")
        if opts.crop:
            img = _trim(img)
        parts: list[Image.Image]
        w, h = img.size
        if w > h * 1.05 and opts.spreads != "none":
            left, right = img.crop((0, 0, w // 2, h)), img.crop((w // 2, 0, w, h))
            halves = [right, left] if opts.manga else [left, right]
            rotated = img.rotate(90 if opts.manga else -90, expand=True)
            parts = {"split": halves, "rotate": [rotated], "both": [rotated] + halves}[opts.spreads]
        else:
            parts = [img]
        for part in parts:
            part = _fit(part, W, H, opts.upscale)
            toned, is_rgb = _tone(part, color_panel, opts)
            buf = io.BytesIO()
            save_kw = {"quality": opts.quality, "optimize": True}
            if is_rgb:
                save_kw["subsampling"] = 0  # 4:4:4 keeps colour edges sharp on Kaleido's 2x2 CFA
            toned.save(buf, "JPEG", **save_kw)
            out.append((buf.getvalue(), toned.size[0], toned.size[1]))
        if idx % 10 == 0:
            progress(f"Procesando página {idx + 1}/{len(pages)}")
    return out


def build_kepub(images: list[tuple[bytes, int, int]], title: str, W: int, H: int, rtl: bool) -> bytes:
    """EPUB3 fixed-layout (pre-paginated) packaged as .kepub.epub for Nickel."""
    book_id = f"urn:uuid:{uuid.uuid4()}"
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml",
                   '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
                   '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
                   "</rootfiles></container>")
        manifest, spine, nav = [], [], []
        for i, (jpg, w, h) in enumerate(images, 1):
            z.writestr(f"OEBPS/Images/p{i:04d}.jpg", jpg, compress_type=zipfile.ZIP_STORED)
            # Center the page on the panel canvas
            z.writestr(f"OEBPS/Text/p{i:04d}.xhtml", (
                '<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE html>'
                '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">'
                f'<head><title>{i}</title><meta name="viewport" content="width={W}, height={H}"/>'
                "<style>html,body{margin:0;padding:0;width:100%;height:100%;background:#fff}"
                "div{width:100%;height:100%;display:flex;align-items:center;justify-content:center}"
                "img{max-width:100%;max-height:100%}</style></head>"
                f'<body><div><img src="../Images/p{i:04d}.jpg" width="{w}" height="{h}" alt=""/></div></body></html>'))
            cover_prop = ' properties="cover-image"' if i == 1 else ""
            manifest.append(f'<item id="i{i}" href="Images/p{i:04d}.jpg" media-type="image/jpeg"{cover_prop}/>')
            manifest.append(f'<item id="p{i}" href="Text/p{i:04d}.xhtml" media-type="application/xhtml+xml"/>')
            spine.append(f'<itemref idref="p{i}"/>')
            if i == 1 or i % 20 == 0:
                nav.append(f'<li><a href="Text/p{i:04d}.xhtml">Página {i}</a></li>')
        z.writestr("OEBPS/nav.xhtml", (
            '<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE html><html xmlns="http://www.w3.org/1999/xhtml" '
            'xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Índice</title></head><body>'
            f'<nav epub:type="toc"><ol>{"".join(nav)}</ol></nav></body></html>'))
        rtl_attr = ' page-progression-direction="rtl"' if rtl else ""
        z.writestr("OEBPS/content.opf", (
            '<?xml version="1.0" encoding="UTF-8"?>'
            '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">'
            '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
            f'<dc:identifier id="bookid">{book_id}</dc:identifier><dc:title>{escape(title)}</dc:title>'
            '<dc:language>es</dc:language><meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>'
            '<meta property="rendition:layout">pre-paginated</meta><meta property="rendition:spread">none</meta>'
            '<meta property="rendition:orientation">portrait</meta>'
            f'<meta name="original-resolution" content="{W}x{H}"/><meta name="cover" content="i1"/></metadata>'
            f'<manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
            f'{"".join(manifest)}</manifest>'
            f'<spine{rtl_attr}>{"".join(spine)}</spine></package>'))
    return buf.getvalue()


def build_cbz(images: list[tuple[bytes, int, int]]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for i, (jpg, _, _) in enumerate(images, 1):
            z.writestr(f"{i:04d}.jpg", jpg, compress_type=zipfile.ZIP_STORED)
    return buf.getvalue()


def safe_title(src: Path) -> str:
    name = src.name if src.is_dir() else src.stem
    return re.sub(r"[\\/:*?\"<>|]+", " ", name).strip() or "Comic"


def convert(src: Path, model_id: str, opts: ComicOptions, progress=lambda m: None) -> tuple[str, bytes]:
    """Return (output filename, file bytes)."""
    W, H, color_panel, kcc_profile = PANELS.get(model_id, (1264, 1680, False, "KoL"))
    title = safe_title(Path(src))
    if opts.use_kcc and shutil.which("kcc-c2e"):
        return _convert_kcc(Path(src), kcc_profile, color_panel, opts, title)
    pages = read_pages(Path(src))
    if not pages:
        raise RuntimeError(f"No se encontraron imágenes en {Path(src).name}")
    progress(f"{title}: {len(pages)} páginas, panel {W}x{H}{' color' if color_panel else ''}")
    images = process_pages(pages, W, H, color_panel, opts, progress)
    if opts.output == "cbz":
        return f"{title}.cbz", build_cbz(images)
    return f"{title}.kepub.epub", build_kepub(images, title, W, H, opts.manga)


def _convert_kcc(src: Path, profile: str, color_panel: bool, opts: ComicOptions, title: str) -> tuple[str, bytes]:
    with tempfile.TemporaryDirectory() as out:
        cmd = ["kcc-c2e", "-p", profile, "-f", "CBZ" if opts.output == "cbz" else "EPUB", "-o", out,
               "-r", {"split": "0", "rotate": "1", "both": "2"}.get(opts.spreads, "0")]
        if opts.manga:
            cmd.append("-m")
        if color_panel and opts.color:
            cmd.append("--forcecolor")  # the CLI does not enable colour for KoLC/KoCC by itself
        if not opts.crop:
            cmd += ["-c", "0"]
        if abs(opts.gamma - 1.0) > 0.01:
            cmd += ["-g", str(opts.gamma)]
        cmd.append(str(src))
        subprocess.run(cmd, check=True, capture_output=True)
        produced = sorted(Path(out).iterdir(), key=lambda p: p.stat().st_size, reverse=True)
        if not produced:
            raise RuntimeError("KCC no generó salida")
        ext = ".cbz" if opts.output == "cbz" else ".kepub.epub"
        return f"{title}{ext}", produced[0].read_bytes()
