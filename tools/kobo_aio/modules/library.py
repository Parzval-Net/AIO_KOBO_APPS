"""Library tools: comics/manga converter, EPUB -> KEPUB, dictionaries and sleep screens.

Everything here only ADDS new files (books, dictionaries, images); existing books are never overwritten.
"""

from __future__ import annotations

import io
import re
import platform
import subprocess
import tempfile
import zipfile
from pathlib import Path

from PIL import Image, ImageOps

from ..comics import PANELS, SUPPORTED_HELP, ComicOptions, convert, expand_inputs
from ..comics import safe_title
from ..core import AioError, Artifact, Module, Option, download, register
from ..txn import safe_component
from ._util import ONBOARD

KEPUBIFY = {
    "arm64": Artifact("https://github.com/pgaskin/kepubify/releases/download/v4.0.4/kepubify-darwin-arm64",
                      "6467d44439ee899113c7f710b509ef9e5ce65e8df711c85192b9ea5b683594b7", "kepubify-darwin-arm64"),
    "x86_64": Artifact("https://github.com/pgaskin/kepubify/releases/download/v4.0.4/kepubify-darwin-64bit",
                       "851afab0b83ecaf11f6965c901483eed3e74a6b41a3ab0a68f7321bc48bac4a3", "kepubify-darwin-64bit"),
}
SCREENSAVER = Artifact("https://github.com/redphx/nickel-screensaver/releases/download/v1.3.1/KoboRoot.tgz",
                       "4db0c61a31b2986aa692f5d02408716f5b2b4642b3597cceb5de765c36790d69", "KoboRoot.tgz", 27520)


def _files(opts, key="files") -> list[Path]:
    raw = opts.get(key) or []
    if isinstance(raw, str):
        raw = [x for x in raw.split(",") if x]
    paths = [Path(p).expanduser() for p in raw]
    missing = [str(p) for p in paths if not p.exists()]
    if missing:
        raise AioError(f"No existen: {', '.join(missing)}")
    return paths


def _dest_folder(opts, default: str) -> str:
    parts = [safe_component(x, "") for x in str(opts.get("folder") or default).split("/")]
    folder = "/".join(x for x in parts if x)
    if not folder:
        raise AioError("Carpeta de destino no válida")
    return folder


# -- Comics --------------------------------------------------------------------
def _comics_apply(mod, ctx, opts):
    files = expand_inputs(_files(opts))
    if not files:
        raise AioError(f"Selecciona al menos un cómic ({SUPPORTED_HELP})")
    folder = _dest_folder(opts, "Comics")
    copts = ComicOptions(manga=opts["manga"], spreads=opts["spreads"], crop=opts["crop"], color=opts["color"],
                         saturation=float(opts["saturation"]), gamma=float(opts["gamma"]),
                         upscale=opts["upscale"], output=opts["output"], use_kcc=opts["use_kcc"])
    for src in files:
        if ctx.txn.dry_run:
            ext = ".cbz" if copts.output == "cbz" else ".kepub.epub"
            ctx.txn.add_book(f"{folder}/{safe_component(safe_title(src), 'Comic')}{ext}", b"",
                             note=f"convertir {src.name} para {ctx.device.name}")
            continue
        name, data = convert(src, ctx.device.model_id, copts, ctx.progress)
        ctx.txn.add_book(f"{folder}/{safe_component(name, 'Comic.kepub.epub')}", data, note="cómic convertido")


register(Module(
    id="lib.comics", category="Biblioteca", title="Convertir cómics y manga para Kobo",
    description="Estilo KCC, ajustado a tu panel: recorta márgenes, divide páginas dobles, ajusta a la "
                "resolución exacta, conserva y realza el color en Kaleido 3 y genera KEPUB de página fija "
                "(paso de página rápido) o CBZ para KOReader.",
    risk="bajo", persistent=False,
    options=[
        Option("files", "Archivos o carpetas", "files", []),
        Option("folder", "Carpeta en el Kobo", "text", "Comics"),
        Option("output", "Formato", "choice", "kepub", [("kepub", "KEPUB página fija (Nickel)"),
                                                        ("cbz", "CBZ (KOReader)")]),
        Option("manga", "Manga (derecha a izquierda)", "bool", False),
        Option("spreads", "Páginas dobles", "choice", "split",
               [("split", "Dividir"), ("rotate", "Girar"), ("both", "Girar y dividir"), ("none", "Dejar")]),
        Option("crop", "Recortar márgenes", "bool", True),
        Option("color", "Conservar color (paneles a color)", "bool", True),
        Option("saturation", "Realce de color Kaleido", "choice", "1.25",
               [("1.0", "Sin realce"), ("1.15", "Suave"), ("1.25", "Medio"), ("1.4", "Fuerte")]),
        Option("gamma", "Gamma (contraste de medios tonos)", "choice", "1.0",
               [("1.0", "Normal"), ("1.4", "Más oscuro"), ("1.8", "Escaneos claros")]),
        Option("upscale", "Ampliar páginas pequeñas", "bool", False),
        Option("use_kcc", "Usar KCC si está instalado", "bool", False),
    ],
    sources=["https://github.com/ciromattia/kcc"], license="ISC (referencia)",
    apply_fn=_comics_apply,
))


# -- EPUB -> KEPUB -------------------------------------------------------------
def _kepubify_bin(ctx) -> Path:
    art = KEPUBIFY["arm64" if platform.machine() == "arm64" else "x86_64"]
    data = ctx.fetch(art)
    dst = Path.home() / "Kobo-AIO" / "bin" / art.filename
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists() or dst.read_bytes() != data:
        dst.write_bytes(data)
        dst.chmod(0o755)
        subprocess.run(["xattr", "-d", "com.apple.quarantine", str(dst)], capture_output=True)
    return dst


def _books_apply(mod, ctx, opts):
    files = _files(opts)
    if not files:
        raise AioError("Selecciona al menos un libro (EPUB, KEPUB, PDF o CBZ)")
    folder = _dest_folder(opts, "Libros")
    tool = _kepubify_bin(ctx) if opts["kepubify"] else None
    for src in files:
        books = sorted(src.rglob("*")) if src.is_dir() else [src]
        for b in books:
            low = b.name.lower()
            if not b.is_file() or b.name.startswith(".") or not low.endswith((".epub", ".pdf", ".cbz")):
                continue
            if ctx.txn.dry_run:
                to_kepub = tool and low.endswith(".epub") and not low.endswith(".kepub.epub")
                name = (b.name[:-5] + ".kepub.epub") if to_kepub else b.name
                ctx.txn.add_book(f"{folder}/{safe_component(name, 'libro.epub')}", b"",
                                 note="convertir a KEPUB" if to_kepub else "copiar")
                continue
            if tool and low.endswith(".epub") and not low.endswith(".kepub.epub"):
                with tempfile.TemporaryDirectory() as tmp:
                    cmd = [str(tool), "-o", tmp]
                    if opts["smart_punct"]:
                        cmd.append("--smarten-punctuation")
                    subprocess.run(cmd + [str(b)], check=True, capture_output=True)
                    out = next(Path(tmp).glob("*.kepub.epub"))
                    ctx.txn.add_book(f"{folder}/{safe_component(out.name, 'libro.kepub.epub')}", out.read_bytes(),
                                     note="EPUB convertido a KEPUB")
            else:
                ctx.txn.add_book(f"{folder}/{safe_component(b.name, 'libro.epub')}", b.read_bytes())


register(Module(
    id="lib.books", category="Biblioteca", title="Enviar libros (EPUB a KEPUB)",
    description="Copia libros al Kobo sin pisar los existentes. Convierte EPUB a KEPUB con kepubify: abre más "
                "rápido, muestra páginas por capítulo, estadísticas y mejor tipografía en Nickel.",
    risk="bajo", persistent=False,
    options=[Option("files", "Libros o carpetas", "files", []),
             Option("folder", "Carpeta en el Kobo", "text", "Libros"),
             Option("kepubify", "Convertir EPUB a KEPUB", "bool", True),
             Option("smart_punct", "Comillas y guiones tipográficos", "bool", False)],
    sources=["https://github.com/pgaskin/kepubify"], license="MIT",
    apply_fn=_books_apply,
))


# -- Dictionaries --------------------------------------------------------------
# Wiktionary-based Kobo dictionaries by reader-dict.com (BoboTiG/ebook-reader-dict, MIT code; content from
# Wiktionary, CC BY-SA). Updated about twice a month, so they are validated structurally instead of pinned.
DICTS = {
    "es-es": ("https://www.reader-dict.com/file/es/dicthtml-es-es.zip", "Español (Wiktionary, ~940 mil entradas, 14 MB)"),
    "es-es-noetym": ("https://www.reader-dict.com/file/es/dicthtml-es-es-noetym.zip", "Español sin etimologías (13 MB)"),
    "en-en": ("https://www.reader-dict.com/file/en/dicthtml-en-en.zip", "English (Wiktionary, 66 MB)"),
    "en-en-noetym": ("https://www.reader-dict.com/file/en/dicthtml-en-en-noetym.zip", "English sin etimologías (54 MB)"),
}


def _validate_dictzip(data: bytes, name: str) -> None:
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            names = zf.namelist()
    except zipfile.BadZipFile as exc:
        raise AioError(f"{name} no es un diccionario Kobo válido") from exc
    if "words" not in names or any("/" in n or n.startswith("..") for n in names):
        raise AioError(f"{name} no tiene la estructura de un dicthtml de Kobo (falta 'words')")


def _dict_apply(mod, ctx, opts):
    targets: list[tuple[str, bytes]] = []
    for key in opts["wiktionary"]:
        url, _ = DICTS[key]
        fname = url.rsplit("/", 1)[1].replace("-noetym", "")
        if ctx.txn.dry_run:
            ctx.txn.note(f"Descargar e instalar {fname} desde reader-dict.com")
            continue
        ctx.progress(f"Descargando diccionario {fname}...")
        data = download(url, timeout=600)
        _validate_dictzip(data, fname)
        targets.append((fname, data))
    for p in _files(opts):
        if not p.name.startswith("dicthtml") or p.suffix != ".zip":
            raise AioError(f"{p.name}: el nombre debe ser dicthtml-xx.zip o dicthtml-xx-yy.zip")
        data = p.read_bytes()
        _validate_dictzip(data, p.name)
        targets.append((p.name, data))
    mine = ctx.state.setdefault("dictionaries", [])
    for fname, data in targets:
        fname = safe_component(fname, "dicthtml.zip")
        ctx.txn.write_bytes(f".kobo/custom-dict/{fname}", data, note="diccionario personalizado")
        if fname not in mine:
            mine.append(fname)
    if targets:
        ctx.txn.note("Reinicia el Kobo y actívalos en Ajustes > Idioma y diccionarios (aparecen como «Custom»).")


def _dict_uninstall(mod, ctx):
    # Only dictionaries installed through the AIO; anything the user copied by hand stays.
    for fname in ctx.state.get("dictionaries", []):
        ctx.txn.delete(f".kobo/custom-dict/{fname}")
    ctx.state["dictionaries"] = []


register(Module(
    id="lib.dictionaries", category="Biblioteca", title="Diccionarios",
    description="Instala diccionarios Wiktionary listos para Kobo o tus propios dicthtml-*.zip en "
                ".kobo/custom-dict (no los pisa la sincronización). Los oficiales es, es-en y en-es se activan "
                "gratis desde el Kobo con WiFi.",
    risk="bajo", persistent=False, min_fw=(4, 24),
    options=[Option("wiktionary", "Wiktionary", "multi", ["es-es"], [(k, v[1]) for k, v in DICTS.items()]),
             Option("files", "Mis diccionarios (dicthtml-*.zip)", "files", [])],
    sources=["https://www.reader-dict.com", "https://github.com/BoboTiG/ebook-reader-dict",
             "https://pgaskin.net/dictutil/"], license="CC BY-SA (contenido Wiktionary)",
    apply_fn=_dict_apply, uninstall_fn=_dict_uninstall,
))


# -- Sleep screens -------------------------------------------------------------
def _sleep_image(path: Path, W: int, H: int, color: bool, fit: str) -> bytes:
    img = ImageOps.exif_transpose(Image.open(path))
    img = img.convert("RGB") if color else img.convert("L")
    if fit == "cover":
        img = ImageOps.fit(img, (W, H), Image.LANCZOS)
    else:
        img = ImageOps.pad(img, (W, H), Image.LANCZOS, color="white")
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def _sleep_apply(mod, ctx, opts):
    W, H, color, _ = PANELS.get(ctx.device.model_id, (1264, 1680, False, ""))
    for i, p in enumerate(_files(opts), 1):
        if p.suffix.lower() not in (".png", ".jpg", ".jpeg", ".webp"):
            continue
        rel = f".kobo/screensaver/aio-{i:02d}-{safe_component(p.stem, 'imagen')[:40]}.png"
        if ctx.txn.dry_run:
            ctx.txn.note(f"Preparar {p.name} a {W}x{H}")
            continue
        ctx.txn.write_bytes(rel, _sleep_image(p, W, H, color, opts["fit"]), note="pantalla de reposo")
    if opts["overlay"]:
        data = ctx.fetch(SCREENSAVER)
        ctx.add_koboroot("nickel-screensaver", data)
        ctx.txn.write_text(".adds/nm/kobo-aio-screensaver",
                           f"menu_item :main :Fondo de reposo on/off :cmd_output :500:/bin/sh "
                           f"{ONBOARD}/.adds/kobo-aio/scripts/toggle_screensaver.sh\n")
        ctx.txn.write_text(".adds/kobo-aio/scripts/toggle_screensaver.sh", (
            "#!/bin/sh\nS=/mnt/onboard/.kobo/screensaver; D=/mnt/onboard/.adds/kobo-aio/screensaver-off\n"
            'if [ -d "$S" ]; then mkdir -p "$(dirname $D)"; mv "$S" "$D" && echo "Fondo: portada del libro";\n'
            'elif [ -d "$D" ]; then mv "$D" "$S" && echo "Fondo: imágenes propias"; else echo "Sin imágenes"; fi\n'))
    ctx.txn.note("Activa Ajustes > Ahorro de energía > «Mostrar portadas a pantalla completa» para usarlas.")


def _sleep_uninstall(mod, ctx):
    root = ctx.device.root / ".kobo" / "screensaver"
    if root.is_dir():
        for f in root.glob("aio-*.png"):
            ctx.txn.delete(f".kobo/screensaver/{f.name}")
    ctx.txn.write_text(".adds/screensaver/uninstall.txt", "", note="desinstala nickel-screensaver")
    ctx.txn.delete(".adds/nm/kobo-aio-screensaver")


register(Module(
    id="lib.sleep", category="Personalización", title="Pantallas de reposo propias",
    description="Convierte tus imágenes a la resolución exacta del panel (color en Kaleido 3). Opcional: "
                "nickel-screensaver muestra el reposo transparente sobre la página, estilo KOReader.",
    risk="bajo",
    options=[Option("files", "Imágenes", "files", []),
             Option("fit", "Ajuste", "choice", "cover", [("cover", "Llenar pantalla"), ("contain", "Imagen completa")]),
             Option("overlay", "Instalar nickel-screensaver 1.3.1 (reposo transparente)", "bool", False)],
    sources=["https://github.com/redphx/nickel-screensaver"], license="MIT", max_fw=(5,),
    apply_fn=_sleep_apply, uninstall_fn=_sleep_uninstall,
))


# -- Audiobooks ----------------------------------------------------------------
AUDIOBOOK_PLUGIN = Artifact(
    "https://github.com/stradichenko/audiobook.koplugin/releases/download/v0.2.23/audiobook-koplugin-v0.2.23.zip",
    "c8b6bb732b6ca15ec23f3b9730a71ad0511c70f87cb7173f4fa771e0e953fbd3", "audiobook-koplugin-v0.2.23.zip", 173328749)


def _abk_plugin_apply(mod, ctx, opts):
    data = ctx.fetch(AUDIOBOOK_PLUGIN)
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        metas = [n for n in zf.namelist() if n.endswith("audiobook.koplugin/_meta.lua") or n == "_meta.lua"]
        if not metas:
            raise AioError("El paquete del plugin no contiene audiobook.koplugin/_meta.lua")
        prefix = metas[0][: -len("_meta.lua")]
        n = 0
        for info in zf.infolist():
            if info.is_dir() or not info.filename.startswith(prefix):
                continue
            rel = info.filename[len(prefix):]
            if not rel or ".." in rel.split("/"):
                continue
            ctx.txn.write_bytes(f".adds/koreader/plugins/audiobook.koplugin/{rel}", zf.read(info),
                                mode=(info.external_attr >> 16) & 0o777 or None)
            n += 1
    ctx.txn.note(f"audiobook.koplugin: {n} archivos. En KOReader: menú Herramientas > Audiobook; empareja tus "
                 "audífonos Bluetooth desde el propio plugin.")


def _abk_plugin_uninstall(mod, ctx):
    from ._util import remove_tree_files
    remove_tree_files(ctx, ".adds/koreader/plugins/audiobook.koplugin")


register(Module(
    id="app.audiobook", category="Apps", title="Audiolibros en KOReader (audiobook.koplugin 0.2.23)",
    description="Reproductor de audiolibros y música para KOReader: MP3/M4B con capítulos, portada, temporizador, "
                "Bluetooth propio (compatible con la pila MediaTek de la Libra Colour) y lectura sincronizada. "
                "Paquete de 173 MB (incluye voces TTS y ffmpeg).",
    risk="medio", requires=["app.koreader"], needs_bluetooth=True,
    sources=["https://github.com/stradichenko/audiobook.koplugin"], license="AGPL-3.0",
    apply_fn=_abk_plugin_apply, uninstall_fn=_abk_plugin_uninstall,
    status_fn=lambda m, d: "instalado" if (d.root / ".adds/koreader/plugins/audiobook.koplugin").is_dir() else "no instalado",
))


def _abk_apply(mod, ctx, opts):
    from ..audiobooks import build, build_mp3z, collect
    files = _files(opts)
    if not files:
        raise AioError("Selecciona una carpeta con las pistas del audiolibro o un archivo MP3/M4B")
    cover = _files(opts, "cover")
    for src in files:
        book = collect(src, opts.get("title") if len(files) == 1 else "", opts.get("author") or "",
                       cover[0] if cover else None)
        author = safe_component(book.author, "Sin autor")
        title_dir = safe_component(book.title, "Audiolibro")
        book.title = title_dir
        if ctx.txn.dry_run:
            ext = ".mp3z" if opts["target"] == "nickel" else ".mp3"
            ctx.txn.add_book(f"Audiobooks/{author}/{title_dir}/{title_dir}{ext}", b"",
                             note=f"{len(book.tracks)} pistas, portada: {'sí' if book.cover else 'de las pistas'}")
            continue
        if opts["target"] == "nickel":
            name, data, _ = build_mp3z(book, ctx.progress, int(opts.get("part_minutes") or 0))
        else:
            name, data, _ = build(book, ctx.progress)
        ctx.txn.add_book(f"Audiobooks/{author}/{title_dir}/{safe_component(name, 'audiolibro.mp3')}", data,
                         note="audiolibro")
    if opts["target"] == "nickel":
        ctx.txn.note("Nickel lo importará como audiolibro al expulsar (pestaña Audiolibros). Empareja tus audífonos "
                     "en Ajustes > Bluetooth. El lector stock retoma desde el inicio de la parte en que ibas; por eso "
                     "las pistas largas se dividen. Si no aparece o no suena, usa el formato KOReader.")
    else:
        ctx.txn.note("Ábrelo en KOReader con el plugin Audiobook.")


register(Module(
    id="lib.audiobooks", category="Biblioteca", title="Subir audiolibros (con capítulos y portada)",
    description="Prepara tus audiolibros para escucharlos por Bluetooth. Stock: formato .mp3z que el propio lector "
                "reproduce, con portada en el reproductor y partes cortas para retomar cerca de donde ibas. "
                "Alternativa: MP3 con capítulos y posición exacta en KOReader. La portada sale de la carpeta "
                "(cover/folder.jpg), de las pistas o la que subas. Solo audio sin DRM propio.",
    risk="bajo", persistent=False, needs_bluetooth=True,
    options=[Option("target", "Reproducir con", "choice", "nickel",
                    [("nickel", "Lector stock del Kobo (.mp3z)"),
                     ("koreader", "KOReader + audiobook.koplugin (MP3 con capítulos)")]),
             Option("part_minutes", "Largo de cada parte (solo lector stock)", "choice", "10",
                    [("5", "5 min (retoma más cerca)"), ("10", "10 min (recomendado)"), ("20", "20 min"),
                     ("0", "Sin dividir (una parte por pista)")],
                    help="El lector stock guarda en qué parte ibas, no el minuto exacto."),
             Option("files", "Carpeta(s) del audiolibro o archivos", "files", []),
             Option("cover", "Portada (opcional)", "files", []),
             Option("title", "Título (opcional)", "text", ""),
             Option("author", "Autor (opcional)", "text", "")],
    sources=["https://github.com/stradichenko/audiobook.koplugin"], license="AGPL-3.0 (plugin)",
    apply_fn=_abk_apply,
))
