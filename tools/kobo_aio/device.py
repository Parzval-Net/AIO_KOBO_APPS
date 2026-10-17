"""Kobo device detection from a mounted onboard partition (.kobo/version)."""

from __future__ import annotations

import platform
from dataclasses import dataclass, field
from pathlib import Path

# Model IDs are the last 3 digits of the hardware UUID in .kobo/version.
# Source: FBInk device table (lib/FBInk/fbink.h, DEVICE_KOBO_* enum).
# soc: "mtk" = MediaTek (hwtcon), "nxp" = i.MX (mxcfb), "sunxi" = Allwinner.
KOBO_MODELS: dict[str, dict] = {
    "376": {"name": "Kobo Clara HD", "codename": "nova", "soc": "nxp", "color": False, "page_buttons": False},
    "377": {"name": "Kobo Forma", "codename": "frost", "soc": "nxp", "color": False, "page_buttons": True},
    "380": {"name": "Kobo Forma 32GB", "codename": "frost32", "soc": "nxp", "color": False, "page_buttons": True},
    "382": {"name": "Kobo Nia", "codename": "luna", "soc": "nxp", "color": False, "page_buttons": False},
    "383": {"name": "Kobo Sage", "codename": "cadmus", "soc": "sunxi", "color": False, "page_buttons": True},
    "384": {"name": "Kobo Libra H2O", "codename": "storm", "soc": "nxp", "color": False, "page_buttons": True},
    "386": {"name": "Kobo Clara 2E", "codename": "goldfinch", "soc": "nxp", "color": False, "page_buttons": False},
    "387": {"name": "Kobo Elipsa", "codename": "europa", "soc": "sunxi", "color": False, "page_buttons": False},
    "388": {"name": "Kobo Libra 2", "codename": "io", "soc": "nxp", "color": False, "page_buttons": True},
    "389": {"name": "Kobo Elipsa 2E", "codename": "condor", "soc": "mtk", "color": False, "page_buttons": False},
    "390": {"name": "Kobo Libra Colour", "codename": "monza", "soc": "mtk", "color": True, "page_buttons": True},
    "391": {"name": "Kobo Clara BW", "codename": "spaBW", "soc": "mtk", "color": False, "page_buttons": False},
    "393": {"name": "Kobo Clara Colour", "codename": "spaColour", "soc": "mtk", "color": True, "page_buttons": False},
    "395": {"name": "Kobo Clara BW (TPV)", "codename": "spaTPV", "soc": "mtk", "color": False, "page_buttons": False},
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
