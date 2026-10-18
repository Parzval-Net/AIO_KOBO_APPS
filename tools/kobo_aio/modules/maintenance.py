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


# -- Back to stock ---------------------------------------------------------------------------------------
# How each NickelHook mod removes itself, read from the pinned binaries with REA/Ghidra (nh_info of every
# NickelHook mod, 2026-10-17): "flag" = create this file, "xflag" = delete this file. The mod deletes its own
# library from the system on the next boot, so the Kobo must be ejected and restarted afterwards.
# A Kobo factory reset formats the user storage but keeps /usr/local/Kobo, so mods that only uninstall on a
# flag (and NickelModManager, which then takes the empty storage for a new install and recreates its marker)
# keep running. Those flags are written even when their folder is gone ("survives" = True).
STOCK_MODS = [
    # (folder that shows it is installed, file to create, file to delete, survives a Kobo factory reset)
    (".adds/nm", ".adds/nm/uninstall", None, True),                                         # NickelMenu: flag
    (".adds/nickel-home", ".adds/nickel-home/uninstall", None, True),                       # NickelHome: flag
    (".adds/nickel-type-fix", ".adds/nickel-type-fix/uninstall-now", ".adds/nickel-type-fix/uninstall", False),
    (".adds/nickelclock", None, ".adds/nickelclock/uninstall", False),                      # NickelClock: xflag
    (".adds/nickel-dissolve", ".adds/nickel-dissolve/uninstall-now", ".adds/nickel-dissolve/uninstall", False),
    # NickelModManager 0.3 (src/entrypoint/lifecycle.cpp): removed when uninstall-now exists, or when its receipt
    # .adds/.nickel-mod-manager.initialized exists and the uninstall marker does not. Not NickelHook.
    (".adds/nickel-mod-manager", ".adds/nickel-mod-manager/uninstall-now", ".adds/nickel-mod-manager/uninstall", True),
    (".adds/screensaver", ".adds/screensaver/uninstall.txt", None, True),                   # nickel-screensaver: flag
    (".adds/aio-audio", ".adds/aio-audio/uninstall", None, True),                           # AIO Audio: flag
]
# Kobo eReader.conf keys that Kobo AIO (or the usual mod guides) set and stock firmware does not have.
STOCK_CONF_KEYS = [("FeatureSettings", "ExcludeSyncFolders"), ("FeatureSettings", "Screenshots"),
                   ("DeveloperSettings", "AutoUsbGadget"), ("DeveloperSettings", "ForceAllowLandscape"),
                   ("ApplicationPreferences", "SideloadedMode")]
APP_TREES = (".adds/koreader", ".adds/cadmus", ".adds/plato", ".adds/inkboy")
KNOWN_ADDS = {p.split("/")[1] for p, _, _, _ in STOCK_MODS} | {p.split("/")[1] for p in APP_TREES} | {"kobo-aio"}


def _files_under(ctx, rel: str, keep=()) -> list[str]:
    root = ctx.device.root / rel
    if not root.is_dir():
        return []
    rels = (f.relative_to(ctx.device.root).as_posix() for f in sorted(root.rglob("*")) if f.is_file())
    return [r for r in rels if r not in keep]   # POSIX paths, also on Windows (keep holds POSIX paths)


def _stock_apply(mod, ctx, opts):
    from ..core import REGISTRY
    from ._util import conf_edit
    root, txn = ctx.device.root, ctx.txn
    installed = ctx.state.get("installed", {})
    # 1. Firmware patches first: their restore needs the official binaries (desktop engine, exact firmware).
    if opts["restore_system"] and "fw.kobopatch" in installed:
        fw = REGISTRY["fw.kobopatch"]
        ok, why = fw.compatible(ctx.device)
        try:
            if not ok:
                raise RuntimeError(why)
            fw.apply_fn(fw, ctx, fw.resolved_options({"mode": "restore", "confirm": True}))
        except Exception as exc:  # noqa: BLE001 - e.g. browser mode (no kobopatch) or another firmware
            txn.note(f"No se pudo restaurar el sistema original aquí ({exc}). Usa la app de escritorio o el "
                     "restablecimiento de fábrica del Kobo.")
    # 2. Nothing queued may install mods on the next boot (except the restored originals added above).
    if txn.exists(".kobo/KoboRoot.tgz"):
        txn.delete(".kobo/KoboRoot.tgz", note="mejoras pendientes de instalar: se descartan")
    # 3. Every mod removes itself on the next boot (flags work in every mode, also inside a KoboRoot.tgz).
    hidden = []
    for folder, flag, xflag, survives in STOCK_MODS:
        name = folder.split("/")[1]
        present = (root / folder).is_dir()
        if flag and (present or survives):
            if present:
                txn.write_text(flag, "", note=f"{name} se desinstala al reiniciar")
            elif not txn.exists(flag):
                txn.write_text(flag, "")
                hidden.append(name)
        if present and xflag and txn.exists(xflag):
            txn.delete(xflag, note=f"{name} se desinstala al reiniciar")
    if hidden:
        txn.note("Por si el Kobo ya se restableció antes y quedaron mods en el sistema, se dejan avisos de "
                 f"desinstalación para {', '.join(hidden)} (si no están instalados, no pasa nada).")
    keep = {flag for _, flag, _, _ in STOCK_MODS if flag}
    # NickelMenu entries and NickelModManager's kept copies (it can put mods back after a system update).
    for rel in _files_under(ctx, ".adds/nm", keep) + _files_under(ctx, ".adds/nickel-mod-manager", keep):
        txn.delete(rel, note="configuración de mods")
    # 4. Apps and their files.
    if opts["remove_apps"] and opts["remove_app_data"]:
        # Whole app folders go, so there is no need to download each package to know which files are its own.
        for tree in APP_TREES:
            for rel in _files_under(ctx, tree):
                txn.delete(rel, note="app y sus datos")
    elif opts["remove_apps"]:
        for mid in ("app.koreader_plugins", "app.audiobook", "app.koreader", "app.cadmus", "app.inkboy"):
            m = REGISTRY.get(mid)
            if m and m.uninstall_fn and (mid in installed or (m.status_fn and not str(m.status(ctx.device)).startswith("no "))):
                m.uninstall_fn(m, ctx)   # keeps user data; KOReader/Cadmus need their package to list their files
    # 5. Fonts, dictionaries and sleep screens that were added to the Kobo.
    if opts["remove_extras"]:
        for rel in _files_under(ctx, "fonts") + _files_under(ctx, ".kobo/custom-dict") + _files_under(ctx, ".kobo/screensaver"):
            txn.delete(rel, note="agregado al Kobo")
    # 6. Settings that stock firmware does not have.
    if opts["reset_settings"]:
        conf_edit(ctx, removes=STOCK_CONF_KEYS, note="ajustes de mods: valores de fábrica")
    # 7. Kobo AIO's own files; the engine then drops its state file instead of rewriting it.
    for rel in _files_under(ctx, ".adds/kobo-aio"):
        txn.delete(rel, note="archivos de Kobo AIO")
    ctx.state["installed"] = {}
    ctx.state["_stock"] = True
    others = sorted(p.name for p in (root / ".adds").iterdir()
                    if p.is_dir() and p.name not in KNOWN_ADDS) if (root / ".adds").is_dir() else []
    if others:
        txn.note(f"Quedan carpetas de mods que Kobo AIO no reconoce ({', '.join(others)}): el restablecimiento de "
                 "fábrica del Kobo las elimina.")
    txn.note("Expulsa el Kobo: al reiniciar, cada mod se desinstala solo. Tus libros y tu progreso no se tocan. "
             "Si lo vas a vender, haz el restablecimiento de fábrica del Kobo recién después de ese reinicio.")


register(Module(
    id="maint.stock", category="Mantenimiento", title="Volver a stock (quitar todos los mods)",
    description="Deja el Kobo como de fábrica: desinstala los mods de Nickel (también los que pusiste con otras "
                "herramientas), las apps, las fuentes y diccionarios agregados, los ajustes de mods y los parches "
                "del sistema. Respalda todo antes y no toca tus libros. Para venderlo, después usa el "
                "restablecimiento de fábrica del propio Kobo.",
    risk="medio", persistent=False, reboot=True, order=98,
    options=[Option("remove_apps", "Quitar apps (KOReader, Cadmus, Ink-boy) y plugins", "bool", True),
             Option("remove_app_data", "Borrar también los datos de esas apps (ajustes, partidas)", "bool", True),
             Option("remove_extras", "Quitar fuentes, diccionarios y pantallas de reposo agregados", "bool", True),
             Option("reset_settings", "Volver los ajustes de mods a los de fábrica", "bool", True),
             Option("restore_system", "Restaurar el sistema original si aplicaste parches", "bool", True)],
    apply_fn=_stock_apply,
))
