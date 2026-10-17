"""Shared helpers for modules."""

from __future__ import annotations

import io
import tarfile
import zipfile

from ..core import Artifact, Context
from ..qtini import QtIni

CONF_REL = ".kobo/Kobo/Kobo eReader.conf"
ONBOARD = "/mnt/onboard"

# Pinned third-party artifacts. Versions/SHA-256 cross-checked against the KoboPatch Web UI lockfile
# (MIT, github.com/nicoverbruggen/kobopatch-webui/installables.lock @ be94f5b) or the GitHub release digests,
# and re-hashed locally. Updated 2026-10-17: NickelTypeFix 1.0, Cadmus 0.12.0, NickelModManager 0.3.
ART = {
    "nickelmenu": Artifact("https://github.com/pgaskin/NickelMenu/releases/download/v0.6.0/KoboRoot.tgz",
                           "322ff9aa863860e8f5f7e0b55cae561c54bf95983b9bce1d19819d1225d064af", "KoboRoot.tgz", 66939),
    "nickelclock": Artifact("https://github.com/shermp/NickelClock/releases/download/v0.4.0/NickelClock-v0.4.0.zip",
                            "3b55a98cc52bb0434a745affca66a3d4c0f3a346b3cc6be095c04409213d3808",
                            "NickelClock-v0.4.0.zip", 38385),
    "nickeltypefix": Artifact("https://github.com/nicoverbruggen/NickelTypeFix/releases/download/v1.0/KoboRoot.tgz",
                              "d1fbbfbc3f8214259ca4ae05ea1f30bdec2f011793b222ae5543843a6a4f43ef", "KoboRoot.tgz", 80827),
    "nickelmodmanager": Artifact("https://github.com/nicoverbruggen/NickelModManager/releases/download/v0.3/KoboRoot.tgz",
                                 "17593550bd4f7c26ca1c277b0852bc175deba5a3409f20c71d0afdac7758084b", "KoboRoot.tgz", 122065),
    "nickeldissolve": Artifact("https://github.com/nicoverbruggen/NickelDissolve/releases/download/v0.7/KoboRoot.tgz",
                               "d543c90ffac8f5cc49b66df1b3d68b0335dbd8d69984745ba533d2c4ea8f27c6", "KoboRoot.tgz", 28200),
    "nickelhome": Artifact("https://github.com/nicoverbruggen/NickelHome/releases/download/v0.6/KoboRoot.tgz",
                           "05b131d8d185b00dbf48bbfc7cba1fb63b7bcb7276f2ebc31324422de1fd187e", "KoboRoot.tgz", 16804),
    "koreader": Artifact("https://github.com/koreader/koreader/releases/download/v2026.07.1/koreader-kobo-v2026.07.1.zip",
                         "0f36a62ce73b12516f969e4ad7862cc06920afb03c7bd4db29a0d990bdd84b2f",
                         "koreader-kobo-v2026.07.1.zip", 42370936),
    "cadmus": Artifact("https://github.com/OGKevin/cadmus/releases/download/v0.12.0/cadmus-kobo.tar.gz",
                       "cbf9934569185037685c2fa8727c529375e713d782fd8a4f572ad6dd13c2b94d", "cadmus-kobo.tar.gz", 40726763),
    "fonts-core": Artifact("https://github.com/nicoverbruggen/ebook-fonts/releases/download/v2026.08.05/kobo-core-fonts.zip",
                           "1be75f10d19d68f790716105a7b42c1481f35ee911e132e32182a98c0cc875f3",
                           "kobo-core-fonts.zip", 5812011),
    "fonts-extra": Artifact("https://github.com/nicoverbruggen/ebook-fonts/releases/download/v2026.08.05/kobo-extra-fonts.zip",
                            "990e060e37b251f2045e2bd9542d246c7aaf14b4ad5573187e02f8d06a5bd1af",
                            "kobo-extra-fonts.zip", 22528615),
}


def conf_edit(ctx: Context, sets: list[tuple[str, str, str]] = (), removes: list[tuple[str, str]] = (),
              note: str = "") -> None:
    """Edit Kobo eReader.conf in place, preserving every other line byte-for-byte."""
    text = ctx.txn.read_text(CONF_REL)
    if text is None:
        ctx.txn.note("Kobo eReader.conf no existe todavía: se omite el ajuste (Nickel lo crea al primer arranque)")
        return
    ini = QtIni(text)
    changed = False
    for section, key, value in sets:
        changed |= ini.set(section, key, value)
    for section, key in removes:
        changed |= ini.remove(section, key)
    if changed:
        ctx.txn.write_text(CONF_REL, ini.dumps(), note=note or "ajustes de Nickel")


def conf_get(ctx: Context, section: str, key: str) -> str | None:
    text = ctx.txn.read_text(CONF_REL)
    return None if text is None else QtIni(text).get(section, key)


def koboroot_from(ctx: Context, key: str) -> bytes:
    data = ctx.fetch(ART[key])
    if ART[key].filename.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            data = zf.read("KoboRoot.tgz")
    return data


def tgz_onboard_files(data: bytes) -> list[tuple[str, bytes]]:
    """Files a KoboRoot.tgz places on the onboard partition (mnt/onboard/...)."""
    out = []
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for m in tf.getmembers():
            name = m.name.lstrip("./")
            if m.isfile() and name.startswith("mnt/onboard/"):
                out.append((name[len("mnt/onboard/"):], tf.extractfile(m).read()))
    return out


def extract_tree(ctx: Context, members: list[tuple[str, bytes, int]], dest_rel: str, note: str) -> int:
    n = 0
    for rel, data, mode in members:
        ctx.txn.write_bytes(f"{dest_rel}/{rel}", data, mode=mode or None)
        n += 1
    ctx.txn.note(f"{note}: {n} archivos en {dest_rel}")
    return n


def remove_tree_files(ctx: Context, dest_rel: str) -> int:
    root = ctx.device.root / dest_rel
    n = 0
    if root.is_dir():
        for f in sorted(root.rglob("*")):
            if f.is_file():
                ctx.txn.delete(str(f.relative_to(ctx.device.root)))
                n += 1
    return n


def toggle_script(target_file: str, sed_on: str, sed_off: str, probe_cmd: str, label: str) -> str:
    """Shell script for NickelMenu cmd_output: flips a setting and reboots so the mod reloads."""
    return f"""#!/bin/sh
# Kobo AIO: alterna {label}. Requiere reinicio para aplicar.
F="{target_file}"
if [ ! -f "$F" ]; then echo "{label}: no está instalado"; exit 0; fi
if {probe_cmd}; then
  sed -i '{sed_off}' "$F"; echo "{label}: DESACTIVADO. Reiniciando..."
else
  sed -i '{sed_on}' "$F"; echo "{label}: ACTIVADO. Reiniciando..."
fi
sync
(sleep 5 && reboot) >/dev/null 2>&1 &
"""
