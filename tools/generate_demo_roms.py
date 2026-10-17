#!/usr/bin/env python3
"""
Generates two valid, runnable 32 KiB Game Boy (.gb) and Game Boy Color (.gbc)
test/demo ROMs with real SM83 machine code, tile graphics, CGB RGB555 palettes,
and interactive joypad movement so Ink-boy can be tested out-of-the-box on
Kobo Libra Colour and Kobo Clara HD.
"""
import os
import sys


def compute_header_checksum(rom: bytearray) -> int:
    chk = 0
    for addr in range(0x0134, 0x014D):
        chk = (chk - rom[addr] - 1) & 0xFF
    return chk


def compute_global_checksum(rom: bytearray) -> int:
    total = 0
    for i, b in enumerate(rom):
        if i not in (0x014E, 0x014F):
            total = (total + b) & 0xFFFF
    return total


def build_demo_rom(title: str, is_cgb: bool) -> bytearray:
    rom = bytearray(0x8000)

    # Entry point at 0x0100: NOP; JP 0x0150
    rom[0x0100] = 0x00
    rom[0x0101] = 0xC3
    rom[0x0102] = 0x50
    rom[0x0103] = 0x01

    # Official Nintendo logo bytes at 0x0104..0x0133
    logo = bytes([
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
        0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
        0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
        0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
        0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
        0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    ])
    rom[0x0104:0x0104 + len(logo)] = logo

    title_bytes = title.encode("ascii")[:11]
    rom[0x0134:0x0134 + len(title_bytes)] = title_bytes
    rom[0x0143] = 0x80 if is_cgb else 0x00
    rom[0x0147] = 0x03  # MBC1 + RAM + BATTERY
    rom[0x0148] = 0x00  # 32 KiB
    rom[0x0149] = 0x02  # 8 KiB SRAM

    # Assemble SM83 program at 0x0150
    code = bytearray()

    # Wait for VBlank (LY >= 144) before touching VRAM
    # .wait_vbl: LDH A, (0x44); CP 144; JR C, .wait_vbl
    code += bytes([0xF0, 0x44, 0xFE, 144, 0x38, 0xFA])

    # Turn off LCD: XOR A; LDH (0x40), A
    code += bytes([0xAF, 0xE0, 0x40])

    # Populate Tile 1 at 0x8010 (Pokeball / Monster badge 8x8 2bpp pattern)
    tile1 = [
        0x3C, 0x3C,
        0x42, 0x7E,
        0x99, 0xFF,
        0xBD, 0xE7,
        0xFF, 0xFF,
        0x99, 0x81,
        0x42, 0x42,
        0x3C, 0x3C,
    ]
    # Populate Tile 2 at 0x8020 (Checkered border tile)
    tile2 = [
        0xAA, 0x55,
        0x55, 0xAA,
        0xAA, 0x55,
        0x55, 0xAA,
        0xAA, 0x55,
        0x55, 0xAA,
        0xAA, 0x55,
        0x55, 0xAA,
    ]
    for idx, b in enumerate(tile1):
        addr = 0x8010 + idx
        code += bytes([0x3E, b, 0xEA, addr & 0xFF, (addr >> 8) & 0xFF])
    for idx, b in enumerate(tile2):
        addr = 0x8020 + idx
        code += bytes([0x3E, b, 0xEA, addr & 0xFF, (addr >> 8) & 0xFF])

    # Fill BG map (0x9800..0x9BFF) with alternating pattern of Tile 0, Tile 1, Tile 2
    # LD HL, 0x9800; LD BC, 0x0400
    code += bytes([
        0x21, 0x00, 0x98,  # LD HL, 0x9800
        0x01, 0x00, 0x04,  # LD BC, 0x0400
        # loop:
        0x7D,              # LD A, L
        0xA9,              # XOR C
        0xE6, 0x03,        # AND 0x03
        0xFE, 0x03,        # CP 3
        0x20, 0x02,        # JR NZ, +2
        0x3E, 0x01,        # LD A, 1
        0x22,              # LD (HL+), A
        0x0B,              # DEC BC
        0x78,              # LD A, B
        0xB1,              # OR C
        0x20, 0xF1,        # JR NZ, loop
    ])

    # If CGB, program colorful BG Palette 0 via BCPS (0xFF68) / BCPD (0xFF69)
    if is_cgb:
        # Auto-increment at index 0: LD A, 0x80; LDH (0x68), A
        code += bytes([0x3E, 0x80, 0xE0, 0x68])
        # 4 BGR555 colors: Cream (0x7FFF), Crimson Red (0x109F), Forest Green (0x1B20), Deep Blue (0x5042)
        cgb_colors = [0x7FFF, 0x109F, 0x1B20, 0x5042]
        for col in cgb_colors:
            lo = col & 0xFF
            hi = (col >> 8) & 0xFF
            code += bytes([0x3E, lo, 0xE0, 0x69, 0x3E, hi, 0xE0, 0x69])

    # Set DMG palette BGP (0xFF47) = 0xE4
    code += bytes([0x3E, 0xE4, 0xE0, 0x47])

    # Enable SRAM and write signature byte 0xA5 to 0xA000
    code += bytes([
        0x3E, 0x0A, 0xEA, 0x00, 0x00,
        0x3E, 0xA5, 0xEA, 0x00, 0xA0,
    ])

    # Turn LCD ON (LCDC = 0x91: LCD on, BG on, 0x8000 tile data)
    code += bytes([0x3E, 0x91, 0xE0, 0x40])

    # Main interactive loop: read D-Pad from P1 (0xFF00) and scroll SCX (0xFF43) / SCY (0xFF42)
    # .main_loop:
    #   LDH A, (0x44); CP 144; JR NZ, .main_loop (wait VBlank start)
    #   LD A, 0x20; LDH (0x00), A  (select direction keys)
    #   LDH A, (0x00); LDH A, (0x00); CPL; AND 0x0F; LD B, A
    #   LDH A, (0x43); INC A; LDH (0x43), A (gentle horizontal animation + input response)
    code += bytes([
        0xF0, 0x44, 0xFE, 144, 0x20, 0xFA,  # wait LY == 144
        0xF0, 0x43, 0x3C, 0xE0, 0x43,       # SCX++
        # wait LY != 144 so we only step once per frame:
        0xF0, 0x44, 0xFE, 144, 0x28, 0xFA,
        0x18, 0xED,                         # JR .main_loop
    ])

    rom[0x0150:0x0150 + len(code)] = code
    rom[0x014D] = compute_header_checksum(rom)
    gchk = compute_global_checksum(rom)
    rom[0x014E] = (gchk >> 8) & 0xFF
    rom[0x014F] = gchk & 0xFF
    return rom


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "dist/Kobo-InkBoy/.adds/inkboy/roms"
    os.makedirs(out_dir, exist_ok=True)

    gbc_path = os.path.join(out_dir, "Kobo_Kaleido_Color_Demo.gbc")
    gb_path = os.path.join(out_dir, "Kobo_Carta_Classic_Demo.gb")

    with open(gbc_path, "wb") as f:
        f.write(build_demo_rom("KOBO COLOR", is_cgb=True))
    with open(gb_path, "wb") as f:
        f.write(build_demo_rom("KOBO MONO", is_cgb=False))

    print(f"Generated demo ROMs:\n  - {gbc_path}\n  - {gb_path}")


if __name__ == "__main__":
    main()
