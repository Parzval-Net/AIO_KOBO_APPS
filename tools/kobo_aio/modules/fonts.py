"""E-reader font packs (ebook-fonts by Nico Verbruggen; each family keeps its own license, mostly SIL OFL)."""

from __future__ import annotations

import io
import zipfile

from ..core import Module, Option, register
from ._util import ART, conf_edit

CORE = ["Bitter", "Cartisse", "Charis", "Garamond", "Jost", "Legible Next", "Libron", "Palatium", "Sourcerer"]
EXTRA = ["Adelph", "Alegreya", "Alizar", "Ancizar Sans", "Ancizar Serif", "Andada", "Basker", "Cardo", "Castoro",
         "Charis Literacy", "Charis Old Style", "Clara", "Disleksio", "Elstob", "Erewhon", "Gentium", "Georsio",
         "Halcyon", "Ibarra", "Junius", "Kierkegaard", "Libertinus", "Literata", "Membo", "Newsreader", "NinePoint",
         "Publica", "Publica Wide", "Readerly", "Sable", "Scarlet", "Tabula", "Technical", "Yrsa", "Zilla Slab"]


def _prefix(family: str) -> str:
    return "KF_" + family.replace(" ", "_") + "-"


def _install(ctx, art_key: str, families: list[str]) -> int:
    if not families:
        return 0
    data = ctx.fetch(ART[art_key])
    n = 0
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        names = [i.filename for i in zf.infolist() if i.filename.endswith((".ttf", ".otf")) and "/" not in i.filename]
        for fam in families:
            for name in names:
                if name.startswith(_prefix(fam)):
                    ctx.txn.write_bytes(f"fonts/{name}", zf.read(name), note=f"fuente KF {fam}")
                    n += 1
    return n


def _fonts_apply(mod, ctx, opts):
    n = _install(ctx, "fonts-core", list(opts["core"]))
    n += _install(ctx, "fonts-extra", list(opts["extra"]))
    ctx.txn.note(f"{n} archivos de fuentes instalados en fonts/")
    if opts["default_font"] != "none":
        conf_edit(ctx, sets=[("Reading", "readingFontFamily", f"KF {opts['default_font']}")],
                  note="fuente de lectura por defecto")


def _fonts_uninstall(mod, ctx):
    root = ctx.device.root / "fonts"
    if root.is_dir():
        for f in root.iterdir():
            if f.is_file() and f.name.startswith("KF_") and any(f.name.startswith(_prefix(x)) for x in CORE + EXTRA):
                ctx.txn.delete(f"fonts/{f.name}")


def _fonts_status(mod, dev):
    root = dev.root / "fonts"
    fams = {f.name.split("-")[0] for f in root.glob("KF_*.ttf")} if root.is_dir() else set()
    return f"{len(fams)} familias KF instaladas" if fams else "no instalado"


register(Module(
    id="fonts.ebook", category="Lectura", title="Fuentes para e-reader (ebook-fonts)",
    description="Familias optimizadas para e-ink (prefijo «KF»): Libron, Charis, Literata, Readerly, Disleksio "
                "(dislexia) y más. Aparecen en el menú de fuentes del lector.",
    risk="bajo", recommended=True,
    options=[
        Option("core", "Colección principal", "multi", list(CORE), [(f, f) for f in CORE]),
        Option("extra", "Colección extra (22 MB)", "multi", [], [(f, f) for f in EXTRA]),
        Option("default_font", "Fuente de lectura por defecto", "choice", "none",
               [("none", "No cambiar")] + [(f, f"KF {f}") for f in CORE + EXTRA]),
    ],
    sources=["https://github.com/nicoverbruggen/ebook-fonts"], license="SIL OFL 1.1 (por familia)",
    apply_fn=_fonts_apply, uninstall_fn=_fonts_uninstall, status_fn=_fonts_status,
))
