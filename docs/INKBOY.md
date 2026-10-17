# Ink-boy for Kobo E-Ink (`Libra Colour` & `Clara HD`)

Port nativo en C/C++17 del proyecto **[Ink-boy](https://github.com/hellominecraft913-dev/Ink-boy)** (`Peanut-GB` + `Walnut-CGB`) para lectores **Kobo** sobre Linux (`armhf`), diseñado bajo la metodología **REA (Requerimientos, Especificaciones y Arquitectura)**.

---

## Dispositivos Soportados y Perfiles de Pantalla

| Modelo Kobo | Hardware UUID | SoC & Arquitectura | Pantalla E-Ink | Modos Visuales Soportados | Entrada |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Kobo Libra Colour** (`monza` - N428) | `...00000390` | MediaTek MT8113T (Dual Cortex-A53 @ 2.0 GHz, `armhf` userland) | Kaleido 3 CFA Color (`1264×1680`, 300 PPI B/W, 150 PPI Color 2×2) | **1. Kaleido 3 Color (`WFM_GLRC16`/`GCC16`)** con realce de saturación RGB565→BGRA8888 y escala par `×6` (`960×864`) alineada al filtro 2×2 CFA.<br/>**2. Action Turbo 1-bit (`WFM_A2`)** con dithering Bayer 2×2. | Pantalla táctil Multitouch + **2 Botones Físicos de Paso de Página** (`A` y `B`) + Gamepad Bluetooth/USB-OTG |
| **Kobo Clara HD** (`nova` - N249) | `...00000376` | NXP i.MX6ULL (Cortex-A7 @ 1.0 GHz, `armhf`) | E-Ink Carta 16-Grises (`1072×1448`, 300 PPI) | **1. Carta 4-Grises (`WFM_DU4`/`GC16`)** mapeando los 4 tonos del Game Boy a `0xFF, 0xAA, 0x55, 0x00`.<br/>**2. Action Turbo 1-bit (`WFM_A2`)** con dithering Bayer 2×2. | Pantalla táctil **Multitouch de 4 slots** (permite pulsar D-Pad + A/B simultáneamente) + USB-OTG |

---

## Instalación Automática Sin Intervención (Estilo `kp.nicoverbruggen.be`)

Este proyecto incluye dos mecanismos de instalación automática:

### Opción A: Daemon USB Zero-Click (100% Automático al enchufar el Kobo)
1. Coloca tus ROMs `.gb` / `.gbc` (ej. *Pokémon Red, Blue, Yellow, Gold, Silver, Crystal*) en la carpeta `roms/` de este proyecto.
2. Ejecuta en la terminal:
   ```bash
   make auto-install
   ```
3. Conecta tu **Kobo Libra Colour** o **Kobo Clara HD** por USB y toca **"Conectar"** en la pantalla del Kobo.
4. El instalador detectará automáticamente `/Volumes/KOBOeReader`, leerá `.kobo/version` para identificar tu modelo exacto, respaldará y actualizará `.kobo/Kobo/Kobo eReader.conf` (`ExcludeSyncFolders`), instalará `.adds/inkboy/`, creará el acceso directo en **NickelMenu** (`.adds/nm/inkboy`), copiará tus ROMs y expulsará el Kobo de forma segura.

### Opción B: Asistente Web con Web File System Access API (`installer/index.html`)
1. Inicia el servidor local del asistente web:
   ```bash
   make installer-web
   ```
2. Abre `http://127.0.0.1:8765` en Chrome, Edge o Brave.
3. Selecciona el volumen `KOBOeReader`, arrastra tus ROMs `.gb` / `.gbc` a la zona de carga y pulsa **"Instalar Ink-boy en mi Kobo Ahora"**.

---

## Estructura del Proyecto (HAL Desacoplada)

```text
├── include/
│   ├── core/
│   │   ├── emulator_core.h       # Sesión unificada DMG/CGB + Triple Buffer Lock-Free (59.73 Hz)
│   │   ├── walnut_cgb_color.h    # Luminancia, Bayer 2x2 1-bit, 4-Grises y Kaleido 3 BGRA8888
│   │   └── walnut_cgb_port.h     # Bridge C opaco para Walnut-CGB con callbacks RGB565 y monocromo
│   └── platform/
│       ├── kobo_display.h        # FBInk / mxcfb / hwtcon + Dirty-Tiles 8x8 + escalado x6/x7
│       ├── kobo_input.h          # Lector epoll /dev/input/event* Multitouch Type A/B + Botones Libra
│       └── kobo_system.h         # Ciclo de vida Nickel (SIGSTOP/SIGCONT), reloj monotónico y escáner ROMs
├── src/
│   ├── core/                     # Implementaciones Peanut-GB (.gb) y Walnut-CGB (.gbc)
│   ├── platform/                 # Implementaciones HAL Kobo Linux + Simulador macOS
│   └── app/main.cpp              # Ejecutable principal con selector de ROMs E-Ink y HUD táctil
├── installer/
│   └── index.html                # Asistente Web File System Access API (estilo kp.nicoverbruggen.be)
├── tools/
│   ├── kobo_auto_installer.py    # Instalador automático Zero-Touch por USB + autodetección de UUID
│   ├── serve_installer.py        # Servidor local para el instalador web
│   └── generate_demo_roms.py     # Generador de ROMs .gb y .gbc de prueba
└── tests/
    └── kobo_port_tests.cpp       # Suite de pruebas automatizadas REA
```
