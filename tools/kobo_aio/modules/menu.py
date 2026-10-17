"""NickelMenu menu builder: curated actions, bottom-tab layout and owner card."""

from __future__ import annotations

import re

from ..core import Module, Option, register
from ..nickelmenu import CATALOG, build_config

NM_DIR = ".adds/nm"
# Hardware ids without Dark mode (Kobo help center, via KoboPatch Web UI kobo/dark-mode.js).
NO_DARK_MODE = {"376", "377", "380", "382", "384"}


def _menu_apply(mod, ctx, opts):
    items = [i for i in opts["items"]]
    if ctx.device.model_id in NO_DARK_MODE:
        items = [i for i in items if i != "dark_mode"]
    header = []
    if opts["tab_label"]:
        label = re.sub(r"[:\t\n]", "", str(opts["tab_label"]))[:12]
        header.append(f"experimental :menu_main_15505_label :{label}")
    if opts["simple_tabs"]:
        # Tab indexes on 4.41+: 0 Home, 1 My Books, 2 Activity, 3 My Notebooks, 4 Discover, 5 More (NickelMenu doc)
        header += [
            "experimental :menu_main_15505_enabled :1",
            "experimental :menu_main_15505_0_enabled :1",
            "experimental :menu_main_15505_1_label :Libros",
            "experimental :menu_main_15505_2_enabled :1",
            "experimental :menu_main_15505_2_label :Actividad",
            f"experimental :menu_main_15505_3_enabled :{1 if opts['notebooks'] else 0}",
            f"experimental :menu_main_15505_4_enabled :{0 if opts['hide_discover'] else 1}",
            "experimental :menu_main_15505_5_enabled :1",
        ]
    ctx.txn.write_text(f"{NM_DIR}/kobo-aio-menu", build_config(items, "\n".join(header)), note="menú NickelMenu")
    legacy = f"{NM_DIR}/webui-preset"
    if opts["replace_kp_preset"] and ctx.txn.exists(legacy):
        # Old KoboPatch Web UI preset duplicates these items and fights over the same experimental keys.
        ctx.txn.write_bytes(".adds/kobo-aio/disabled/webui-preset", ctx.txn.read_bytes(legacy),
                            note="copia del preset anterior (para restaurarlo si quieres)")
        ctx.txn.delete(legacy, note="preset antiguo de KoboPatch Web UI reemplazado por el menú del AIO")


def _menu_uninstall(mod, ctx):
    ctx.txn.delete(f"{NM_DIR}/kobo-aio-menu")


_choices = [(i.id, f"[{i.group}] {i.label}" + (" (riesgo alto)" if i.risk == "alto" else "")) for i in CATALOG]
register(Module(
    id="nm.menu", category="Menú", title="Menú rápido de NickelMenu",
    description="Atajos validados contra la documentación oficial: suspender, reiniciar, importar libros, modo "
                "oscuro, WiFi, capturas, juegos ocultos, búsqueda en RAE/Wikipedia y respaldo de la base de datos.",
    risk="bajo", recommended=True, requires=["mod.nickelmenu"],
    options=[
        Option("items", "Atajos", "multi", [i.id for i in CATALOG if i.default], _choices),
        Option("tab_label", "Nombre de la pestaña NickelMenu (máx. 12)", "text", "Kobo AIO"),
        Option("simple_tabs", "Reordenar pestañas inferiores", "bool", False),
        Option("notebooks", "Mostrar Cuadernos", "bool", False),
        Option("hide_discover", "Ocultar Descubrir (tienda)", "bool", True),
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
