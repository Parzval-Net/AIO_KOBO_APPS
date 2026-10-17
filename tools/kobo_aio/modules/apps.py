"""Applications launched from NickelMenu: KOReader, Cadmus and Ink-boy (Game Boy / GBC)."""

from __future__ import annotations

import io
import tarfile

from ..core import PROJECT_ROOT, Artifact, Module, Option, register, zip_members
from ._util import ART, ONBOARD, extract_tree

NM_DIR = ".adds/nm"


def _status_dir(rel: str):
    def fn(mod, dev):
        return "instalado" if (dev.root / rel).exists() else "no instalado"
    return fn


# -- KOReader ------------------------------------------------------------------
def _koreader_apply(mod, ctx, opts):
    data = ctx.fetch(ART["koreader"])
    members = zip_members(data, "koreader/")
    if not members:
        raise RuntimeError("El paquete de KOReader no tiene la estructura esperada (koreader/)")
    # Keep the user's KOReader settings, history and plugins configuration across updates.
    keep = {"settings.reader.lua", "defaults.custom.lua"}
    members = [m for m in members if not (m[0] in keep and ctx.txn.exists(f".adds/koreader/{m[0]}"))]
    extract_tree(ctx, members, ".adds/koreader", "KOReader")
    ctx.txn.write_text(f"{NM_DIR}/kobo-aio-koreader",
                       f"menu_item :main :KOReader :cmd_spawn :quiet:exec {ONBOARD}/.adds/koreader/koreader.sh\n")


def _koreader_uninstall(mod, ctx):
    # Remove only files shipped by the package: settings, statistics, history, plugins and dictionaries the
    # user added stay on the device.
    data = ctx.fetch(ART["koreader"])
    for rel, _, _ in zip_members(data, "koreader/"):
        ctx.txn.delete(f".adds/koreader/{rel}")
    ctx.txn.delete(f"{NM_DIR}/kobo-aio-koreader")


register(Module(
    id="app.koreader", category="Apps", title="KOReader 2026.07.1",
    description="Lector alternativo muy configurable: PDF y cómics con zoom/recorte, refresco y dithering a "
                "medida, estadísticas, sincronización y diccionarios StarDict. Se abre desde NickelMenu.",
    risk="bajo", requires=["mod.nickelmenu"],
    sources=["https://github.com/koreader/koreader", "https://github.com/koreader/koreader/issues/12739"],
    license="AGPL-3.0",
    apply_fn=_koreader_apply, uninstall_fn=_koreader_uninstall, status_fn=_status_dir(".adds/koreader/koreader.sh"),
))


# -- Cadmus --------------------------------------------------------------------
def _cadmus_apply(mod, ctx, opts):
    data = ctx.fetch(ART["cadmus"])
    members = []
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for m in tf.getmembers():
            name = m.name.lstrip("./")
            if m.isfile() and name and ".." not in name.split("/"):
                members.append((name, tf.extractfile(m).read(), m.mode & 0o777))
    extract_tree(ctx, members, ".adds/cadmus", "Cadmus")
    ctx.txn.write_text(f"{NM_DIR}/kobo-aio-cadmus",
                       f"menu_item :main :Cadmus :cmd_spawn :quiet:exec {ONBOARD}/.adds/cadmus/cadmus.sh\n")


def _cadmus_uninstall(mod, ctx):
    data = ctx.fetch(ART["cadmus"])
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for m in tf.getmembers():
            name = m.name[2:] if m.name.startswith("./") else m.name
            if m.isfile() and name and ".." not in name.split("/"):
                ctx.txn.delete(f".adds/cadmus/{name}")
    ctx.txn.delete(f"{NM_DIR}/kobo-aio-cadmus")


register(Module(
    id="app.cadmus", category="Apps", title="Cadmus 0.12.0",
    description="Lector minimalista y rápido (fork de Plato) para EPUB, PDF, CBZ y DjVu. La 0.12 agrega suspensión "
                "automática y WiFi que duerme; si ya lo usabas, revisa sus ajustes de energía (cambiaron de lugar).",
    risk="bajo", requires=["mod.nickelmenu"],
    sources=["https://github.com/OGKevin/cadmus"], license="AGPL-3.0",
    apply_fn=_cadmus_apply, uninstall_fn=_cadmus_uninstall, status_fn=_status_dir(".adds/cadmus/cadmus.sh"),
))


# -- Ink-boy -------------------------------------------------------------------
INKBOY_DIST = PROJECT_ROOT / "dist" / "Kobo-InkBoy" / ".adds"


def _inkboy_config(dev, opts) -> str:
    color = dev.model.get("color", False)
    return (
        "# Ink-boy - generado por Kobo AIO. Tus cambios se conservan al actualizar.\n"
        "[display]\n"
        f"scale = {opts['scale']}\n"
        f"mode = {opts['mode']}\n"
        f"dmg_palette = {opts['dmg_palette'] if color else 0}\n\n"
        "[input]\n"
        f"libra_buttons = {opts['libra_buttons']}\n\n"
        "[system]\n"
        f"exit_mode = {opts['exit_mode']}\n"
        "pause_watchdog = no\n"
    )


def _inkboy_apply(mod, ctx, opts):
    binary = INKBOY_DIST / "inkboy" / "inkboy"
    launcher = INKBOY_DIST / "inkboy" / "inkboy.sh"
    if not binary.is_file() or not launcher.is_file():
        raise RuntimeError("Falta el binario de Ink-boy: ejecuta 'make kobo' en el proyecto")
    ctx.txn.copy_file(binary, ".adds/inkboy/inkboy", note="binario ARM", mode=0o755)
    ctx.txn.copy_file(launcher, ".adds/inkboy/inkboy.sh", note="lanzador", mode=0o755)
    cfg = _inkboy_config(ctx.device, opts)
    if opts["overwrite_config"]:
        ctx.txn.write_text(".adds/inkboy/config.ini", cfg, note="configuración")
    else:
        ctx.txn.write_if_absent(".adds/inkboy/config.ini", cfg.encode(), note="configuración")
    roms = INKBOY_DIST / "inkboy" / "roms"
    if opts["demo_roms"] and roms.is_dir():
        for rom in sorted(roms.iterdir()):
            if rom.suffix.lower() in (".gb", ".gbc"):
                ctx.txn.write_if_absent(f".adds/inkboy/roms/{rom.name}", rom.read_bytes(), note="ROM demo")
    ctx.txn.write_text(f"{NM_DIR}/kobo-aio-inkboy", (
        f"menu_item :main :Ink-boy (Game Boy) :cmd_spawn :quiet:/bin/sh {ONBOARD}/.adds/inkboy/inkboy.sh\n"
        f"menu_item :main :Ink-boy turbo :cmd_spawn :quiet:/bin/sh {ONBOARD}/.adds/inkboy/inkboy.sh --mode turbo\n"))
    # Older installers wrote their own NickelMenu entry; drop it to avoid duplicated menu items.
    if ctx.txn.exists(f"{NM_DIR}/inkboy"):
        ctx.txn.delete(f"{NM_DIR}/inkboy", note="entrada antigua de Ink-boy (reemplazada)")


def _inkboy_uninstall(mod, ctx):
    # ROMs and .sav files are user data: never removed.
    for rel in (".adds/inkboy/inkboy", ".adds/inkboy/inkboy.sh", f"{NM_DIR}/kobo-aio-inkboy", f"{NM_DIR}/inkboy"):
        ctx.txn.delete(rel)


register(Module(
    id="app.inkboy", category="Juegos", title="Ink-boy (Game Boy / Game Boy Color)",
    description="Emulador nativo optimizado para e-ink: color Kaleido 3 en Libra Colour, 4 grises en Carta, modo "
                "turbo A2 y guardado automático. Nunca sobrescribe tus partidas (.sav).",
    risk="bajo", requires=["mod.nickelmenu"],
    options=[
        Option("mode", "Modo de pantalla", "choice", "quality",
               [("quality", "Calidad (color / 4 grises)"), ("turbo", "Turbo 1-bit (A2)")]),
        Option("scale", "Escala", "choice", "6", [("6", "x6 (960x864)"), ("7", "x7 (1120x1008, solo Libra)")]),
        Option("dmg_palette", "Paleta para juegos .gb", "choice", "1",
               [("0", "Grises"), ("1", "Pokémon Rojo/Azul"), ("2", "Verde clásico"), ("3", "Oro/Cian")]),
        Option("libra_buttons", "Botones físicos", "choice", "A_B",
               [("A_B", "Arriba A / abajo B"), ("B_A", "Arriba B / abajo A"), ("START_SELECT", "Start / Select")]),
        Option("exit_mode", "Al salir", "choice", "resume",
               [("resume", "Volver a Nickel"), ("reboot", "Reiniciar (si Nickel se congela)")]),
        Option("demo_roms", "Copiar ROMs de demostración", "bool", True),
        Option("overwrite_config", "Reemplazar mi config.ini", "bool", False),
    ],
    sources=["https://github.com/hellominecraft913-dev/Ink-boy"], license="MIT",
    apply_fn=_inkboy_apply, uninstall_fn=_inkboy_uninstall, status_fn=_status_dir(".adds/inkboy/inkboy"),
))


# -- KOReader plugins ----------------------------------------------------------
KOPLUGINS = {
    "projecttitle": (Artifact("https://github.com/joshuacant/ProjectTitle/releases/download/2026.07-v3.8.3/ProjectTitle-2026-07-01-v3-8-3.zip",
                              "c407d3432190c982dda52c99d6e51f89f004203ed7d8935cd27bf8f51c0701d3", "ProjectTitle-v3.8.3.zip"),
                     "projecttitle.koplugin", "Project: Title: biblioteca visual con portadas (AGPL-3.0)"),
    "appstore": (Artifact("https://github.com/omer-faruq/appstore.koplugin/releases/download/v1.14.0/appstore.koplugin.zip",
                          "52e802f2d18d1efc576aa489ded1781e2592ea82fa614cd4244632ce3140a548", "appstore-v1.14.0.zip"),
                 "appstore.koplugin", "App Store: instala y actualiza plugins desde el Kobo (GPL-3.0)"),
    "kobo": (Artifact("https://github.com/OGKevin/kobo.koplugin/releases/download/v0.4.1/kobo.koplugin.zip",
                      "5521515971e682def6af4060741090656e24fa7ced48fe1cba67b4f742e189bd", "kobo-koplugin-v0.4.1.zip"),
             "kobo.koplugin", "Sincroniza progreso con el lector de Kobo y controles Bluetooth (GPL-3.0)"),
    "hardcover": (Artifact("https://github.com/Billiam/hardcoverapp.koplugin/releases/download/0.4.0/hardcoverapp.koplugin.zip",
                           "0ca1eba0eccc627b0cab8c3457883ae7a9e771b2cd26aa09794a7041904e1b10", "hardcoverapp-0.4.0.zip"),
                  "hardcoverapp.koplugin", "Hardcover: sincroniza lecturas con hardcover.app (MIT)"),
    "localsend": (Artifact("https://github.com/kaikozlov/localsend.koplugin/releases/download/v1.4.5/localsend-koplugin-armv7.zip",
                           "baf8f20cc54274a418b51b96a58fbbe6e8447d98b8cb909dbe2634826a0eaa9d", "localsend-armv7-1.4.5.zip"),
                  "localsend.koplugin", "LocalSend: envía libros por WiFi desde el celular, sin cables (MIT)"),
}


def _kop_apply(mod, ctx, opts):
    for key in opts["plugins"]:
        art, folder, _ = KOPLUGINS[key]
        members = zip_members(ctx.fetch(art), f"{folder}/")
        if not members:
            raise RuntimeError(f"El paquete de {folder} no tiene la estructura esperada")
        extract_tree(ctx, members, f".adds/koreader/plugins/{folder}", folder)
        installed = ctx.state.setdefault("koreader_plugins", [])
        if key not in installed:
            installed.append(key)


def _kop_uninstall(mod, ctx):
    for key in ctx.state.get("koreader_plugins", []):
        art, folder, _ = KOPLUGINS[key]
        for rel, _, _ in zip_members(ctx.fetch(art), f"{folder}/"):
            ctx.txn.delete(f".adds/koreader/plugins/{folder}/{rel}")
    ctx.state["koreader_plugins"] = []


register(Module(
    id="app.koreader_plugins", category="Apps", title="Plugins para KOReader",
    description="Los más usados por la comunidad en 2026: biblioteca con portadas, tienda de plugins, sincronización "
                "con el lector de Kobo, Hardcover y envío inalámbrico con LocalSend.",
    risk="bajo", requires=["app.koreader"],
    options=[Option("plugins", "Plugins", "multi", ["projecttitle", "localsend"],
                    [(k, v[2]) for k, v in KOPLUGINS.items()])],
    sources=["https://github.com/joshuacant/ProjectTitle", "https://github.com/omer-faruq/appstore.koplugin",
             "https://github.com/OGKevin/kobo.koplugin", "https://github.com/Billiam/hardcoverapp.koplugin",
             "https://github.com/kaikozlov/localsend.koplugin"],
    license="AGPL-3.0 / GPL-3.0 / MIT (por plugin)",
    apply_fn=_kop_apply, uninstall_fn=_kop_uninstall,
    status_fn=lambda m, d: (f"{len([p for p in KOPLUGINS.values() if (d.root / '.adds/koreader/plugins' / p[1]).is_dir()])} instalados"
                            if (d.root / ".adds/koreader/plugins").is_dir() else "no instalado"),
))
