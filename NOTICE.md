# Avisos de licencia

## Código de este repositorio

- `tools/kobo_aio/`, `web/`, `tests/` y los scripts de `tools/`: licencia MIT (ver [LICENSE](LICENSE)).
- `src/`, `include/` (Ink-boy): licencia MIT, **pero** el binario para Kobo (`dist/Kobo-InkBoy/.adds/inkboy/inkboy`) enlaza estáticamente FBInk, que es **GPL-3.0-or-later**. Por eso ese binario se distribuye bajo **GPL-3.0-or-later**; su código fuente completo está en este repositorio (`src/`, `include/`, `lib/`) y se compila con `make kobo`.

## Código de terceros incluido en `lib/`

| Componente | Licencia |
|---|---|
| FBInk (NiLuJe) | GPL-3.0-or-later (`lib/FBInk/LICENSE`) |
| Peanut-GB | MIT |
| Walnut-CGB | MIT |
| i2c-tools (parte de FBInk) | LGPL-2.1 / GPL-2.0 |

## Componentes que se descargan al instalar (no se redistribuyen aquí)

Se descargan desde sus publicaciones oficiales y se verifican por SHA-256. Cada uno conserva su licencia:

| Componente | Licencia |
|---|---|
| NickelMenu, kobopatch, kepubify (pgaskin) | MIT |
| NickelHome, NickelTypeFix, NickelDissolve (Nico Verbruggen) | MIT |
| NickelClock (shermp) | MIT |
| nickel-screensaver (redphx) | MIT |
| KOReader | AGPL-3.0 |
| Cadmus | AGPL-3.0 (según su autor) |
| audiobook.koplugin | AGPL-3.0 |
| ebook-fonts | Por familia, mayoritariamente SIL OFL 1.1 (Cartisse: licencia Bitstream Charter) |
| Diccionarios de reader-dict.com | Contenido de Wiktionary (CC BY-SA); se descargan desde su sitio |
| Parches de firmware 4.45 | Trabajo comunitario de MobileRead, curado en kobopatch-webui (MIT); los YAML no declaran licencia propia y se descargan desde ese repositorio, no se redistribuyen |
| Firmware oficial de Kobo | Propiedad de Rakuten Kobo; se descarga desde `ereaderfiles.kobo.com` solo para generar el parche en tu equipo |

## Referencias de diseño

- KoboPatch Web UI (MIT): referencia para versiones fijadas, fusión de `KoboRoot.tgz`, validación de parches y scripts de alternancia (reimplementados).
- NickelMenu Builder (MIT, Parzval-Net): biblioteca de acciones adaptada y validada contra la documentación oficial de NickelMenu.
- KCC (ISC): referencia del flujo de conversión de cómics (reimplementado con Pillow).
