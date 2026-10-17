# Kobo AIO

Todo en uno para lectores **Kobo**: mods de Nickel, apps, fuentes, menús, parches de firmware, cómics, audiolibros, diccionarios, el emulador **Ink-boy** y mantenimiento. Todo con respaldo automático y vuelta atrás.

**Interfaz web:** https://parzval-net.github.io/AIO_KOBO_APPS/ (se conecta al motor local de tu computador; no sube nada a internet)

> Probado en Kobo Libra Colour (N428, firmware 4.45.23697) y validado en simulación con los 29 modelos Kobo y 4 líneas de firmware (116 combinaciones, `make matrix`). El catálogo se adapta solo a cada modelo y firmware.

---

## Empezar

1. Descarga este repo ([ZIP](https://github.com/Parzval-Net/AIO_KOBO_APPS/archive/refs/heads/main.zip)) o clónalo.
2. **macOS:** doble clic en `Kobo AIO.command`. La primera vez: clic derecho > Abrir.
   **Linux/Windows:** `cd tools && python3 -m kobo_aio serve`
3. Conecta el Kobo por USB y toca **Conectar**. Se abre `http://127.0.0.1:8765`. También puedes usar la página de GitHub Pages.

Requisitos: Python 3.9+ y Pillow (`pip3 install Pillow`; el lanzador lo instala si falta).

## Qué incluye

| Área | Módulos |
|---|---|
| **Interfaz** | Asistente «Mejorar mi Kobo» en 3 pasos con perfiles (Esencial, Minimalista, Lectura cuidada, Todo incluido), lenguaje simple, modo claro/oscuro con contraste WCAG 2.2 AA y modo experto con buscador |
| **Se adapta a tu modelo** | Reconoce los 29 modelos Kobo (Touch a Libra Colour): ilustración con la proporción real, botones y lápiz; capacidades (resolución, color, Bluetooth, modo oscuro); oculta lo incompatible por hardware o firmware y muestra vistas previas ilustrativas en color o grises según la pantalla |
| **Diagnóstico** | Detecta modelo y firmware (avisa 4.46 y bloquea 5.x/6.x), `ExcludeSyncFolders` corrupto, libros de Calibre que faltan, integridad de `KoboReader.sqlite` (sobre una copia), basura de macOS, Spotlight, espacio libre y errores en configs de NickelMenu: sintaxis según la spec oficial, atajos a scripts inexistentes, duplicados, claves en conflicto y servicios sin contraseña |
| **Sistema** | NickelMenu 0.6.0 · Inicio minimalista (NickelHome) · Gestor de mods en el propio Kobo (NickelModManager) · **Parches de firmware con kobopatch** (interlineado, márgenes, fuentes, encabezados, suspensión, bloqueo de actualizaciones…), con opción de restaurar los originales |
| **Lectura** | Fuentes para e-reader (ebook-fonts, 44 familias) · Tipografía mejorada (NickelTypeFix 1.0) · Reloj y batería (NickelClock) · Animación de página (NickelDissolve, MTK) |
| **Menú** | Atajos validados (suspender, reiniciar, importar libros, modo oscuro, WiFi, capturas, juegos ocultos, búsqueda en RAE/Wikipedia/traductor, respaldo de la base de datos, ReKindle) · Reorden de pestañas · Tarjeta del propietario |
| **Apps** | KOReader + plugins (Project: Title, App Store, sincronización con Kobo, Hardcover, LocalSend) · Cadmus 0.12 · **Audiolibros** (formato `.mp3z` del lector stock o audiobook.koplugin en KOReader) |
| **Juegos** | Ink-boy: Game Boy y Game Boy Color nativo para e-ink (color Kaleido 3, 4 grises en Carta, modo turbo A2) |
| **Biblioteca** | **Conversor de cómics y manga estilo KCC** para CBZ, ZIP, CBR, RAR, CB7, 7Z, CBT, TAR, PDF, EPUB, imágenes sueltas, carpetas y lotes, ajustado a cada panel (recorte, páginas dobles, color Kaleido, KEPUB de página fija o CBZ) · Envío de libros con EPUB a KEPUB (kepubify) · **Armador de audiolibros**: MP3 con capítulos y portada · Diccionarios Wiktionary o propios · Pantallas de reposo |
| **Ajustes** | ExcludeSyncFolders bien escapado · Capturas, auto USB, apagado, horizontal · Modo sideload |
| **Mantenimiento** | Respaldo de libros, progreso y mods al computador · Limpieza de `._*` y `.DS_Store` (valida la firma AppleDouble) · Desactivar Spotlight |

## Seguridad

- **Nunca toca tus libros ni la base de datos.** Solo escribe en rutas de mods (`.adds/`, `fonts/`, `.kobo/KoboRoot.tgz`, `.kobo/custom-dict/`, `.kobo/screensaver/`, una clave puntual de `Kobo eReader.conf`). Los libros nuevos se agregan sin sobrescribir nada.
- **Respaldo y rollback.** Antes de cambiar un archivo se guarda el original en `~/Kobo-AIO/backups/<serie>/<fecha>/` y cada cambio se anota en un journal. Puedes revertir una instalación aunque se haya cortado a mitad de camino (pestaña *Respaldos* o `python3 -m kobo_aio rollback`).
- **Simulación primero.** La UI muestra la lista exacta de cambios antes de escribir.
- **Descargas fijadas por SHA-256.** Todos los mods, apps y el firmware oficial se verifican. Si un hash no coincide, la instalación se aborta.
- **`KoboRoot.tgz` validado.** Los mods solo pueden instalar en `usr/local/Kobo/imageformats/` y `.adds/`. Los parches de firmware solo pueden reemplazar los 4 binarios objetivo, con el mismo tamaño que el original y ELF ARM válido. Si queda un `KoboRoot.tgz` pendiente, se fusiona en vez de perderse.
- **Bloqueo por firmware.** Los mods de Nickel se desactivan en firmware 5.x/6.x (Qt6), porque ahí no funcionan.
- **Motor local acotado.** Escucha solo en `127.0.0.1`, rechaza DNS rebinding y orígenes ajenos (solo acepta la propia UI y GitHub Pages), y exige un header propio en cada operación de escritura.

## Validado con ingeniería inversa (REA + Ghidra)

- Los 7 mods de Nickel y Ink-boy son ELF ARM 32 bits EABI5 hard-float; los mods enlazan Qt5, por eso se bloquean en firmware 5.x/6.x (Qt6).
- Rutas y marcadores de desinstalación confirmados en los binarios (por ejemplo `nh_info` de NickelHome: crear `.adds/nickel-home/uninstall`; nickel-screensaver: `.adds/screensaver/uninstall.txt`; NickelModManager: `.adds/nickel-mod-manager`).
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

- **Audiolibros:** Nickel no reproduce audio propio de forma fiable. El AIO arma el MP3 (capítulos ID3 CHAP/CTOC y portada) y lo reproduces en KOReader con audiobook.koplugin. Úsalo solo con audio sin DRM que te pertenezca.
- **Parches de firmware:** por ahora solo para Libra Colour 4.45.23697 en Mac con Apple Silicon. Se pierden al actualizar el firmware. Si algo falla, usa *Restaurar originales* o reinstala el firmware.
- **ReKindle** (CC BY-NC-SA) se abre como enlace en el navegador de Nickel; no se redistribuye. No inicies sesión con cuentas de trabajo.
- **Diccionarios oficiales** (es, es-en, en-es): son gratis desde el propio Kobo con WiFi y no se redistribuyen.

## Desarrollo

```bash
make aio-test          # tests del AIO (Python, sin dependencias externas salvo Pillow)
make matrix            # valida los 29 modelos Kobo x 4 firmwares (4.6, 4.38, 4.45, 6.0) con instalación real simulada
make test              # Ink-boy: suites C++, simulador y tests del AIO
make kobo              # compila Ink-boy para ARM (zig)
```

Estructura: `tools/kobo_aio/` (motor, módulos, servidor), `web/` (interfaz, publicada en GitHub Pages), `src/` e `include/` (Ink-boy), `tests/`. Documentación de Ink-boy en [docs/INKBOY.md](docs/INKBOY.md).

## Créditos

Construido sobre el trabajo de la comunidad Kobo. Gracias a: [NickelMenu](https://github.com/pgaskin/NickelMenu), [kobopatch](https://github.com/pgaskin/kobopatch), [kepubify](https://github.com/pgaskin/kepubify) y [dictutil](https://pgaskin.net/dictutil/) (pgaskin) · [KoboPatch Web UI](https://github.com/nicoverbruggen/kobopatch-webui), NickelHome, NickelTypeFix, NickelDissolve y [ebook-fonts](https://github.com/nicoverbruggen/ebook-fonts) (Nico Verbruggen) · [NickelClock](https://github.com/shermp/NickelClock) (shermp) · [nickel-screensaver](https://github.com/redphx/nickel-screensaver) (redphx) · [KOReader](https://github.com/koreader/koreader) · [Cadmus](https://github.com/OGKevin/cadmus) · [audiobook.koplugin](https://github.com/stradichenko/audiobook.koplugin) · [KCC](https://github.com/ciromattia/kcc) (referencia del conversor) · [reader-dict](https://www.reader-dict.com) · [FBInk](https://github.com/NiLuJe/FBInk) (NiLuJe) · Peanut-GB / Walnut-CGB · los parches de MobileRead (GeoffR, jackie_w, JSWolf y otros) · [NickelMenu Builder](https://github.com/Parzval-Net/NickelMenu-Builder).

Kobo es marca de Rakuten Kobo Inc. Este proyecto no está afiliado ni respaldado por Kobo. Licencias: ver [LICENSE](LICENSE) y [NOTICE.md](NOTICE.md).
