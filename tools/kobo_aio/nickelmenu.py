"""NickelMenu config catalog, generator and linter.

Spec source: https://github.com/pgaskin/NickelMenu/blob/master/res/doc (official NickelMenu docs).
Action library adapted from NickelMenu-Builder (MIT, https://github.com/Parzval-Net/NickelMenu-Builder),
validated against the official spec: invalid entries such as "nickel_setting:toggle:wifi" were fixed.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

LOCATIONS = {"main", "reader", "browser", "library", "selection", "selection_search"}
ACTIONS = {
    "cmd_spawn", "cmd_output", "dbg_syslog", "dbg_error", "dbg_msg", "dbg_toast", "kfmon", "nickel_setting",
    "nickel_extras", "nickel_browser", "nickel_misc", "nickel_open", "nickel_wifi", "nickel_bluetooth",
    "nickel_screenshot", "nickel_orientation", "power", "skip",
}
SETTING_ACTIONS = {"toggle", "enable", "disable"}
SETTINGS = {"invert", "dark_mode", "lockscreen", "screenshots", "force_wifi", "auto_usb_gadget"}
ENUM_ARGS = {
    "nickel_extras": {"unblock_it", "sketch_pad", "solitaire", "sudoku", "word_scramble"},
    "nickel_misc": {"home", "force_usb_connection", "rescan_books", "rescan_books_full"},
    "nickel_open": {"discover:storefront", "discover:wishlist", "library:library", "library:all", "library:authors",
                    "library:series", "library:shelves", "library:pocket", "library:instapaper", "library:dropbox",
                    "library:gdrive", "reading_life:reading_life", "reading_life:stats", "reading_life:awards",
                    "reading_life:words", "store:overdrive", "store:search"},
    "nickel_wifi": {"autoconnect", "autoconnect_silent", "enable", "disable", "toggle"},
    "nickel_orientation": {"portrait", "landscape", "inverted_portrait", "inverted_landscape", "invert", "swap"},
    "power": {"shutdown", "reboot", "sleep"},
    "nickel_bluetooth": {"enable", "disable", "toggle", "check", "scan"},
    "nickel_screenshot": {"capture"},
}
EXPERIMENTAL_RE = re.compile(r"^menu_main_15505(_\d+)?_(enabled|label|icon|icon_active)$|^menu_main_15505_default$"
                             r"|^hide_home_\w+$")


@dataclass
class NmItem:
    id: str
    group: str
    label: str
    location: str
    action: str
    arg: str
    chain: list[tuple[str, str, str]] = field(default_factory=list)  # (chain_kind, action, arg)
    description: str = ""
    risk: str = "bajo"
    default: bool = False

    def render(self, label: str | None = None) -> str:
        lines = [f"menu_item :{self.location} :{label or self.label} :{self.action} :{self.arg}"]
        lines += [f"  {kind} :{act} :{arg}" for kind, act, arg in self.chain]
        return "\n".join(lines)


def _i(id, group, label, loc, action, arg, desc="", risk="bajo", default=False, chain=None):
    return NmItem(id, group, label, loc, action, arg, chain or [], desc, risk, default)


# Curated, spec-validated action library (labels in Spanish; ':' is forbidden in labels).
CATALOG: list[NmItem] = [
    # Energía
    _i("sleep", "Energía", "Suspender", "main", "power", "sleep", "Suspende el lector al instante.", default=True),
    _i("reboot", "Energía", "Reiniciar", "main", "power", "reboot", "Reinicio ordenado de Nickel y el sistema.", default=True),
    _i("shutdown", "Energía", "Apagar", "main", "power", "shutdown", "Apagado ordenado.", default=True),
    # Biblioteca
    _i("rescan_full", "Biblioteca", "Importar libros nuevos", "library", "nickel_misc", "rescan_books_full",
       "Ciclo USB completo: importa libros copiados sin desconectar.", default=True),
    _i("rescan", "Biblioteca", "Reescanear (rápido)", "library", "nickel_misc", "rescan_books",
       "Reescaneo liviano de la biblioteca."),
    _i("open_authors", "Biblioteca", "Autores", "main", "nickel_open", "library:authors", "Abre la vista de autores."),
    _i("open_series", "Biblioteca", "Series", "main", "nickel_open", "library:series", "Abre la vista de series."),
    _i("open_shelves", "Biblioteca", "Colecciones", "main", "nickel_open", "library:shelves", "Abre colecciones."),
    _i("stats", "Biblioteca", "Estadísticas", "main", "nickel_open", "reading_life:stats", "Estadísticas de lectura."),
    _i("words", "Biblioteca", "Mis palabras", "main", "nickel_open", "reading_life:words", "Palabras buscadas."),
    # Lectura
    _i("dark_mode", "Lectura", "Modo oscuro", "reader", "nickel_setting", "toggle:dark_mode",
       "Alterna el modo oscuro del lector.", default=True),
    _i("orient_invert", "Lectura", "Invertir orientación", "reader", "nickel_orientation", "invert",
       "Gira la pantalla 180° (útil con botones físicos)."),
    _i("orient_swap", "Lectura", "Vertical / horizontal", "reader", "nickel_orientation", "swap",
       "Cambia entre vertical y horizontal."),
    _i("screenshot", "Lectura", "Captura de pantalla", "reader", "nickel_screenshot", "capture",
       "Guarda una captura en la raíz del Kobo."),
    # Sistema
    _i("invert", "Sistema", "Invertir pantalla", "main", "nickel_setting", "toggle:invert",
       "Invierte colores (requiere reinicio en firmware 4.28+).", chain=[("chain_success", "power", "reboot")]),
    _i("lockscreen", "Sistema", "Pantalla de bloqueo", "main", "nickel_setting", "toggle:lockscreen",
       "Alterna el código de desbloqueo."),
    _i("usb", "Sistema", "Conectar USB", "main", "nickel_misc", "force_usb_connection",
       "Muestra el diálogo de conexión USB."),
    # Only shows inside the web browser's own menu. Every browser shortcut here opens it "modal" (with its own close
    # button), so this is only needed if you open the browser some other way (NickelMenu doc: 4.23.15505+).
    _i("home", "Extras", "Salir del navegador", "browser", "nickel_misc", "home",
       "Aparece solo en el menú del navegador web y vuelve al inicio. No hace falta con los atajos de navegador de "
       "este menú, que ya se abren en una ventana con botón para cerrar."),
    # Red
    _i("wifi_toggle", "Red", "WiFi on/off", "main", "nickel_wifi", "toggle", "Alterna el WiFi.", default=True),
    _i("wifi_connect", "Red", "Conectar WiFi", "main", "nickel_wifi", "autoconnect", "Enciende y conecta a red conocida."),
    _i("bt_toggle", "Red", "Bluetooth on/off", "main", "nickel_bluetooth", "toggle", "Alterna Bluetooth (4.34+)."),
    _i("ip", "Red", "Dirección IP", "main", "cmd_output",
       "500:/sbin/ifconfig | /usr/bin/awk '/inet addr/{print substr($2,6)}'", "Muestra la IP del lector."),
    # Extras de Nickel
    _i("sketch", "Extras", "Bloc de dibujo", "main", "nickel_extras", "sketch_pad", "Bloc de dibujo oculto."),
    _i("sudoku", "Extras", "Sudoku", "main", "nickel_extras", "sudoku", "Sudoku integrado."),
    _i("solitaire", "Extras", "Solitario", "main", "nickel_extras", "solitaire", "Solitario integrado."),
    _i("scramble", "Extras", "Word Scramble", "main", "nickel_extras", "word_scramble", "Juego de palabras."),
    _i("unblock", "Extras", "Unblock It", "main", "nickel_extras", "unblock_it", "Juego de bloques."),
    _i("browser", "Extras", "Navegador", "main", "nickel_browser", "modal", "Navegador en ventana."),
    _i("wikipedia", "Extras", "Wikipedia", "main", "nickel_browser", "modal:https://es.m.wikipedia.org/"),
    _i("rekindle", "Extras", "ReKindle (apps web)", "main", "nickel_browser", "modal:https://rekindle.ink/",
       "Panel web para e-ink: notas, tareas, calendario y juegos (rekindle.ink, CC BY-NC-SA). No inicies "
       "sesión con cuentas de trabajo."),
    # Selección de texto
    _i("sel_rae", "Selección", "Buscar en la RAE", "selection_search", "nickel_browser",
       "modal:https://dle.rae.es/{1|S|%}", "Busca la selección en el diccionario RAE."),
    _i("sel_wiki", "Selección", "Buscar en Wikipedia", "selection_search", "nickel_browser",
       "modal:https://es.m.wikipedia.org/wiki/Special:Search?search={1|S|%}", "Busca la selección en Wikipedia."),
    _i("sel_translate", "Selección", "Traducir (Google)", "selection_search", "nickel_browser",
       "modal:https://translate.google.com/m?sl=auto&tl=es&q={1|S|%}", "Traduce la selección al español."),
    # Diagnóstico
    _i("kernel", "Diagnóstico", "Versión del kernel", "main", "cmd_output", "500:uname -a"),
    _i("mem", "Diagnóstico", "Memoria libre", "main", "cmd_output", "500:free -m"),
    _i("syslog", "Diagnóstico", "Guardar syslog", "main", "cmd_spawn",
       "quiet:logread > /mnt/onboard/.adds/kobo-aio/syslog.txt",
       "Guarda el log del sistema en .adds/kobo-aio/syslog.txt.",
       chain=[("chain_success", "dbg_toast", "Syslog guardado en .adds/kobo-aio")]),
    _i("backup_db", "Diagnóstico", "Respaldar base de datos", "main", "cmd_spawn",
       "quiet:mkdir -p /mnt/onboard/.adds/kobo-aio/db-backup && cp /mnt/onboard/.kobo/KoboReader.sqlite "
       "/mnt/onboard/.adds/kobo-aio/db-backup/KoboReader-$(date +%Y%m%d-%H%M).sqlite",
       "Copia KoboReader.sqlite a .adds/kobo-aio/db-backup (progreso, notas, colecciones).",
       chain=[("chain_success", "dbg_toast", "Base de datos respaldada"),
              ("chain_failure", "dbg_toast", "Error al respaldar la base de datos")]),
    # Red avanzada (riesgo: servicios sin autenticación en la red WiFi)
    _i("ftp", "Avanzado", "Servidor FTP (puerto 1021)", "main", "cmd_spawn",
       'quiet:/usr/bin/pkill -f "^/usr/bin/tcpsvd -E 0.0.0.0 1021" || true && exec /usr/bin/tcpsvd -E 0.0.0.0 '
       "1021 /usr/sbin/ftpd -w -t 30 /mnt/onboard",
       "FTP SIN contraseña a todo el almacenamiento. Úsalo solo en redes de confianza.", risk="alto",
       chain=[("chain_success", "dbg_toast", "FTP activo en el puerto 1021")]),
]
CATALOG_BY_ID = {i.id: i for i in CATALOG}
# Former toggles now set once in "Ajustes ocultos del Kobo" (conf.preferences); old selections ignore them.
RETIRED = ("screenshots_toggle", "auto_usb")


def build_config(item_ids: list[str], header: str = "") -> str:
    lines = ["# Generado por Kobo AIO. No lo edites: se regenera al reinstalar el módulo.",
             "# Spec: https://github.com/pgaskin/NickelMenu/blob/master/res/doc"]
    if header:
        lines.append(header)
    for iid in item_ids:
        item = CATALOG_BY_ID.get(iid)
        if item:
            lines.append(item.render())
    return "\n".join(lines) + "\n"


def lint_line(line: str) -> str | None:
    """Return an error message for an invalid NickelMenu config line, or None if it is valid."""
    s = line.strip()
    if not s or s.startswith("#"):
        return None
    parts = [p.strip() for p in s.split(":")]
    kind = parts[0]
    if kind == "menu_item":
        if len(parts) < 5:
            return "menu_item necesita ubicación, etiqueta, acción y argumento"
        loc, label, action = parts[1], parts[2], parts[3]
        arg = ":".join(parts[4:]).strip()
        if loc not in LOCATIONS:
            return f"ubicación inválida '{loc}'"
        if not label:
            return "etiqueta vacía"
        return _lint_action(action, arg)
    if kind in ("chain_success", "chain_failure", "chain_always"):
        if len(parts) < 3:
            return f"{kind} necesita acción y argumento"
        return _lint_action(parts[1], ":".join(parts[2:]).strip())
    if kind == "experimental":
        if len(parts) < 3:
            return "experimental necesita clave y valor"
        return None if EXPERIMENTAL_RE.match(parts[1]) else f"opción experimental desconocida '{parts[1]}' (se ignora)"
    if kind == "generator":
        return None if len(parts) >= 3 and parts[1] in LOCATIONS else "generator inválido"
    return f"directiva desconocida '{kind}'"


def _lint_action(action: str, arg: str) -> str | None:
    if action not in ACTIONS:
        return f"acción desconocida '{action}'"
    norm = ":".join(p.strip() for p in arg.split(":"))
    if action == "nickel_setting":
        p = norm.split(":")
        if len(p) != 2 or p[0] not in SETTING_ACTIONS or p[1] not in SETTINGS:
            return f"nickel_setting inválido '{arg}' (usa toggle|enable|disable : {'|'.join(sorted(SETTINGS))})"
    elif action in ENUM_ARGS and norm not in ENUM_ARGS[action] and action != "nickel_extras":
        return f"argumento inválido para {action}: '{arg}'"
    elif action == "cmd_output":
        m = re.match(r"^(\d+):", norm)
        if not m or not (0 < int(m.group(1)) < 10000):
            return "cmd_output necesita timeout en ms (1-9999) antes del comando"
    elif action == "skip":
        if not re.match(r"^-?\d+$", norm):
            return "skip necesita un número"
    return None


def _issue(file, n, text, error, level="warn", title="", hint=""):
    return {"file": file, "line": n, "text": text, "error": error, "level": level,
            "title": title or "Hay un atajo del menú con problemas", "hint": hint}


def lint_dir(nm_dir: Path, onboard_root: Path | None = None) -> list[dict]:
    """Lint every NickelMenu config file.

    Syntax is checked against the official spec; on top of that it looks for problems users actually hit:
    scripts that do not exist on the device, duplicated labels, experimental keys declared twice (only the
    first one wins), services without a password, KFMon actions without KFMon and invert without reboot.
    """
    issues: list[dict] = []
    seen_exp: dict[str, str] = {}
    labels: dict[tuple[str, str], str] = {}
    if not nm_dir.is_dir():
        return issues
    onboard_root = onboard_root or nm_dir.parent.parent
    for f in sorted(nm_dir.iterdir()):
        if not f.is_file() or f.name.startswith(".") or f.name in ("doc", "uninstall") or f.suffix in (".png", ".sh"):
            continue
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        last_item = None
        for n, line in enumerate(text.splitlines(), 1):
            raw = line.strip()
            err = lint_line(line)
            if err:
                issues.append(_issue(f.name, n, raw, err, title="Un atajo del menú tiene un error",
                                     hint="NickelMenu mostrará un mensaje de error en vez del atajo."))
                continue
            parts = [p.strip() for p in raw.split(":")]
            if not parts or raw.startswith("#"):
                continue
            kind = parts[0]
            if kind == "experimental" and len(parts) >= 3:
                key = parts[1]
                if key in seen_exp and seen_exp[key] != f.name:
                    issues.append(_issue(f.name, n, raw, f"'{key}' ya está definido en {seen_exp[key]}; NickelMenu usa solo el primero",
                                         title="Dos configuraciones del menú definen lo mismo",
                                         hint="El Kobo usa solo la primera; conviene dejar una sola."))
                seen_exp.setdefault(key, f.name)
                continue
            if kind == "menu_item" and len(parts) >= 5:
                loc, label, action, arg = parts[1], parts[2], parts[3], ":".join(parts[4:]).strip()
                last_item = (action, arg)
                key = (loc, label.lower())
                if key in labels and labels[key] != f"{f.name}:{n}":
                    issues.append(_issue(f.name, n, raw, f"La etiqueta «{label}» ya existe en {labels[key]}", level="info",
                                         title=f"Atajo repetido: «{label}»", hint="Aparecerá dos veces en el menú."))
                labels.setdefault(key, f"{f.name}:{n}")
            elif kind.startswith("chain_") and len(parts) >= 3:
                action, arg = parts[1], ":".join(parts[2:]).strip()
            else:
                continue
            if action in ("cmd_spawn", "cmd_output"):
                for path in re.findall(r"/mnt/onboard/[^\s'\";|&]+", arg):
                    rel = path[len("/mnt/onboard/"):]
                    if rel.endswith((".sh", ".lua")) or "/scripts/" in rel or rel.endswith("koreader.sh"):
                        if not (onboard_root / rel).exists():
                            issues.append(_issue(f.name, n, raw, f"No existe {path}",
                                                 title="Un atajo apunta a algo que no está instalado",
                                                 hint="Al tocarlo no pasará nada o dará error. Reinstala la app o quita el atajo."))
                if "telnetd" in arg or "ftpd" in arg:
                    issues.append(_issue(f.name, n, raw, "Servicio de red sin contraseña", level="info",
                                         title="Un atajo abre acceso sin contraseña",
                                         hint="Cualquiera en tu misma red WiFi podría entrar al Kobo mientras esté activo."))
            if action == "kfmon" and not (onboard_root / ".adds" / "kfmon").exists():
                issues.append(_issue(f.name, n, raw, "KFMon no está instalado",
                                     title="Un atajo necesita KFMon", hint="Instala KFMon o quita el atajo."))
            if kind == "menu_item" and action == "nickel_setting" and arg.replace(" ", "").endswith("invert"):
                following = text.splitlines()[n:n + 3]
                if not any("power" in x and "reboot" in x for x in following):
                    issues.append(_issue(f.name, n, raw, "invert requiere reinicio en firmware 4.28+", level="info",
                                         title="«Invertir pantalla» necesita reiniciar",
                                         hint="Agrega un reinicio en cadena o reinicia a mano después de usarlo."))
    return issues
