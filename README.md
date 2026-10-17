# Kobo AIO

Todo en uno para lectores **Kobo**: mods de Nickel, apps, fuentes, menús, parches de firmware, cómics, audiolibros, diccionarios, el emulador **Ink-boy** y mantenimiento. Todo con respaldo automático y vuelta atrás.

**Interfaz web:** https://parzval-net.github.io/AIO_KOBO_APPS/ (se conecta al motor local de tu computador; no sube nada a internet)

> Probado en Kobo Libra Colour (N428, firmware 4.45.23697) y validado en simulación con los 29 modelos Kobo y 4 líneas de firmware (116 combinaciones, `make matrix`). El catálogo se adapta solo a cada modelo y firmware.

---

## Empezar

Hay dos formas. En las dos, conecta el Kobo por USB y toca **Conectar** en su pantalla.

**A. Desde el navegador, sin instalar nada.** Abre la [página](https://parzval-net.github.io/AIO_KOBO_APPS/), toca **Conectar desde el navegador** y elige la unidad `KOBOeReader`.
- **Chrome y Edge** escriben directo en el Kobo. La primera vez que apliques un cambio te piden una carpeta de tu computador para guardar los respaldos.
- **Safari y Firefox** solo pueden leer el Kobo: Kobo AIO te entrega un único `KoboRoot.tgz` para copiar dentro de `.kobo/`, y el Kobo lo instala al reiniciar.

**B. App de escritorio (todas las funciones, sin instalar Python).** Un solo archivo con todo incluido (Python, Pillow y PyMuPDF), en la [última versión](https://github.com/Parzval-Net/AIO_KOBO_APPS/releases/latest):

| Sistema | Archivo | Primera vez |
|---|---|---|
| Windows | `Kobo-AIO-windows-x64.exe` | Si aparece «Windows protegió tu PC»: «Más información» > «Ejecutar de todas formas» |
| Mac con chip Apple | `Kobo-AIO-macos-arm64.zip` | Doble clic en «Kobo AIO». Si macOS no puede comprobar el desarrollador: Ajustes del Sistema > Privacidad y seguridad > «Abrir igualmente» |
| Mac con Intel | `Kobo-AIO-macos-x64.zip` | Igual que arriba |
| Linux (x64) | `Kobo-AIO-linux-x64.tar.gz` | Descomprime y ejecuta `./Kobo-AIO` |

La app no está firmada con un certificado pagado de Apple o Microsoft; por eso el sistema pide confirmar la primera vez. Cada versión publica `SHA256SUMS.txt`. Al abrirla se abre `http://127.0.0.1:8765`, y la página de GitHub Pages la detecta y ofrece cambiar a ella sola.

**C. Desde el código (Python 3.9+).** `cd tools && python3 -m kobo_aio serve`, o doble clic en `Kobo AIO.command` en Mac. Requiere Pillow (`pip3 install Pillow`).

| Función | Navegador | App de escritorio |
|---|---|---|
| Mods de Nickel, menú de atajos, fuentes, ajustes, KOReader, Cadmus y plugins | Sí | Sí |
| Diagnóstico, simulación previa y respaldo de cada cambio | Sí | Sí |
| Cómics (CBZ, ZIP, EPUB, imágenes), audiolibros MP3, pantallas de reposo | Sí | Sí |
| Revertir una instalación | Chrome y Edge | Sí |
| Cómics CBR, RAR, 7Z y PDF; audiolibros M4B | No | Sí (con `unar`, `pymupdf`, `ffmpeg`) |
| Ajustes avanzados del sistema (kobopatch), EPUB a KEPUB, respaldo de toda la biblioteca, descarga de diccionarios, Ink-boy, audiobook.koplugin | No | Sí |

En el navegador, cada función que necesita la app de escritorio lo indica ahí mismo, con la descarga para tu sistema.

## Qué incluye

| Área | Módulos |
|---|---|
| **Interfaz** | Asistente «Mejorar mi Kobo» en 3 pasos con perfiles (Esencial, Minimalista, Lectura cuidada, Todo incluido), lenguaje simple, modo claro/oscuro con contraste WCAG 2.2 AA y modo experto con buscador |
| **Se adapta a tu modelo** | Reconoce los 29 modelos Kobo (Touch a Libra Colour): ilustración con la proporción real, botones y lápiz; capacidades (resolución, color, Bluetooth, modo oscuro); oculta lo incompatible por hardware o firmware y muestra vistas previas ilustrativas en color o grises según la pantalla |
| **Diagnóstico** | Detecta modelo y firmware (avisa 4.46 y bloquea 5.x/6.x), `ExcludeSyncFolders` corrupto, libros de Calibre que faltan, integridad de `KoboReader.sqlite` (sobre una copia), basura de macOS, Spotlight, espacio libre y errores en configs de NickelMenu: sintaxis según la spec oficial, atajos a scripts inexistentes, duplicados, claves en conflicto y servicios sin contraseña |
| **Sistema** | NickelMenu 0.6.0 · Inicio minimalista (NickelHome) · Gestor de mods en el propio Kobo (NickelModManager) · **Parches de firmware con kobopatch** (interlineado, márgenes, fuentes, encabezados, suspensión, bloqueo de actualizaciones…), con opción de restaurar los originales |
| **Lectura** | Fuentes para e-reader (ebook-fonts, 44 familias) · Tipografía mejorada (NickelTypeFix 1.0) · Reloj y batería (NickelClock) · Animación de página (NickelDissolve, MTK) |
| **Menú** | Atajos validados (suspender, reiniciar, importar libros, modo oscuro, WiFi, capturas, juegos ocultos, búsqueda en RAE/Wikipedia/traductor, respaldo de la base de datos, ReKindle) · **Nombre e ícono de la pestaña** (8 íconos con el mismo tamaño y estilo que los de Kobo, o tu propia imagen) · Reorden de pestañas · Tarjeta del propietario |
| **Apps** | KOReader + plugins (Project: Title, App Store, sincronización con Kobo, Hardcover, LocalSend) · Cadmus 0.12 · **Audiolibros** (formato `.mp3z` del lector stock o audiobook.koplugin en KOReader) |
| **Juegos** | Ink-boy: Game Boy y Game Boy Color nativo para e-ink (color Kaleido 3, 4 grises en Carta, modo turbo A2) |
| **Biblioteca** | **Conversor de cómics y manga estilo KCC** para CBZ, ZIP, CBR, RAR, CB7, 7Z, CBT, TAR, PDF, EPUB, imágenes sueltas, carpetas y lotes, ajustado a cada panel (recorte, páginas dobles, color Kaleido, KEPUB de página fija o CBZ) · Envío de libros con EPUB a KEPUB (kepubify) · **Armador de audiolibros**: `.mp3z` para el lector stock (partes cortas y portada) o MP3 con capítulos para KOReader · Diccionarios Wiktionary o propios · Pantallas de reposo |
| **Ajustes** | ExcludeSyncFolders bien escapado · Capturas, auto USB, apagado, horizontal · Modo sideload |
| **Mantenimiento** | Respaldo de libros, progreso y mods al computador · **Dejar de fábrica**: quita todos los mods (también los de otras herramientas), apps, fuentes, diccionarios y ajustes de mods, con respaldo y vuelta atrás, y guía el restablecimiento de fábrica del Kobo para venderlo o regalarlo · Limpieza de `._*` y `.DS_Store` (valida la firma AppleDouble) · Desactivar Spotlight |

## Seguridad

- **Nunca toca tus libros ni la base de datos.** Solo escribe en rutas de mods (`.adds/`, `fonts/`, `.kobo/KoboRoot.tgz`, `.kobo/custom-dict/`, `.kobo/screensaver/`, una clave puntual de `Kobo eReader.conf`). Los libros nuevos se agregan sin sobrescribir nada.
- **Respaldo y rollback.** Antes de cambiar un archivo se guarda el original en `~/Kobo-AIO/backups/<serie>/<fecha>/` y cada cambio se anota en un journal. Puedes revertir una instalación aunque se haya cortado a mitad de camino (pestaña *Respaldos* o `python3 -m kobo_aio rollback`).
- **Simulación primero.** La UI muestra la lista exacta de cambios antes de escribir.
- **Descargas fijadas por SHA-256.** Todos los mods, apps y el firmware oficial se verifican. Si un hash no coincide, la instalación se aborta.
- **`KoboRoot.tgz` validado.** Los mods solo pueden instalar en `usr/local/Kobo/imageformats/` y `.adds/`. Los parches de firmware solo pueden reemplazar los 4 binarios objetivo, con el mismo tamaño que el original y ELF ARM válido. Si queda un `KoboRoot.tgz` pendiente, se fusiona en vez de perderse.
- **Bloqueo por firmware.** Los mods de Nickel se desactivan en firmware 5.x/6.x (Qt6), porque ahí no funcionan.
- **Modo navegador con las mismas reglas.** Es el mismo motor Python, ejecutado en la página con [Pyodide](https://pyodide.org) 0.27.7 (alojado junto a la página y fijado por SHA-256). Trabaja sobre una copia en memoria del Kobo: lee cada archivo solo cuando lo necesita, no sube nada a internet y no escribe en el Kobo hasta que la corrida terminó bien y el respaldo quedó guardado en tu computador. Si una escritura falla, devuelve lo ya escrito a como estaba.
- **Motor local acotado.** Escucha solo en `127.0.0.1`, rechaza DNS rebinding y orígenes ajenos (solo acepta la propia UI y GitHub Pages), y exige un header propio en cada operación de escritura.

## Validado con ingeniería inversa (REA + Ghidra)

- Los 7 mods de Nickel y Ink-boy son ELF ARM 32 bits EABI5 hard-float; los mods enlazan Qt5, por eso se bloquean en firmware 5.x/6.x (Qt6).
- Rutas y marcadores de desinstalación confirmados en los binarios (por ejemplo `nh_info` de NickelHome: crear `.adds/nickel-home/uninstall`; nickel-screensaver: `.adds/screensaver/uninstall.txt`; NickelModManager: `.adds/nickel-mod-manager`).
- Lector de audiolibros stock (`libmp3z.so`): orden, búsqueda de pistas, marcador y portada, ver *Notas*.
- Íconos de la barra inferior de Nickel (recursos Qt del ejecutable `nickel` 4.45): PNG de 50×50 en gris con transparencia, contorno en reposo y relleno activo; los íconos del AIO siguen ese formato y se guardan fuera de `.adds/nm` porque NickelMenu lee como configuración todo lo que hay ahí.
- Desinstalación de cada mod de Nickel leída de su `nh_info` (NickelHook): NickelMenu, NickelHome y nickel-screensaver se quitan al crear su marca; NickelTypeFix, NickelClock, NickelDissolve y NickelModManager, al borrarla. NickelModManager guarda copias de los mods y las repone tras actualizar el sistema, por eso «Dejar de fábrica» también las elimina.
- Parche de firmware verificado byte a byte: solo cambia `ReadingSettings::lineHeightScalars()`, `ReadingFooter::update(...)` y la URL de actualización, según los parches elegidos.

## Línea de comandos

```bash
cd tools
python3 -m kobo_aio doctor                     # diagnóstico (solo lectura)
python3 -m kobo_aio catalog                    # módulos disponibles para el Kobo conectado
python3 -m kobo_aio plan  --select mod.nickelhome,fonts.ebook
python3 -m kobo_aio apply --select lib.comics --opt lib.comics.files="~/Comics/Berserk 01.cbz" --opt lib.comics.manga=true
python3 -m kobo_aio backups
python3 -m kobo_aio rollback ~/Kobo-AIO/backups/<serie>/<fecha>/manifest.json
```

## Notas y límites conocidos

- **Audiolibros en el lector stock (`.mp3z`):** formato ZIP sin compresión de pistas MP3 que lee `libmp3z.so` del firmware. Validado con REA sobre 4.45.23697: el título sale del nombre del archivo, cada pista es una «Page N» (texto del firmware), el orden es alfabético, la posición se guarda por pista (no por minuto) y un libro de una sola pista se marca como terminado al guardar el marcador. Por eso el AIO divide en partes de 10 min, usa nombres ASCII con numeración fija (el lector busca la pista con `toLatin1()` y falla con tildes) y pone la portada en cada pista (el reproductor la muestra). Sin autor ni portada en la biblioteca. Requiere Bluetooth. Pendiente de prueba de reproducción en el equipo.
- **Audiolibros en KOReader:** MP3 único con capítulos ID3 (CHAP/CTOC), portada y posición exacta con audiobook.koplugin. Usa solo audio sin DRM que te pertenezca.
- **Parches de firmware:** por ahora solo para Libra Colour 4.45.23697 (kobopatch oficial para Windows, macOS y Linux). Se pierden al actualizar el firmware. Si algo falla, usa *Restaurar originales* o reinstala el firmware.
- **ReKindle** (CC BY-NC-SA) se abre como enlace en el navegador de Nickel; no se redistribuye. No inicies sesión con cuentas de trabajo.
- **Diccionarios oficiales** (es, es-en, en-es): son gratis desde el propio Kobo con WiFi y no se redistribuyen.

## Desarrollo

```bash
make aio-test          # tests del AIO (Python, sin dependencias externas salvo Pillow)
make site              # arma _site/ como GitHub Pages: web + motor + Pyodide + mods verificados (~130 MB)
make app               # app de escritorio para este sistema en _app/dist (requiere pip install -r packaging/requirements.txt)
make matrix            # valida los 29 modelos Kobo x 4 firmwares (4.6, 4.38, 4.45, 6.0) con instalación real simulada
make test              # Ink-boy: suites C++, simulador y tests del AIO
make kobo              # compila Ink-boy para ARM (zig)
```

Estructura: `tools/kobo_aio/` (motor, módulos, servidor; `web.py` es el puente del modo navegador), `web/` (interfaz y `aio-worker.js`, publicados en GitHub Pages por `tools/build_site.py`), `src/` e `include/` (Ink-boy), `tests/`. Documentación de Ink-boy en [docs/INKBOY.md](docs/INKBOY.md).

## Créditos

Construido sobre el trabajo de la comunidad Kobo. Gracias a: [NickelMenu](https://github.com/pgaskin/NickelMenu), [kobopatch](https://github.com/pgaskin/kobopatch), [kepubify](https://github.com/pgaskin/kepubify) y [dictutil](https://pgaskin.net/dictutil/) (pgaskin) · [KoboPatch Web UI](https://github.com/nicoverbruggen/kobopatch-webui), NickelHome, NickelTypeFix, NickelDissolve y [ebook-fonts](https://github.com/nicoverbruggen/ebook-fonts) (Nico Verbruggen) · [NickelClock](https://github.com/shermp/NickelClock) (shermp) · [nickel-screensaver](https://github.com/redphx/nickel-screensaver) (redphx) · [KOReader](https://github.com/koreader/koreader) · [Cadmus](https://github.com/OGKevin/cadmus) · [audiobook.koplugin](https://github.com/stradichenko/audiobook.koplugin) · [KCC](https://github.com/ciromattia/kcc) (referencia del conversor) · [reader-dict](https://www.reader-dict.com) · [FBInk](https://github.com/NiLuJe/FBInk) (NiLuJe) · [Pyodide](https://pyodide.org) (MPL-2.0) · Peanut-GB / Walnut-CGB · los parches de MobileRead (GeoffR, jackie_w, JSWolf y otros) · [NickelMenu Builder](https://github.com/Parzval-Net/NickelMenu-Builder).

Kobo es marca de Rakuten Kobo Inc. Este proyecto no está afiliado ni respaldado por Kobo. Licencias: ver [LICENSE](LICENSE) y [NOTICE.md](NOTICE.md).
