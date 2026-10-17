"""Nickel settings in '.kobo/Kobo/Kobo eReader.conf' (written with correct QSettings escaping)."""

from __future__ import annotations

from ..checks import EXCLUDE_SYNC_RAW
from ..core import Module, Option, register
from ..qtini import QtIni, qt_unescape
from ._util import CONF_REL, conf_edit, conf_get

EXCLUDE_SYNC_VALUE = qt_unescape(EXCLUDE_SYNC_RAW)          # plain regex; qt_escape doubles the backslashes
EXCLUDE_SYNC_CALIBRE = r"(calibre|\.(?!kobo|adobe|calibre).+|([^.][^/]*/)+\..+)"


def _exclude_apply(mod, ctx, opts):
    value = EXCLUDE_SYNC_CALIBRE if opts["hide_calibre"] else EXCLUDE_SYNC_VALUE
    conf_edit(ctx, sets=[("FeatureSettings", "ExcludeSyncFolders", value)],
              note="Nickel ignora carpetas ocultas (.adds, ._*) sin esconder tus libros")


def _exclude_status(mod, dev):
    raw = QtIni.load(dev.root / CONF_REL).get_raw("FeatureSettings", "ExcludeSyncFolders")
    if raw is None:
        return "no configurado"
    return "correcto" if raw == EXCLUDE_SYNC_RAW else ("CORRUPTO" if "\\\\" not in raw else "personalizado")


register(Module(
    id="conf.exclude_sync", category="Ajustes", title="Proteger la biblioteca (ExcludeSyncFolders)",
    description="Evita que Nickel importe ROMs, scripts e imágenes de carpetas ocultas. Se escribe con el escape "
                "correcto de Qt: una versión mal escapada oculta todos los libros en subcarpetas.",
    risk="bajo", recommended=True,
    options=[Option("hide_calibre", "Ocultar también la carpeta 'calibre'", "bool", False)],
    sources=["https://github.com/pgaskin/NickelMenu", "https://www.mobileread.com/forums/showthread.php?t=313456"],
    apply_fn=_exclude_apply, uninstall_fn=None, status_fn=_exclude_status,
))


def _bool(v) -> str:
    return "true" if v else "false"


def _prefs_apply(mod, ctx, opts):
    sets = [
        ("FeatureSettings", "Screenshots", _bool(opts["screenshots"])),
        ("DeveloperSettings", "AutoUsbGadget", _bool(opts["auto_usb"])),
        ("PowerOptions", "AutoOffMinutes", str(opts["auto_off"])),
    ]
    if opts["force_landscape"]:
        sets.append(("DeveloperSettings", "ForceAllowLandscape", "true"))
    conf_edit(ctx, sets=sets, note="preferencias de Nickel")


def _prefs_status(mod, dev):
    ini = QtIni.load(dev.root / CONF_REL)
    return f"capturas={ini.get('FeatureSettings', 'Screenshots') or '?'}, auto USB={ini.get('DeveloperSettings', 'AutoUsbGadget') or '?'}"


register(Module(
    id="conf.preferences", category="Ajustes", title="Preferencias rápidas de Nickel",
    description="Capturas con el botón de encendido, montaje USB sin preguntar, apagado automático y rotación "
                "horizontal en todas las vistas.",
    risk="bajo",
    options=[
        Option("screenshots", "Capturas de pantalla (botón de encendido)", "bool", False),
        Option("auto_usb", "Montar USB automáticamente al conectar", "bool", True),
        Option("auto_off", "Apagar tras inactividad", "choice", 30,
               [(15, "15 min"), (30, "30 min"), (60, "1 hora"), (120, "2 horas"), (0, "Nunca")]),
        Option("force_landscape", "Permitir horizontal en todas las vistas", "bool", False),
    ],
    apply_fn=_prefs_apply, status_fn=_prefs_status,
))


def _sideload_apply(mod, ctx, opts):
    conf_edit(ctx, sets=[("ApplicationPreferences", "SideloadedMode", _bool(opts["enabled"]))],
              note="modo sideload (sin tienda en Inicio)")


def _sideload_uninstall(mod, ctx):
    conf_edit(ctx, sets=[("ApplicationPreferences", "SideloadedMode", "false")])


register(Module(
    id="conf.sideloaded", category="Ajustes", title="Modo sideload (sin cuenta Kobo)",
    description="Inicio centrado en tus libros sin tienda ni recomendaciones. Ideal si no usas cuenta Kobo.",
    risk="bajo", min_fw=(4, 31),
    options=[Option("enabled", "Activar modo sideload", "bool", True)],
    apply_fn=_sideload_apply, uninstall_fn=_sideload_uninstall,
    status_fn=lambda m, d: "activo" if (QtIni.load(d.root / CONF_REL).get("ApplicationPreferences", "SideloadedMode")
                                        or "").lower() == "true" else "inactivo",
))
_ = conf_get  # re-exported helper for other modules
