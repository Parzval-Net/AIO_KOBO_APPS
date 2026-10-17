# Kobo AIO

Todo en uno para lectores **Kobo**: mods de Nickel, apps, fuentes, menús, parches de firmware, cómics, audiolibros, diccionarios, el emulador **Ink-boy** y mantenimiento. Todo con respaldo automático y vuelta atrás.

**Interfaz web:** https://parzval-net.github.io/AIO_KOBO_APPS/ (se conecta al motor local de tu computador; no sube nada a internet)

> Probado en Kobo Libra Colour (N428, firmware 4.45.23697). El catálogo se adapta solo a cada modelo y firmware: lo que no es compatible aparece desactivado y explica por qué.

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
| **Diagnóstico** | Detecta modelo y firmware, `ExcludeSyncFolders` corrupto, libros de Calibre que faltan, integridad de `KoboReader.sqlite` (sobre una copia), basura de macOS, Spotlight, espacio libre y errores en configs de NickelMenu (linter según la spec oficial) |
| **Sistema** | NickelMenu 0.6.0 · Inicio minimalista (NickelHome) · **Parches de firmware con kobopatch** (interlineado, márgenes, fuentes, encabezados, suspensión, bloqueo de actualizaciones…), con opción de restaurar los originales |
| **Lectura** | Fuentes para e-reader (ebook-fonts, 44 familias) · Tipografía mejorada (NickelTypeFix) · Reloj y batería (NickelClock) · Animación de página (NickelDissolve, MTK) |
| **Menú** | Atajos validados (suspender, reiniciar, importar libros, modo oscuro, WiFi, capturas, juegos ocultos, búsqueda en RAE/Wikipedia/traductor, respaldo de la base de datos, ReKindle) · Reorden de pestañas · Tarjeta del propietario |
| **Apps** | KOReader · Cadmus · **Audiolibros** (audiobook.koplugin para KOReader, con Bluetooth en la Libra Colour) |
| **Juegos** | Ink-boy: Game Boy y Game Boy Color nativo para e-ink (color Kaleido 3, 4 grises en Carta, modo turbo A2) |
| **Biblioteca** | **Conversor de cómics y manga estilo KCC** ajustado a cada panel (recorte, páginas dobles, color Kaleido, KEPUB de página fija o CBZ) · Envío de libros con EPUB a KEPUB (kepubify) · **Armador de audiolibros**: MP3 con capítulos y portada · Diccionarios Wiktionary o propios · Pantallas de reposo |
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
make test              # Ink-boy: suites C++, simulador y tests del AIO
make kobo              # compila Ink-boy para ARM (zig)
```

Estructura: `tools/kobo_aio/` (motor, módulos, servidor), `web/` (interfaz, publicada en GitHub Pages), `src/` e `include/` (Ink-boy), `tests/`. Documentación de Ink-boy en [docs/INKBOY.md](docs/INKBOY.md).

## Créditos

Construido sobre el trabajo de la comunidad Kobo. Gracias a: [NickelMenu](https://github.com/pgaskin/NickelMenu), [kobopatch](https://github.com/pgaskin/kobopatch), [kepubify](https://github.com/pgaskin/kepubify) y [dictutil](https://pgaskin.net/dictutil/) (pgaskin) · [KoboPatch Web UI](https://github.com/nicoverbruggen/kobopatch-webui), NickelHome, NickelTypeFix, NickelDissolve y [ebook-fonts](https://github.com/nicoverbruggen/ebook-fonts) (Nico Verbruggen) · [NickelClock](https://github.com/shermp/NickelClock) (shermp) · [nickel-screensaver](https://github.com/redphx/nickel-screensaver) (redphx) · [KOReader](https://github.com/koreader/koreader) · [Cadmus](https://github.com/OGKevin/cadmus) · [audiobook.koplugin](https://github.com/stradichenko/audiobook.koplugin) · [KCC](https://github.com/ciromattia/kcc) (referencia del conversor) · [reader-dict](https://www.reader-dict.com) · [FBInk](https://github.com/NiLuJe/FBInk) (NiLuJe) · Peanut-GB / Walnut-CGB · los parches de MobileRead (GeoffR, jackie_w, JSWolf y otros) · [NickelMenu Builder](https://github.com/Parzval-Net/NickelMenu-Builder).

Kobo es marca de Rakuten Kobo Inc. Este proyecto no está afiliado ni respaldado por Kobo. Licencias: ver [LICENSE](LICENSE) y [NOTICE.md](NOTICE.md).
