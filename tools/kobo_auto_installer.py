#!/usr/bin/env python3
"""
===============================================================================
Ink-boy Automatic Zero-Touch Kobo Installer
Inspired by https://kp.nicoverbruggen.be
===============================================================================
Supports:
  1. Direct One-Shot USB Install: Detects /Volumes/KOBOeReader (macOS) or
     /media/*/KOBOeReader (Linux), reads .kobo/version to identify the exact
     Kobo hardware (Kobo Libra Colour N428 vs Kobo Clara HD N249, etc.),
     patches .kobo/Kobo/Kobo eReader.conf (ExcludeSyncFolders), installs
     .adds/inkboy + .adds/nm/inkboy + local ROMs, and optionally installs
     NickelMenu (KoboRoot.tgz) if missing.
  2. USB Daemon Watch Mode (--watch): Waits in the background for a Kobo to be
     plugged in via USB, automatically installs everything with zero user
     intervention, syncs filesystems, and optionally safely ejects the drive.
  3. Self-Test Mode (--self-test): Verifies installation against synthetic
     Kobo Libra Colour and Kobo Clara HD volumes.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

NICKELMENU_RELEASE_URL = (
    "https://github.com/pgaskin/NickelMenu/releases/download/v0.5.4/KoboRoot.tgz"
)

EXCLUDE_SYNC_REGEX = r"(\\.(?!kobo|adobe).+|([^.][^/]*/)+\\..+)"

# Official Kobo Hardware UUID Suffix Table (from .kobo/version)
KOBO_DEVICES = {
    "390": {
        "name": "Kobo Libra Colour (N428)",
        "soc": "MediaTek MT8113T (Dual Cortex-A53 @ 2.0 GHz)",
        "panel": "E-Ink Kaleido 3 CFA Color (1264x1680)",
        "is_color": True,
        "default_scale": 6,
        "default_mode": "quality",
        "has_buttons": True,
    },
    "393": {
        "name": "Kobo Clara Colour (N367)",
        "soc": "MediaTek MT8113T (Dual Cortex-A53 @ 2.0 GHz)",
        "panel": "E-Ink Kaleido 3 CFA Color (1072x1448)",
        "is_color": True,
        "default_scale": 6,
        "default_mode": "quality",
        "has_buttons": False,
    },
    "376": {
        "name": "Kobo Clara HD (N249)",
        "soc": "NXP i.MX6ULL (Cortex-A7 @ 1.0 GHz)",
        "panel": "E-Ink Carta 16-Gray (1072x1448)",
        "is_color": False,
        "default_scale": 6,
        "default_mode": "quality",
        "has_buttons": False,
    },
    "391": {
        "name": "Kobo Clara BW (N365)",
        "soc": "MediaTek MT8113L (Cortex-A53)",
        "panel": "E-Ink Carta 1300 16-Gray (1072x1448)",
        "is_color": False,
        "default_scale": 6,
        "default_mode": "quality",
        "has_buttons": False,
    },
    "384": {
        "name": "Kobo Libra 2 (N418)",
        "soc": "NXP i.MX6SLL",
        "panel": "E-Ink Carta 1200 16-Gray (1264x1680)",
        "is_color": False,
        "default_scale": 6,
        "default_mode": "quality",
        "has_buttons": True,
    },
    "382": {
        "name": "Kobo Nia (N306)",
        "soc": "NXP i.MX6ULL",
        "panel": "E-Ink Carta (758x1024)",
        "is_color": False,
        "default_scale": 4,
        "default_mode": "turbo",
        "has_buttons": False,
    },
}


def find_mounted_kobo() -> Path | None:
    """Locate a mounted KOBOeReader volume on macOS, Linux, or Windows."""
    candidates = [
        Path("/Volumes/KOBOeReader"),
    ]
    # Scan /Volumes/* on macOS or /media, /run/media on Linux
    for root in (Path("/Volumes"), Path("/media"), Path("/run/media"), Path("/mnt")):
        if root.is_dir():
            try:
                for child in root.iterdir():
                    if child.is_dir():
                        if (child / ".kobo" / "version").is_file():
                            return child
                        for sub in child.iterdir():
                            if sub.is_dir() and (sub / ".kobo" / "version").is_file():
                                return sub
            except PermissionError:
                pass

    for c in candidates:
        if (c / ".kobo" / "version").is_file():
            return c
    return None


def parse_kobo_version(kobo_root: Path) -> dict:
    """
    Parse .kobo/version to extract serial number, firmware version, and hardware UUID.
    Format example:
      N428340012345,4.40.23081,4.39.23027,2024-06-12,00000000-0000-0000-0000-000000000390
    """
    ver_file = kobo_root / ".kobo" / "version"
    info = {
        "serial": "UNKNOWN",
        "firmware": "4.x",
        "uuid": "00000000-0000-0000-0000-000000000390",
        "device_id": "390",
        "profile": KOBO_DEVICES["390"],
    }
    if not ver_file.is_file():
        return info

    raw = ver_file.read_text(encoding="utf-8", errors="replace").strip()
    parts = [p.strip() for p in raw.split(",")]
    if len(parts) >= 1 and parts[0]:
        info["serial"] = parts[0]
    if len(parts) >= 3 and parts[2]:
        info["firmware"] = parts[2]
    if len(parts) >= 1:
        uuid = parts[-1]
        info["uuid"] = uuid
        m = re.search(r"0*([1-9][0-9]*)$", uuid)
        if m:
            dev_id = m.group(1)
            info["device_id"] = dev_id
            if dev_id in KOBO_DEVICES:
                info["profile"] = KOBO_DEVICES[dev_id]
            else:
                info["profile"] = {
                    "name": f"Kobo eReader (ID {dev_id})",
                    "soc": "ARMv7 / ARMv8 Linux (armhf)",
                    "panel": "E-Ink Display",
                    "is_color": False,
                    "default_scale": 6,
                    "default_mode": "quality",
                    "has_buttons": False,
                }
    return info


def patch_kobo_ereader_conf(kobo_root: Path) -> bool:
    """
    Safely update .kobo/Kobo/Kobo eReader.conf so Nickel ignores hidden folders
    inside .adds/ (ExcludeSyncFolders with properly doubled backslashes for Qt QSettings),
    and automatically recover from Kobo eReader.conf.inkboy.bak if a previous run
    wrote single-backslash ExcludeSyncFolders or truncated the file.
    """
    conf_dir = kobo_root / ".kobo" / "Kobo"
    conf_dir.mkdir(parents=True, exist_ok=True)
    conf_path = conf_dir / "Kobo eReader.conf"
    bak_path = conf_dir / "Kobo eReader.conf.inkboy.bak"

    content = ""
    if conf_path.is_file():
        content = conf_path.read_text(encoding="utf-8", errors="replace")
        if not bak_path.is_file() and len(content.strip()) > 80:
            bak_path.write_text(content, encoding="utf-8")

    # Restore the backup only when the current config is missing or truncated. A shorter but valid
    # config is legitimate (Nickel rewrites it), and a broken ExcludeSyncFolders line is fixed below
    # by replacing just that line, so newer Nickel settings are never rolled back.
    if bak_path.is_file() and len(content.strip()) <= 80:
        bak_content = bak_path.read_text(encoding="utf-8", errors="replace")
        if len(bak_content.strip()) > 80:
            content = bak_content
            print("-> [RECOVERY] Restored original Kobo eReader.conf from Kobo eReader.conf.inkboy.bak")

    target_line = f"ExcludeSyncFolders={EXCLUDE_SYNC_REGEX}"

    if "[FeatureSettings]" not in content:
        if content and not content.endswith("\n"):
            content += "\n"
        content += f"\n[FeatureSettings]\n{target_line}\n"
    else:
        lines = content.splitlines()
        new_lines = []
        in_feature = False
        replaced = False
        for line in lines:
            stripped = line.strip()
            if stripped.startswith("[") and stripped.endswith("]"):
                if in_feature and not replaced:
                    new_lines.append(target_line)
                    replaced = True
                in_feature = (stripped == "[FeatureSettings]")
                new_lines.append(line)
            elif in_feature and stripped.startswith("ExcludeSyncFolders="):
                new_lines.append(target_line)
                replaced = True
            else:
                new_lines.append(line)
        if in_feature and not replaced:
            new_lines.append(target_line)
        content = "\n".join(new_lines) + "\n"

    conf_path.write_text(content, encoding="utf-8")
    touch_sideloaded_books(kobo_root)
    return True


def touch_sideloaded_books(kobo_root: Path) -> int:
    """
    Update mtime on non-hidden book files and folders on kobo_root so Nickel
    immediately re-indexes sideloaded books upon USB disconnect if they were
    previously hidden by a single-backslash ExcludeSyncFolders regex.
    """
    book_exts = {".epub", ".pdf", ".cbz", ".cbr", ".mobi", ".txt", ".rtf", ".html"}
    touched = 0
    try:
        for root_dir, dirs, files in os.walk(kobo_root):
            # Skip hidden directories (.kobo, .adds, .adobe-digital-editions, etc.)
            dirs[:] = [d for d in dirs if not d.startswith(".")]
            root_path = Path(root_dir)
            if root_path != kobo_root:
                try:
                    os.utime(root_path, None)
                except OSError:
                    pass
            for fname in files:
                if fname.startswith("."):
                    continue
                fpath = root_path / fname
                low = fname.lower()
                if any(low.endswith(ext) for ext in book_exts):
                    try:
                        os.utime(fpath, None)
                        touched += 1
                    except OSError:
                        pass
    except Exception:
        pass
    if touched > 0:
        print(f"-> [RECOVERY] Triggered Nickel library re-index for {touched} sideloaded book(s)")
    return touched



def generate_tailored_config_ini(profile: dict) -> str:
    scale = profile.get("default_scale", 6)
    mode = profile.get("default_mode", "quality")
    dmg_pal = 1 if profile.get("is_color", False) else 0
    return (
        f"# Auto-generated by Ink-boy Installer for {profile['name']}\n"
        f"# SoC: {profile['soc']} | Panel: {profile['panel']}\n\n"
        f"[display]\n"
        f"scale = {scale}\n"
        f"mode = {mode}\n"
        f"dmg_palette = {dmg_pal}\n\n"
        f"[input]\n"
        f"libra_buttons = A_B\n\n"
        f"[system]\n"
        f"# resume = return to Nickel, reboot = clean reboot on exit\n"
        f"exit_mode = resume\n"
        f"# Experimental: pause Nickel's watchdog (sickel) while playing\n"
        f"pause_watchdog = no\n"
    )


def install_to_kobo(
    kobo_root: Path,
    project_root: Path,
    extra_roms_dir: Path | None = None,
    install_nickelmenu_tgz: bool = False,
) -> dict:
    """Perform a complete, non-destructive installation of Ink-boy onto a Kobo volume."""
    device_info = parse_kobo_version(kobo_root)
    profile = device_info["profile"]

    print(f"-> Detected Device : {profile['name']}")
    print(f"-> Hardware SoC    : {profile['soc']}")
    print(f"-> Display Panel   : {profile['panel']}")
    print(f"-> Firmware        : {device_info['firmware']} (UUID: {device_info['uuid']})")

    # 1. Patch Kobo eReader.conf with ExcludeSyncFolders
    patch_kobo_ereader_conf(kobo_root)
    print("-> [OK] Updated .kobo/Kobo/Kobo eReader.conf (ExcludeSyncFolders)")

    # 2. Create .adds/inkboy and .adds/nm structure
    inkboy_dst = kobo_root / ".adds" / "inkboy"
    roms_dst = inkboy_dst / "roms"
    saves_dst = inkboy_dst / "saves"
    nm_dst = kobo_root / ".adds" / "nm"

    roms_dst.mkdir(parents=True, exist_ok=True)
    saves_dst.mkdir(parents=True, exist_ok=True)
    nm_dst.mkdir(parents=True, exist_ok=True)

    dist_root = project_root / "dist" / "Kobo-InkBoy"

    # 3. Copy inkboy binary, inkboy.sh launcher, and tailored config.ini
    bin_src = dist_root / ".adds" / "inkboy" / "inkboy"
    if bin_src.is_file():
        shutil.copy2(bin_src, inkboy_dst / "inkboy")
        os.chmod(inkboy_dst / "inkboy", 0o755)
        print("-> [OK] Installed ARMhf native binary: .adds/inkboy/inkboy")

    sh_src = dist_root / ".adds" / "inkboy" / "inkboy.sh"
    if sh_src.is_file():
        shutil.copy2(sh_src, inkboy_dst / "inkboy.sh")
        os.chmod(inkboy_dst / "inkboy.sh", 0o755)
        print("-> [OK] Installed launcher script: .adds/inkboy/inkboy.sh")

    (inkboy_dst / "config.ini").write_text(
        generate_tailored_config_ini(profile), encoding="utf-8"
    )
    print(f"-> [OK] Generated optimal config.ini for {profile['name']}")

    # 4. Copy NickelMenu entry
    nm_src = dist_root / ".adds" / "nm" / "inkboy"
    if nm_src.is_file():
        shutil.copy2(nm_src, nm_dst / "inkboy")
        print("-> [OK] Installed NickelMenu shortcut: .adds/nm/inkboy")

    # 5. Copy bundled demo ROMs + any user ROMs in extra_roms_dir or ./roms
    copied_roms = []
    skipped_saves = []
    rom_sources = [dist_root / ".adds" / "inkboy" / "roms", project_root / "roms"]
    if extra_roms_dir:
        rom_sources.append(extra_roms_dir)

    for src_dir in rom_sources:
        if src_dir and src_dir.is_dir():
            for item in src_dir.iterdir():
                if not item.is_file():
                    continue
                suffix = item.suffix.lower()
                if suffix in (".gb", ".gbc", ".cgb"):
                    shutil.copy2(item, roms_dst / item.name)
                    copied_roms.append(item.name)
                elif suffix == ".sav":
                    # Never overwrite in-game progress made on the Kobo with an older copy from the host
                    if (roms_dst / item.name).exists():
                        skipped_saves.append(item.name)
                        continue
                    shutil.copy2(item, roms_dst / item.name)
                    copied_roms.append(item.name)

    if copied_roms:
        print(f"-> [OK] Copied {len(copied_roms)} ROM/save file(s): {', '.join(copied_roms)}")
    if skipped_saves:
        print(f"-> [KEEP] Preserved existing save(s) on Kobo: {', '.join(skipped_saves)}")

    # 6. Optional NickelMenu KoboRoot.tgz provisioning if requested
    if install_nickelmenu_tgz:
        tgz_dst = kobo_root / ".kobo" / "KoboRoot.tgz"
        local_cached_tgz = project_root / "dist" / "NickelMenu-KoboRoot.tgz"
        if local_cached_tgz.is_file():
            shutil.copy2(local_cached_tgz, tgz_dst)
            print("-> [OK] Staged NickelMenu KoboRoot.tgz into .kobo/KoboRoot.tgz")
        else:
            try:
                print(f"-> Downloading NickelMenu from {NICKELMENU_RELEASE_URL} ...")
                urllib.request.urlretrieve(NICKELMENU_RELEASE_URL, local_cached_tgz)
                shutil.copy2(local_cached_tgz, tgz_dst)
                print("-> [OK] Downloaded & staged NickelMenu into .kobo/KoboRoot.tgz")
            except Exception as exc:
                print(f"-> [WARN] Could not download NickelMenu automatically: {exc}")

    # Sync filesystem buffers
    if hasattr(os, "sync"):
        os.sync()

    return {
        "device_info": device_info,
        "copied_roms": copied_roms,
        "kobo_root": str(kobo_root),
    }


def safely_eject_kobo(kobo_root: Path) -> None:
    sys_name = platform.system()
    try:
        if sys_name == "Darwin":
            subprocess.run(["diskutil", "eject", str(kobo_root)], check=False)
        elif sys_name == "Linux":
            subprocess.run(["umount", str(kobo_root)], check=False)
    except Exception as exc:
        print(f"-> [WARN] Auto-eject notice: {exc}")


def run_self_test(project_root: Path) -> int:
    print("=== Running Auto-Installer Self-Test (Kobo Libra Colour & Kobo Clara HD) ===")
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)

        # 1. Mock Kobo Libra Colour (UUID 390)
        libra_vol = tmp_path / "KOBOeReader_LibraColour"
        (libra_vol / ".kobo" / "Kobo").mkdir(parents=True)
        (libra_vol / ".kobo" / "version").write_text(
            "N428390112233,4.40.23081,4.40.23081,2024-06-12,00000000-0000-0000-0000-000000000390\n",
            encoding="utf-8",
        )
        res_libra = install_to_kobo(libra_vol, project_root)
        assert res_libra["device_info"]["device_id"] == "390"
        assert res_libra["device_info"]["profile"]["is_color"] is True
        assert (libra_vol / ".adds" / "nm" / "inkboy").is_file()
        assert (libra_vol / ".adds" / "inkboy" / "inkboy.sh").is_file()
        conf_text = (libra_vol / ".kobo" / "Kobo" / "Kobo eReader.conf").read_text(encoding="utf-8")
        assert "ExcludeSyncFolders=" in conf_text

        # Reinstall must keep Kobo-side progress and newer (shorter) Nickel settings
        kobo_sav = libra_vol / ".adds" / "inkboy" / "roms" / "Kobo_Carta_Classic_Demo.sav"
        kobo_sav.write_bytes(b"KOBO-PROGRESS")
        conf_path = libra_vol / ".kobo" / "Kobo" / "Kobo eReader.conf"
        (conf_path.parent / "Kobo eReader.conf.inkboy.bak").write_text(
            "[OldSettings]\n" + "x" * 500 + "\n", encoding="utf-8"
        )
        newer_conf = "[FeatureSettings]\nExcludeSyncFolders=old\n[NewerSettings]\nKeep=true\n" + "y" * 80 + "\n"
        conf_path.write_text(newer_conf, encoding="utf-8")
        install_to_kobo(libra_vol, project_root)
        assert kobo_sav.read_bytes() == b"KOBO-PROGRESS"
        conf_text = conf_path.read_text(encoding="utf-8")
        assert "[NewerSettings]" in conf_text and "[OldSettings]" not in conf_text
        assert f"ExcludeSyncFolders={EXCLUDE_SYNC_REGEX}" in conf_text

        # 2. Mock Kobo Clara HD (UUID 376)
        clara_vol = tmp_path / "KOBOeReader_ClaraHD"
        (clara_vol / ".kobo" / "Kobo").mkdir(parents=True)
        (clara_vol / ".kobo" / "version").write_text(
            "N249376998877,4.38.21908,4.38.21908,2023-10-01,00000000-0000-0000-0000-000000000376\n",
            encoding="utf-8",
        )
        res_clara = install_to_kobo(clara_vol, project_root)
        assert res_clara["device_info"]["device_id"] == "376"
        assert res_clara["device_info"]["profile"]["is_color"] is False
        clara_ini = (clara_vol / ".adds" / "inkboy" / "config.ini").read_text(encoding="utf-8")
        assert "Kobo Clara HD" in clara_ini
        assert "dmg_palette = 0" in clara_ini

    print("=== Auto-Installer Self-Test PASSED! ===")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Automatic Zero-Touch Installer for Ink-boy on Kobo eReaders"
    )
    parser.add_argument(
        "--kobo-path",
        type=Path,
        default=None,
        help="Path to mounted KOBOeReader volume (auto-detected if omitted)",
    )
    parser.add_argument(
        "--roms-dir",
        type=Path,
        default=None,
        help="Optional local folder of .gb/.gbc ROMs to copy onto the Kobo",
    )
    parser.add_argument(
        "--watch",
        action="store_true",
        help="Wait in background for a Kobo to be plugged in via USB and install automatically",
    )
    parser.add_argument(
        "--eject",
        action="store_true",
        help="Safely eject KOBOeReader volume automatically after installation completes",
    )
    parser.add_argument(
        "--install-nickelmenu",
        action="store_true",
        help="Also stage NickelMenu KoboRoot.tgz into .kobo/ if needed",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run automated self-test against simulated Libra Colour & Clara HD volumes",
    )
    args = parser.parse_args()

    project_root = Path(__file__).resolve().parent.parent

    if args.self_test:
        return run_self_test(project_root)

    if args.watch:
        print("==================================================================")
        print(" [Ink-boy USB Auto-Installer] Waiting for Kobo eReader connection ")
        print(" Plug in your Kobo Libra Colour or Kobo Clara HD and tap 'Connect'")
        print("==================================================================")
        while True:
            vol = args.kobo_path if (args.kobo_path and args.kobo_path.is_dir()) else find_mounted_kobo()
            if vol:
                print(f"\n[+] Kobo detected at: {vol}")
                time.sleep(1.0)  # Allow FAT32 mount to settle
                install_to_kobo(vol, project_root, args.roms_dir, args.install_nickelmenu)
                if args.eject:
                    print("[+] Safely ejecting Kobo volume...")
                    safely_eject_kobo(vol)
                print("[+] Installation complete! Unplug your Kobo and open NickelMenu -> Ink-boy.")
                return 0
            time.sleep(1.5)

    vol = args.kobo_path if args.kobo_path else find_mounted_kobo()
    if not vol or not vol.is_dir():
        print(
            "[!] No mounted KOBOeReader volume found.\n"
            "    - Connect your Kobo via USB and tap 'Connect' on its screen,\n"
            "    - Or run with `--watch` to wait automatically for USB connection,\n"
            "    - Or open `installer/index.html` in Chrome/Edge for the Web Wizard."
        )
        return 1

    install_to_kobo(vol, project_root, args.roms_dir, args.install_nickelmenu)
    if args.eject:
        safely_eject_kobo(vol)
    print("\n[+] Done! Safely eject your Kobo to play from NickelMenu -> Ink-boy.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
