"""One-shot maintenance actions. Library and database are only ever READ (backups go to the computer)."""

from __future__ import annotations

import shutil
import time
from pathlib import Path

from ..checks import scan_volume
from ..core import Module, Option, register
from ..txn import is_appledouble

HOST_LIBRARY = Path.home() / "Kobo-AIO" / "library"


def _clean_apply(mod, ctx, opts):
    root = ctx.device.root
    scan = scan_volume(root)
    n = 0
    for rel in scan["junk"]:
        p = root / rel
        if is_appledouble(p) or p.name == ".DS_Store":
            ctx.txn.delete_junk(p)
            n += 1
    ctx.txn.note(f"{n} archivos basura de macOS eliminados (._*, .DS_Store)")


register(Module(
    id="maint.clean_macos", category="Mantenimiento", title="Limpiar basura de macOS",
    description="Borra los archivos ._* (validando su firma AppleDouble) y .DS_Store que macOS deja en el Kobo.",
    risk="bajo", recommended=True, persistent=False, order=99, apply_fn=_clean_apply,
))


def _nospot_apply(mod, ctx, opts):
    ctx.txn.write_if_absent(".metadata_never_index", b"", note="Spotlight no indexará el Kobo")


register(Module(
    id="maint.no_spotlight", category="Mantenimiento", title="Desactivar Spotlight en el Kobo",
    description="Evita que macOS cree índices (.Spotlight-V100) en el lector en cada conexión.",
    risk="bajo", recommended=True, persistent=False, apply_fn=_nospot_apply,
))


def _backup_apply(mod, ctx, opts):
    """Copy books + Kobo database to the computer. Nothing is written to the Kobo."""
    root = ctx.device.root
    dest = HOST_LIBRARY / (ctx.device.serial or "kobo") / time.strftime("%Y%m%d-%H%M%S")
    scan = scan_volume(root)
    items = list(scan["books"]) if opts["books"] else []
    if opts["database"]:
        items += [r for r in (".kobo/KoboReader.sqlite", ".kobo/KoboReader.sqlite-wal", ".kobo/Kobo/Kobo eReader.conf",
                              "metadata.calibre", ".kobo/version") if (root / r).is_file()]
    if opts["mods"] and (root / ".adds").is_dir():
        items += [str(p.relative_to(root)) for p in (root / ".adds").rglob("*")
                  if p.is_file() and not p.name.startswith("._") and p.stat().st_size < 20 * 1024 * 1024]
    total = sum((root / r).stat().st_size for r in items)
    ctx.txn.note(f"Respaldo de {len(items)} archivos ({total / 1024 ** 2:.0f} MB) en {dest}")
    if ctx.txn.dry_run:
        return
    free = shutil.disk_usage(Path.home()).free
    if total > free - 512 * 1024 ** 2:
        raise RuntimeError(f"No hay espacio en el computador: se necesitan {total / 1024 ** 3:.1f} GB")
    for rel in items:
        dst = dest / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / rel, dst)
    ctx.progress(f"Respaldo listo en {dest}")


register(Module(
    id="maint.backup", category="Mantenimiento", title="Respaldar libros y progreso al computador",
    description="Copia tus libros, la base de datos (progreso, notas, colecciones) y la configuración a "
                "~/Kobo-AIO/library. Hazlo antes de cualquier cambio grande.",
    risk="bajo", recommended=True, persistent=False, order=0,
    options=[Option("books", "Libros", "bool", True), Option("database", "Base de datos y configuración", "bool", True),
             Option("mods", "Mods y partidas (.adds)", "bool", True)],
    apply_fn=_backup_apply,
))
