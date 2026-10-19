"""NickelMenu menu builder: curated actions, bottom-tab layout and owner card."""

from __future__ import annotations

import re

from pathlib import Path

from .. import tabicons
from ..core import AioError, Module, Option, register
from ..nickelmenu import CATALOG, CATALOG_BY_ID, GAMES, RETIRED, build_config
from ._util import ONBOARD

NM_DIR = ".adds/nm"
# Outside .adds/nm on purpose: NickelMenu parses every file in that folder as a config.
ICON_REL = ".adds/kobo-aio/tab-icon.png"
ICON_ACTIVE_REL = ".adds/kobo-aio/tab-icon-active.png"


def nickel_latin1(text: str) -> str:
    """The tab label as NickelMenu 0.6 reads it: main_nav_button_configure builds it with QString::fromAscii,
    which is Latin-1 in Qt 5 (REA on libnm.so), while menu item labels use fromUtf8. So the label is written as
    Latin-1 bytes (carried through write_text via surrogateescape); characters outside Latin-1 lose their accent.
    """
    import unicodedata
    out = []
    for ch in text:
        try:
            ch.encode("latin-1")
            out.append(ch)
        except UnicodeEncodeError:
            out.append(unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode("ascii"))
    return "".join(out).encode("latin-1").decode("utf-8", "surrogateescape")


def _menu_apply(mod, ctx, opts):
    items = [i for i in opts["items"]]
    model = ctx.device.model
    if not model.get("dark_mode", True):   # Kobo help center list of models without Dark mode
        items = [i for i in items if i != "dark_mode"]
        ctx.txn.note("Tu modelo no tiene modo oscuro: se omitió ese atajo")
    if not model.get("bluetooth", False):
        items = [i for i in items if i != "bt_toggle"]
    header = []
    if opts["tab_label"]:
        label = re.sub(r"[:\t\n]", "", str(opts["tab_label"]))[:12]
        header.append(f"experimental :menu_main_15505_label :{nickel_latin1(label)}")
    own = [Path(p) for p in (opts.get("tab_icon_file") or [])]
    if own:
        try:
            idle, active = tabicons.from_image(own[0].read_bytes())
        except Exception as exc:  # noqa: BLE001 - unreadable picture
            raise AioError(f"No se pudo usar {own[0].name} como ícono: elige un PNG o JPG") from exc
    else:
        icon = opts.get("tab_icon") or tabicons.DEFAULT
        idle, active = tabicons.render_png(icon, False), tabicons.render_png(icon, True)
    ctx.txn.write_bytes(ICON_REL, idle, note="ícono de la pestaña (50×50, como los de Nickel)")
    ctx.txn.write_bytes(ICON_ACTIVE_REL, active, note="ícono de la pestaña activa")
    header += [f"experimental :menu_main_15505_icon :{ONBOARD}/{ICON_REL}",
               f"experimental :menu_main_15505_icon_active :{ONBOARD}/{ICON_ACTIVE_REL}"]
    if opts["simple_tabs"]:
        # Tab indexes on 4.41+: 0 Home, 1 My Books, 2 Activity, 3 My Notebooks, 4 Discover, 5 More (NickelMenu doc)
        header += [
            "experimental :menu_main_15505_enabled :1",
            f"experimental :menu_main_15505_0_enabled :{0 if opts['hide_home'] else 1}",
            "experimental :menu_main_15505_1_label :Libros",
            "experimental :menu_main_15505_2_enabled :1",
            "experimental :menu_main_15505_2_label :Actividad",
            f"experimental :menu_main_15505_3_enabled :{1 if opts['notebooks'] else 0}",
            f"experimental :menu_main_15505_4_enabled :{0 if opts['hide_discover'] else 1}",
            "experimental :menu_main_15505_5_enabled :1",
        ]
    old = ctx.txn.read_text(f"{NM_DIR}/kobo-aio-menu") if ctx.txn.exists(f"{NM_DIR}/kobo-aio-menu") else ""
    if ":nickel_extras :" in old and any(f":{CATALOG_BY_ID[g].arg}" in old for g in GAMES) \
            and not ctx.txn.exists(f"{NM_DIR}/kobo-aio-games"):
        ctx.txn.note("Los juegos de Kobo se movieron a «Juegos ocultos de Kobo»: actívalo para seguir viéndolos.")
    ctx.txn.write_text(f"{NM_DIR}/kobo-aio-menu", build_config(items, "\n".join(header)), note="menú NickelMenu")
    legacy = f"{NM_DIR}/webui-preset"
    if opts["replace_kp_preset"] and ctx.txn.exists(legacy):
        # Old KoboPatch Web UI preset duplicates these items and fights over the same experimental keys.
        ctx.txn.write_bytes(".adds/kobo-aio/disabled/webui-preset", ctx.txn.read_bytes(legacy),
                            note="copia del preset anterior (para restaurarlo si quieres)")
        ctx.txn.delete(legacy, note="preset antiguo de KoboPatch Web UI reemplazado por el menú del AIO")


def _menu_uninstall(mod, ctx):
    for rel in (f"{NM_DIR}/kobo-aio-menu", ICON_REL, ICON_ACTIVE_REL):
        ctx.txn.delete(rel)


_choices = [(i.id, f"[{i.group}] {i.label}" + (" (riesgo alto)" if i.risk == "alto" else "")) for i in CATALOG
            if i.id not in GAMES]
register(Module(
    id="nm.menu", category="Menú", title="Pestaña de atajos (NickelMenu)",
    description="Una pestaña en la barra inferior del Kobo con botones que tocas cuando los necesitas: suspender, "
                "reiniciar, importar libros, modo oscuro, WiFi, tomar una captura, búsqueda en "
                "RAE/Wikipedia y respaldo de la base de datos. Los ajustes que se fijan una sola vez están en "
                "«Ajustes ocultos del Kobo».",
    risk="bajo", recommended=True, requires=["mod.nickelmenu"],
    options=[
        Option("items", "Atajos", "multi", [i.id for i in CATALOG if i.default and i.id not in GAMES], _choices,
               retired=RETIRED + GAMES),
        Option("tab_label", "Nombre de la pestaña (máx. 12 letras)", "text", "Kobo AIO"),
        Option("tab_icon", "Ícono de la pestaña", "choice", tabicons.DEFAULT, tabicons.choices(),
               help="Mismo tamaño y estilo que los íconos de Kobo: contorno en reposo, relleno al tocarla.",
               icons=tabicons.previews()),
        Option("tab_icon_file", "O usa tu propia imagen (PNG o JPG, opcional)", "files", []),
        Option("simple_tabs", "Reordenar pestañas inferiores", "bool", False),
        Option("notebooks", "Mostrar Cuadernos", "bool", False),
        Option("hide_discover", "Ocultar Descubrir (tienda)", "bool", True),
        Option("hide_home", "Ocultar Inicio (sin cuenta Kobo muestra lo mismo que Mis libros)", "bool", False,
               help="Requiere «Reordenar pestañas inferiores». Al cerrar un libro vuelves a Mis libros (probado en "
                    "una Libra Colour con firmware 4.45)."),
        Option("replace_kp_preset", "Reemplazar el preset antiguo de KoboPatch Web UI (si existe)", "bool", True),
    ],
    sources=["https://github.com/pgaskin/NickelMenu/blob/master/res/doc",
             "https://github.com/Parzval-Net/NickelMenu-Builder"],
    license="MIT",
    apply_fn=_menu_apply, uninstall_fn=_menu_uninstall,
    status_fn=lambda m, d: "instalado" if (d.root / NM_DIR / "kobo-aio-menu").exists() else "no instalado",
))


def _sh_quote(s: str) -> str:
    return "'" + str(s).replace("'", "'\"'\"'") + "'"


def _owner_apply(mod, ctx, opts):
    fields = [("Nombre", opts["name"]), ("Correo", opts["email"]), ("Teléfono", opts["phone"]),
              ("Recompensa", opts["reward"]), ("Nota", opts["note"])]
    lines = ["#!/bin/sh", "# Kobo AIO: datos del propietario (se muestran desde NickelMenu)",
             "echo 'Si encuentras este lector, por favor contáctame:'"]
    lines += [f"echo {_sh_quote(f'{k}: {v}')}" for k, v in fields if str(v).strip()]
    ctx.txn.write_text(".adds/kobo-aio/scripts/owner.sh", "\n".join(lines) + "\n", mode=0o755)
    ctx.txn.write_text(f"{NM_DIR}/kobo-aio-owner",
                       "menu_item :main :Propietario :cmd_output :500:/bin/sh /mnt/onboard/.adds/kobo-aio/scripts/owner.sh\n")


def _owner_uninstall(mod, ctx):
    ctx.txn.delete(".adds/kobo-aio/scripts/owner.sh")
    ctx.txn.delete(f"{NM_DIR}/kobo-aio-owner")


register(Module(
    id="nm.owner", category="Menú", title="Tarjeta del propietario",
    description="Muestra tus datos de contacto desde NickelMenu por si pierdes el lector. Comparte solo lo "
                "mínimo (por ejemplo, un correo alternativo).",
    risk="bajo", requires=["mod.nickelmenu"],
    options=[Option("name", "Nombre", "text", ""), Option("email", "Correo", "text", ""),
             Option("phone", "Teléfono", "text", ""), Option("reward", "Recompensa", "text", ""),
             Option("note", "Nota", "text", "")],
    sources=["https://github.com/Parzval-Net/NickelMenu-Builder"], license="MIT",
    apply_fn=_owner_apply, uninstall_fn=_owner_uninstall,
    status_fn=lambda m, d: "instalado" if (d.root / NM_DIR / "kobo-aio-owner").exists() else "no instalado",
))


# -- Juegos de Kobo -----------------------------------------------------------
GAMES_REL = f"{NM_DIR}/kobo-aio-games"


def _games_apply(mod, ctx, opts):
    ids = [g for g in GAMES if g in opts["games"]]
    if not ids:
        raise AioError("Elige al menos un juego (o desinstala «Juegos de Kobo»)")
    ctx.txn.write_text(GAMES_REL, build_config(ids, "# Juegos que Kobo trae escondidos, en la Pestaña de atajos"),
                       note="juegos en la pestaña de atajos")


def _games_uninstall(mod, ctx):
    ctx.txn.delete(GAMES_REL)


register(Module(
    id="games.kobo", category="Juegos", title="Juegos ocultos de Kobo",
    description="Kobo trae juegos que no muestra en sus menús. Este módulo los agrega a la Pestaña de atajos para "
                "abrirlos con un toque: Sudoku, Solitario, Word Scramble y Unblock It. No instala nada nuevo.",
    risk="bajo", requires=["mod.nickelmenu"],
    options=[Option("games", "Juegos", "multi", list(GAMES), [(g, CATALOG_BY_ID[g].label) for g in GAMES])],
    apply_fn=_games_apply, uninstall_fn=_games_uninstall,
    status_fn=lambda m, d: "instalado" if (d.root / GAMES_REL).is_file() else "no instalado",
))
