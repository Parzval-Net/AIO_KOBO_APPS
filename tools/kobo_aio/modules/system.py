"""Nickel plugins delivered through KoboRoot.tgz (installed by the Kobo on eject + reboot)."""

from __future__ import annotations

import re

from ..core import Module, Option, register
from ._util import ONBOARD, koboroot_from, tgz_onboard_files, toggle_script

NM_DIR = ".adds/nm"
MTK_COLOR_IDS = ("390", "391", "393", "395")  # NickelDissolve is validated on these (per KoboPatch Web UI)


def _status_file(rel: str):
    def fn(mod, dev):
        return "instalado" if (dev.root / rel).exists() else "no instalado"
    return fn


def _stage_plugin(ctx, key: str, keep_onboard: tuple = ()) -> None:
    """Queue a plugin KoboRoot.tgz. Onboard files it ships (doc, uninstall markers) are kept as-is."""
    data = koboroot_from(ctx, key)
    ctx.add_koboroot(key, data)
    for rel, _ in tgz_onboard_files(data):
        if rel in keep_onboard and ctx.txn.exists(rel):
            ctx.txn.note(f"Conservar {rel}")


# -- NickelMenu ----------------------------------------------------------------
def _nm_apply(mod, ctx, opts):
    _stage_plugin(ctx, "nickelmenu")
    if ctx.txn.exists(f"{NM_DIR}/uninstall"):
        ctx.txn.delete(f"{NM_DIR}/uninstall", note="quita la marca de desinstalación de NickelMenu")


def _nm_uninstall(mod, ctx):
    # Official method: an 'uninstall' file in .adds/nm makes NickelMenu remove libnm.so on next boot.
    ctx.txn.write_text(f"{NM_DIR}/uninstall", "", note="NickelMenu se desinstala en el próximo reinicio")


register(Module(
    id="mod.nickelmenu", category="Sistema", title="NickelMenu 0.6.0",
    description="Menús personalizados en Nickel: base para lanzar apps, atajos y casi todo el AIO.",
    risk="bajo", recommended=True, reboot=True, min_fw=(4, 6), max_fw=(5,),
    sources=["https://github.com/pgaskin/NickelMenu"], license="MIT",
    apply_fn=_nm_apply, uninstall_fn=_nm_uninstall, status_fn=_status_file(f"{NM_DIR}/doc"),
))


# -- NickelHome ----------------------------------------------------------------
HOME_KEYS = [
    ("row1col2", "Ocultar recomendaciones junto a la lectura actual", True),
    ("row2col2", "Ocultar el contenido junto a «Mis libros»", True),
    ("row3", "Ocultar la fila de avisos y promociones", True),
    ("row2", "Ocultar también «Mis libros» en Inicio", False),
]
LEGACY_HOME_RE = re.compile(r"^\s*experimental\s*:\s*hide_home_\w+", re.M)


def _home_apply(mod, ctx, opts):
    _stage_plugin(ctx, "nickelhome")
    if ctx.txn.exists(".adds/nickel-home/uninstall"):  # a previous uninstall left the flag: clear it
        ctx.txn.delete(".adds/nickel-home/uninstall", note="quita la marca de desinstalación de NickelHome")
    lines = ["# Kobo AIO - NickelHome (requiere reinicio para aplicar cambios)", "nhm_enabled:1"]
    lines += [f"hide_home_{k}_enabled:{1 if opts[k] else 0}" for k, _, _ in HOME_KEYS]
    ctx.txn.write_text(".adds/nickel-home/config", "\n".join(lines) + "\n", note="configuración de NickelHome")
    ctx.txn.write_text(".adds/kobo-aio/scripts/toggle_home.sh",
                       toggle_script(f"{ONBOARD}/.adds/nickel-home/config", "s/^nhm_enabled:0/nhm_enabled:1/",
                                     "s/^nhm_enabled:1/nhm_enabled:0/", "grep -q '^nhm_enabled:1' \"$F\"",
                                     "Inicio minimalista"))
    ctx.txn.write_text(f"{NM_DIR}/kobo-aio-home",
                       f"menu_item :main :Inicio minimalista :cmd_output :7000:/bin/sh {ONBOARD}/.adds/kobo-aio/scripts/toggle_home.sh\n")
    # Old KoboPatch Web UI presets (< v1.50) hid widgets with a NickelMenu fork; stock NickelMenu ignores
    # those keys, so comment them out to avoid confusion now that NickelHome does the job.
    nm_root = ctx.device.root / NM_DIR
    if nm_root.is_dir():
        for f in nm_root.iterdir():
            if not f.is_file() or f.name.startswith(("kobo-aio", ".")) or f.name in ("doc", "uninstall"):
                continue
            text = ctx.txn.read_text(f"{NM_DIR}/{f.name}") or ""
            if LEGACY_HOME_RE.search(text):
                new = LEGACY_HOME_RE.sub(lambda m: "# (migrado a NickelHome) " + m.group(0).lstrip(), text)
                ctx.txn.write_text(f"{NM_DIR}/{f.name}", new, note="migra hide_home_* antiguos a NickelHome")


def _home_uninstall(mod, ctx):
    # Verified with REA/Ghidra on libnickelhome.so v0.6 (nh_info @0x2726c): uninstall_flag is
    # /mnt/onboard/.adds/nickel-home/uninstall (created -> uninstall) and uninstall_xflag is the folder itself.
    # The transaction cannot remove folders, so create the flag; NickelHook removes the plugin on next boot.
    ctx.txn.write_text(".adds/nickel-home/uninstall", "", note="NickelHome se desinstala en el próximo reinicio")
    for rel in (f"{NM_DIR}/kobo-aio-home", ".adds/kobo-aio/scripts/toggle_home.sh"):
        ctx.txn.delete(rel)


register(Module(
    id="mod.nickelhome", category="Sistema", title="Inicio minimalista (NickelHome 0.6)",
    description="Oculta recomendaciones, promociones y avisos de la pantalla de inicio sin parchear el firmware.",
    risk="bajo", recommended=True, reboot=True, requires=["mod.nickelmenu"], min_fw=(4, 23), max_fw=(5,),
    options=[Option(k, label, "bool", default) for k, label, default in HOME_KEYS],
    sources=["https://github.com/nicoverbruggen/NickelHome"], license="MIT",
    apply_fn=_home_apply, uninstall_fn=_home_uninstall, status_fn=_status_file(".adds/nickel-home/doc"),
))


# -- NickelTypeFix -------------------------------------------------------------
def _typefix_apply(mod, ctx, opts):
    from ._util import conf_edit
    _stage_plugin(ctx, "nickeltypefix", keep_onboard=(".adds/nickel-type-fix/uninstall",))
    if opts["optimize_legibility"]:
        conf_edit(ctx, sets=[("Reading", "webkitTextRendering", "optimizeLegibility")],
                  note="kerning y ligaduras (optimizeLegibility)")


def _typefix_uninstall(mod, ctx):
    from ._util import conf_edit
    ctx.txn.delete(".adds/nickel-type-fix/uninstall", note="NickelTypeFix se quita en el próximo reinicio")
    conf_edit(ctx, removes=[("Reading", "webkitTextRendering")])


register(Module(
    id="mod.nickeltypefix", category="Lectura", title="Tipografía mejorada (NickelTypeFix 1.0)",
    description="Activa kerning/ligaduras y corrige justificado irregular, letras que bailan, versalitas y fuentes "
                "que vuelven a la del sistema en kepubs. La 1.0 abre capítulos largos varias veces más rápido.",
    risk="bajo", recommended=True, reboot=True, min_fw=(4, 21), max_fw=(5,),
    options=[Option("optimize_legibility", "Activar optimizeLegibility (kerning y ligaduras)", "bool", True)],
    sources=["https://github.com/nicoverbruggen/NickelTypeFix"], license="MIT",
    apply_fn=_typefix_apply, uninstall_fn=_typefix_uninstall,
    status_fn=_status_file(".adds/nickel-type-fix/uninstall"),
))


# -- NickelClock ---------------------------------------------------------------
def _clock_apply(mod, ctx, opts):
    _stage_plugin(ctx, "nickelclock", keep_onboard=(".adds/nickelclock/uninstall",))
    ini = (
        "[General]\nDebug=false\nMargin=Auto\n\n"
        f"[Clock]\nEnabled=true\nPlacement={opts['placement']}\nPosition={opts['position']}\n\n"
        f"[Battery]\nEnabled={'true' if opts['battery'] else 'false'}\nBatteryType=Level\nLevelTemplate=%1%\n"
        f"Placement={opts['placement']}\nPosition={'Left' if opts['position'] == 'Right' else 'Right'}\n"
    )
    ctx.txn.write_text(".adds/nickelclock/settings.ini", ini, note="reloj y batería en el lector")
    ctx.txn.write_text(".adds/kobo-aio/scripts/toggle_clock.sh", toggle_script(
        f"{ONBOARD}/.adds/nickelclock/settings.ini", "/^\\[Clock\\]/,/^\\[/ s/^Enabled=false/Enabled=true/",
        "/^\\[Clock\\]/,/^\\[/ s/^Enabled=true/Enabled=false/",
        "sed -n '/^\\[Clock\\]/,/^\\[B/p' \"$F\" | grep -q '^Enabled=true'", "Reloj"))
    ctx.txn.write_text(f"{NM_DIR}/kobo-aio-clock",
                       f"menu_item :main :Reloj on/off :cmd_output :7000:/bin/sh {ONBOARD}/.adds/kobo-aio/scripts/toggle_clock.sh\n")


def _clock_uninstall(mod, ctx):
    for rel in (".adds/nickelclock/uninstall", f"{NM_DIR}/kobo-aio-clock", ".adds/kobo-aio/scripts/toggle_clock.sh"):
        ctx.txn.delete(rel)


register(Module(
    id="mod.nickelclock", category="Lectura", title="Reloj y batería al leer (NickelClock 0.4.0)",
    description="Muestra la hora (y opcionalmente la batería) en el encabezado o pie del lector.",
    risk="bajo", reboot=True, requires=["mod.nickelmenu"], min_fw=(4, 23), max_fw=(5,),
    options=[
        Option("placement", "Ubicación", "choice", "Header", [("Header", "Encabezado"), ("Footer", "Pie")]),
        Option("position", "Lado del reloj", "choice", "Right", [("Left", "Izquierda"), ("Center", "Centro"),
                                                                  ("Right", "Derecha")]),
        Option("battery", "Mostrar batería", "bool", True),
    ],
    sources=["https://github.com/shermp/NickelClock"], license="MIT",
    apply_fn=_clock_apply, uninstall_fn=_clock_uninstall, status_fn=_status_file(".adds/nickelclock/uninstall"),
))


# -- NickelDissolve ------------------------------------------------------------
def _dissolve_apply(mod, ctx, opts):
    _stage_plugin(ctx, "nickeldissolve", keep_onboard=(".adds/nickel-dissolve/uninstall",))


def _dissolve_uninstall(mod, ctx):
    ctx.txn.delete(".adds/nickel-dissolve/uninstall", note="NickelDissolve se quita en el próximo reinicio")


register(Module(
    id="mod.nickeldissolve", category="Lectura", title="Animación al pasar página (NickelDissolve 0.7)",
    description="Transición de disolución entre páginas. Experimental; solo en Kobo MediaTek recientes.",
    risk="medio", reboot=True, model_ids=MTK_COLOR_IDS, min_fw=(4, 38), max_fw=(5,),
    sources=["https://github.com/nicoverbruggen/NickelDissolve"], license="MIT",
    apply_fn=_dissolve_apply, uninstall_fn=_dissolve_uninstall,
    status_fn=_status_file(".adds/nickel-dissolve/uninstall"),
))


# -- NickelModManager ----------------------------------------------------------
def _nmm_apply(mod, ctx, opts):
    _stage_plugin(ctx, "nickelmodmanager")


def _nmm_uninstall(mod, ctx):
    ctx.txn.delete(".adds/nickel-mod-manager/uninstall", note="NickelModManager se quita en el próximo reinicio")


register(Module(
    id="mod.nickelmodmanager", category="Sistema", title="Gestor de mods en el Kobo (NickelModManager 0.3)",
    description="Agrega «Más > Administrar mods» en el Kobo para activar o desactivar cada mod sin computador. "
                "Útil si un mod da problemas tras una actualización. Proyecto nuevo (octubre 2026).",
    risk="medio", reboot=True, min_fw=(4, 23), max_fw=(5,),
    sources=["https://github.com/nicoverbruggen/NickelModManager"], license="MIT",
    apply_fn=_nmm_apply, uninstall_fn=_nmm_uninstall,
    status_fn=_status_file(".adds/nickel-mod-manager"),
))
