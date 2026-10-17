"""Kobo device detection from a mounted onboard partition (.kobo/version)."""

from __future__ import annotations

import platform
from dataclasses import dataclass, field
from pathlib import Path

# Model IDs are the last 3 digits of the hardware UUID in .kobo/version.
# Sources: FBInk device table (lib/FBInk/fbink.h, DEVICE_KOBO_* enum and per-device quirks), KCC Kobo profiles
# (resolutions), Kobo help center (Bluetooth/audiobooks, Dark mode lists) via KoboPatch Web UI kobo/dark-mode.js.
# soc: "nxp" = i.MX (mxcfb), "sunxi" = Allwinner, "mtk" = MediaTek (hwtcon).
def _m(name, codename, soc, w, h, ppi, color=False, buttons=False, bt=False, stylus=False, dark=True, year=0):
    return {"name": name, "codename": codename, "soc": soc, "color": color, "page_buttons": buttons,
            "width": w, "height": h, "ppi": ppi, "bluetooth": bt, "stylus": stylus, "dark_mode": dark, "year": year}


KOBO_MODELS: dict[str, dict] = {
    "310": _m("Kobo Touch A/B", "trilogy", "nxp", 600, 800, 167, dark=False, year=2011),
    "320": _m("Kobo Touch C", "trilogy", "nxp", 600, 800, 167, dark=False, year=2012),
    "330": _m("Kobo Glo", "kraken", "nxp", 758, 1024, 212, dark=False, year=2012),
    "340": _m("Kobo Mini", "pixie", "nxp", 600, 800, 200, dark=False, year=2012),
    "350": _m("Kobo Aura HD", "dragon", "nxp", 1080, 1440, 265, dark=False, year=2013),
    "360": _m("Kobo Aura", "phoenix", "nxp", 758, 1024, 212, dark=False, year=2013),
    "370": _m("Kobo Aura H2O", "dahlia", "nxp", 1080, 1430, 265, dark=False, year=2014),
    "371": _m("Kobo Glo HD", "alyssum", "nxp", 1072, 1448, 300, dark=False, year=2015),
    "372": _m("Kobo Touch 2.0", "pika", "nxp", 600, 800, 167, dark=False, year=2015),
    "373": _m("Kobo Aura ONE", "daylight", "nxp", 1404, 1872, 300, dark=False, year=2016),
    "381": _m("Kobo Aura ONE Limited Edition", "daylight", "nxp", 1404, 1872, 300, dark=False, year=2017),
    "374": _m("Kobo Aura H2O Edition 2", "snow", "nxp", 1080, 1440, 265, dark=False, year=2017),
    "378": _m("Kobo Aura H2O Edition 2 (r2)", "snow", "nxp", 1080, 1440, 265, dark=False, year=2018),
    "375": _m("Kobo Aura Edition 2", "star", "nxp", 758, 1024, 212, dark=False, year=2016),
    "379": _m("Kobo Aura Edition 2 (r2)", "star", "nxp", 758, 1024, 212, dark=False, year=2018),
    "376": _m("Kobo Clara HD", "nova", "nxp", 1072, 1448, 300, dark=False, year=2018),
    "377": _m("Kobo Forma", "frost", "nxp", 1440, 1920, 300, buttons=True, dark=False, year=2018),
    "380": _m("Kobo Forma 32GB", "frost32", "nxp", 1440, 1920, 300, buttons=True, dark=False, year=2019),
    "384": _m("Kobo Libra H2O", "storm", "nxp", 1264, 1680, 300, buttons=True, dark=False, year=2019),
    "382": _m("Kobo Nia", "luna", "nxp", 758, 1024, 212, dark=False, year=2020),
    "387": _m("Kobo Elipsa", "europa", "sunxi", 1404, 1872, 227, bt=True, stylus=True, year=2021),
    "388": _m("Kobo Libra 2", "io", "nxp", 1264, 1680, 300, buttons=True, bt=True, year=2021),
    "383": _m("Kobo Sage", "cadmus", "sunxi", 1440, 1920, 300, buttons=True, bt=True, stylus=True, year=2021),
    "386": _m("Kobo Clara 2E", "goldfinch", "nxp", 1072, 1448, 300, bt=True, year=2022),
    "389": _m("Kobo Elipsa 2E", "condor", "mtk", 1404, 1872, 227, bt=True, stylus=True, year=2023),
    "390": _m("Kobo Libra Colour", "monza", "mtk", 1264, 1680, 300, color=True, buttons=True, bt=True, stylus=True, year=2024),
    "391": _m("Kobo Clara BW", "spaBW", "mtk", 1072, 1448, 300, bt=True, year=2024),
    "393": _m("Kobo Clara Colour", "spaColour", "mtk", 1072, 1448, 300, color=True, bt=True, year=2024),
    "395": _m("Kobo Clara BW (TPV)", "spaTPV", "mtk", 1072, 1448, 300, bt=True, year=2025),
}


@dataclass
class KoboDevice:
    root: Path
    serial: str = ""
    firmware: str = ""
    model_id: str = ""
    model: dict = field(default_factory=dict)

    @property
    def name(self) -> str:
        return self.model.get("name", f"Kobo desconocido ({self.model_id or '?'})")

    @property
    def is_known(self) -> bool:
        return bool(self.model)

    @property
    def firmware_tuple(self) -> tuple[int, ...]:
        try:
            return tuple(int(p) for p in self.firmware.split("."))
        except ValueError:
            return ()

    def to_dict(self) -> dict:
        return {
            "root": str(self.root),
            "serial": self.serial,
            "firmware": self.firmware,
            "model_id": self.model_id,
            "name": self.name,
            "codename": self.model.get("codename", ""),
            "soc": self.model.get("soc", ""),
            "color": self.model.get("color", False),
            "page_buttons": self.model.get("page_buttons", False),
            "width": self.model.get("width"), "height": self.model.get("height"), "ppi": self.model.get("ppi"),
            "bluetooth": self.model.get("bluetooth", False), "stylus": self.model.get("stylus", False),
            "dark_mode": self.model.get("dark_mode", True), "year": self.model.get("year"),
            "known": self.is_known,
        }


def parse_version_file(root: Path) -> KoboDevice:
    """Parse .kobo/version: serial,kernel?,firmware,...,hardware-uuid (format varies by firmware)."""
    dev = KoboDevice(root=root)
    vf = root / ".kobo" / "version"
    if not vf.is_file():
        return dev
    parts = [p.strip() for p in vf.read_text(encoding="utf-8", errors="replace").strip().split(",")]
    if parts:
        dev.serial = parts[0]
    # Firmware is the field shaped like N.N.NNNNN (4.45.23697, 5.1.12345...): the long build number
    # distinguishes it from the kernel version field (e.g. 4.9.77).
    for p in parts[1:]:
        segs = p.split(".")
        if len(segs) == 3 and all(s.isdigit() for s in segs) and len(segs[2]) >= 4:
            dev.firmware = p
            break
    uuid = parts[-1] if parts else ""
    dev.model_id = uuid[-3:] if len(uuid) >= 3 and uuid[-3:].isdigit() else ""
    dev.model = KOBO_MODELS.get(dev.model_id, {})
    return dev


def candidate_mounts() -> list[Path]:
    import os
    if os.environ.get("KOBO_AIO_ROOT"):
        root = Path(os.environ["KOBO_AIO_ROOT"])
        return [root] if (root / ".kobo" / "version").is_file() else []
    sysname = platform.system()
    if sysname == "Darwin":
        bases = [Path("/Volumes")]
    elif sysname == "Linux":
        bases = [Path("/media"), Path("/run/media"), Path("/mnt")]
    else:
        bases = [Path(f"{d}:/") for d in "DEFGHIJKLMNOPQRSTUVWXYZ"]
    found: list[Path] = []
    for base in bases:
        if not base.exists():
            continue
        try:
            entries = [base] if sysname == "Windows" else list(base.iterdir())
            # Linux mounts live one level deeper (/media/<user>/KOBOeReader)
            if sysname == "Linux":
                entries = entries + [c for e in entries if e.is_dir() for c in e.iterdir()]
        except OSError:
            continue
        for e in entries:
            try:
                if (e / ".kobo" / "version").is_file():
                    found.append(e)
            except OSError:
                pass
    return found


def find_device(explicit: Path | None = None) -> KoboDevice | None:
    import os
    if explicit is None and os.environ.get("KOBO_AIO_ROOT"):
        explicit = Path(os.environ["KOBO_AIO_ROOT"])  # testing / non-standard mount points
    if explicit is not None:
        return parse_version_file(explicit) if (explicit / ".kobo" / "version").is_file() else None
    mounts = candidate_mounts()
    return parse_version_file(mounts[0]) if mounts else None
