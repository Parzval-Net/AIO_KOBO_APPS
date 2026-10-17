"""Firmware patching with kobopatch (advanced). Mirrors the safeguards of KoboPatch Web UI (MIT):
exact firmware/model match, official firmware from ereaderfiles.kobo.com, only the known target binaries in
the output, each the same size as the original and a valid 32-bit little-endian ARM ELF.

Patch definitions are community work from MobileRead (GeoffR, pgaskin, jackie_w, JSWolf...) as curated in
github.com/nicoverbruggen/kobopatch-webui (pinned commit). They are downloaded at install time, not bundled.
"""

from __future__ import annotations

import io
import json
import platform
import re
import subprocess
import tarfile
import tempfile
import zipfile
from pathlib import Path

from ..core import AioError, Artifact, Module, Option, register

KP_COMMIT = "be94f5b04bf4969b6a19025ac1972905bb7584c7"
KP_RAW = f"https://raw.githubusercontent.com/nicoverbruggen/kobopatch-webui/{KP_COMMIT}/patches"
TARGETS = {
    "src/nickel.yaml": "usr/local/Kobo/nickel",
    "src/nickel_custom.yaml": "usr/local/Kobo/nickel",
    "src/libadobe.so.yaml": "usr/local/Kobo/libadobe.so",
    "src/libnickel.so.1.0.0.yaml": "usr/local/Kobo/libnickel.so.1.0.0",
    "src/librmsdk.so.1.0.0.yaml": "usr/local/Kobo/librmsdk.so.1.0.0",
    "src/cloud_sync.yaml": "usr/local/Kobo/libnickel.so.1.0.0",
}
PATCH_SHA = {
    "src/cloud_sync.yaml": "c149ccdfc683ff562d241ae6efad2d615fd2682912f99d6e60e8f196315c23b8",
    "src/libadobe.so.yaml": "e36ca5582e446e9769b8e1a7d9617f9918341b4a88f459d5e978c90b4b2bb22b",
    "src/libnickel.so.1.0.0.yaml": "1b21591ad9751a22bf8ca75fcc788bae80d901a3a3e9b1e96720a2270c5f1325",
    "src/librmsdk.so.1.0.0.yaml": "bdcd4570c67c462c51bd1bcf34d760c6ea9ac3d04e9bef91b346fde0514c8219",
    "src/nickel.yaml": "c52791ecaf94c5bd4b6fb59e7fe6ae998993f54cec9273f883dd4e3dfa37282f",
    "src/nickel_custom.yaml": "ccbbc482101716dcca0a56c438bd587ae0c0f9c2deb5304cd57e5808a5e68b57",
}
BLACKLIST_SHA = "ea7ff99337d5f14084cdfcdb88ee3636dada2f32450fe7c5a5a8ba91d4436a90"
# (firmware, model id) -> official update zip. Channel kobo13 = Libra Colour (verified by KoboPatch Web UI).
FIRMWARE = {
    ("4.45.23697", "390"): Artifact("https://ereaderfiles.kobo.com/firmwares/kobo13/May2026/kobo-update-4.45.23697.zip",
                                    "41ac8068fa216d7fad2d28431b082fdd07b6719a669c2745e11d8df17ec4ba0e",
                                    "kobo-update-4.45.23697.zip", 161697408),
}
KOBOPATCH = {
    "arm64": Artifact("https://github.com/pgaskin/kobopatch/releases/download/v0.16.0/kobopatch-darwin-arm64",
                      "4e9b702676d12e457fc4d4d55de39108a746c4f51786af9a51cc05a89ef5f407", "kobopatch-darwin-arm64"),
}
SUPPORTED_MODELS = tuple(sorted({m for _, m in FIRMWARE}))
SUPPORTED_FIRMWARES = tuple(sorted({f for f, _ in FIRMWARE}))

# Curated, low-risk and popular patches offered in the UI (exact names from the 4.45 YAML files).
FEATURED = [
    ("src/libnickel.so.1.0.0.yaml", "My 10 line spacing values", "10 interlineados más finos"),
    ("src/libnickel.so.1.0.0.yaml", "Custom font sizes", "Tamaños de fuente personalizados"),
    ("src/libnickel.so.1.0.0.yaml", "Custom left & right margins", "Márgenes laterales personalizados"),
    ("src/libnickel.so.1.0.0.yaml", "Force user line spacing in KePubs", "Respetar tu interlineado en KePub"),
    ("src/libnickel.so.1.0.0.yaml", "Remove title from reading header/footer", "Quitar el título del encabezado/pie"),
    ("src/libnickel.so.1.0.0.yaml", "Don't uppercase header/footer text", "Encabezado/pie sin mayúsculas"),
    ("src/libnickel.so.1.0.0.yaml", "Larger Sleep/Power-off timeouts", "Tiempos de suspensión más largos"),
    ("src/libnickel.so.1.0.0.yaml", "Block WiFi firmware upgrade", "Bloquear actualización de firmware por WiFi"),
    ("src/libnickel.so.1.0.0.yaml", "Both page turn buttons go next", "Ambos botones avanzan página"),
    ("src/libnickel.so.1.0.0.yaml", "Disable all tutorial dialogs", "Desactivar tutoriales"),
    ("src/libnickel.so.1.0.0.yaml", "Increase page navigation history", "Historial de navegación más largo"),
    ("src/libnickel.so.1.0.0.yaml", "Dictionary text font-family/font-size/line-height", "Texto del diccionario más legible"),
    ("src/libnickel.so.1.0.0.yaml", "Allow USB storage even when device locked", "USB aunque el lector esté bloqueado"),
]


def _fetch_patch_files(ctx) -> dict[str, bytes]:
    files = {}
    for rel, sha in PATCH_SHA.items():
        files[rel] = ctx.fetch(Artifact(f"{KP_RAW}/4.45/{rel}", sha, f"kp-{KP_COMMIT[:7]}-{Path(rel).name}"))
    return files


def list_patches(yaml_text: str) -> list[tuple[str, bool]]:
    """(name, enabled_by_default) for each top-level patch in a kobopatch YAML file."""
    out, current = [], None
    for line in yaml_text.splitlines():
        m = re.match(r"^([^\s#][^:]*):\s*$", line)
        if m:
            current = m.group(1).strip()
            out.append([current, False])
            continue
        if current and re.match(r"^\s+-\s*Enabled:\s*yes\s*$", line):
            out[-1][1] = True
    return [(n, e) for n, e in out]


def _elf_ok(data: bytes) -> bool:
    return len(data) > 20 and data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1 and \
        int.from_bytes(data[18:20], "little") == 40  # EM_ARM


def _original_binaries(fw_zip: bytes) -> dict[str, bytes]:
    with zipfile.ZipFile(io.BytesIO(fw_zip)) as zf:
        root = zf.read("KoboRoot.tgz")
    wanted = set(TARGETS.values())
    out = {}
    with tarfile.open(fileobj=io.BytesIO(root), mode="r:gz") as tf:
        for m in tf.getmembers():
            name = m.name.lstrip("./")
            if name in wanted and m.isfile():
                out[name] = tf.extractfile(m).read()
    return out


def _tgz(files: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.GNU_FORMAT) as tf:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo("./" + name)
            info.size, info.mode, info.uname, info.gname = len(data), 0o755, "root", "root"
            tf.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def _fw_apply(mod, ctx, opts):
    dev = ctx.device
    key = (dev.firmware, dev.model_id)
    if key not in FIRMWARE:
        raise AioError(f"Parches disponibles solo para: {', '.join(f'{f} ({m})' for f, m in FIRMWARE)}. "
                       f"Tu Kobo tiene {dev.firmware} ({dev.model_id}).")
    if not opts["confirm"]:
        raise AioError("Confirma que entiendes el riesgo (opción «Entiendo el riesgo») para parchear el firmware.")
    machine = "arm64" if platform.machine() == "arm64" else None
    if opts["mode"] == "patch" and machine is None:
        raise AioError("kobopatch para este Mac (Intel) no está fijado todavía; usa KoboPatch Web UI.")
    ctx.progress("Preparando firmware oficial (162 MB, se guarda en caché)…")
    fw = ctx.fetch(FIRMWARE[key])
    originals = _original_binaries(fw)
    if opts["mode"] == "restore":
        targets = sorted(set(TARGETS.values()))
        ctx.add_firmware("firmware original", _tgz({t: originals[t] for t in targets}))
        ctx.txn.note("Se restaurarán nickel, libnickel, libadobe y librmsdk originales al reiniciar.")
        return
    selected = list(opts["patches"])
    if not selected:
        raise AioError("Elige al menos un parche")
    files = _fetch_patch_files(ctx)
    blacklist = json.loads(ctx.fetch(Artifact(f"{KP_RAW}/blacklist.json", BLACKLIST_SHA, f"kp-{KP_COMMIT[:7]}-blacklist.json")))
    banned = {n for names in blacklist.get("4.45", {}).values() for n in names}
    overrides: dict[str, dict[str, bool]] = {}
    known = {rel: {n for n, _ in list_patches(data.decode("utf-8"))} for rel, data in files.items()}
    for sel in selected:
        rel, _, name = sel.partition("::")
        if name in banned:
            raise AioError(f"«{name}» está marcado como no funcional en 4.45")
        if name not in known.get(rel, set()):
            raise AioError(f"Parche desconocido: {name}")
        overrides.setdefault(rel, {})[name] = True
    if ctx.txn.dry_run:
        ctx.txn.note(f"Parchear firmware {dev.firmware} con {len(selected)} parche(s): " +
                     ", ".join(s.partition('::')[2] for s in selected))
        ctx.add_firmware("kobopatch (simulado)", b"")
        return
    tool = Path.home() / "Kobo-AIO" / "bin" / KOBOPATCH[machine].filename
    data = ctx.fetch(KOBOPATCH[machine])
    tool.parent.mkdir(parents=True, exist_ok=True)
    if not tool.exists() or tool.read_bytes() != data:
        tool.write_bytes(data)
        tool.chmod(0o755)
        subprocess.run(["xattr", "-d", "com.apple.quarantine", str(tool)], capture_output=True)
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        (t / "src").mkdir()
        (t / "out").mkdir()
        for rel, content in files.items():
            (t / rel).write_bytes(content)
        (t / "firmware.zip").write_bytes(fw)
        lines = [f'version: "{dev.firmware}"', "in: firmware.zip", "out: out/KoboRoot.tgz", "log: out/log.txt",
                 "patchFormat: kobopatch",
                 "patches:"]
        lines += [f"  {rel}: {target}" for rel, target in TARGETS.items()]
        lines.append("overrides:")
        for rel in TARGETS:
            names = overrides.get(rel, {})
            lines.append(f"  {rel}:")
            for n, _ in list_patches(files[rel].decode("utf-8")):
                lines.append(f"    {json.dumps(n)}: {'yes' if names.get(n) else 'no'}")
        (t / "kobopatch.yaml").write_text("\n".join(lines) + "\n", encoding="utf-8")
        ctx.progress("Ejecutando kobopatch…")
        proc = subprocess.run([str(tool), "kobopatch.yaml"], cwd=t, capture_output=True, text=True)
        if proc.returncode != 0:
            log = (t / "out" / "log.txt").read_text(errors="replace")[-2000:] if (t / "out" / "log.txt").exists() else ""
            raise AioError(f"kobopatch falló: {(proc.stdout + proc.stderr)[-1500:]}\n{log}")
        out = (t / "out" / "KoboRoot.tgz").read_bytes()
    patched: dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(out), mode="r:gz") as tf:
        for m in tf.getmembers():
            name = m.name.lstrip("./")
            if m.isdir():
                continue
            if name not in originals:
                raise AioError(f"kobopatch generó un archivo inesperado: {name}")
            blob = tf.extractfile(m).read()
            if len(blob) != len(originals[name]) or not _elf_ok(blob):
                raise AioError(f"Validación fallida para {name} (tamaño o ELF distinto del original)")
            if blob != originals[name]:
                patched[name] = blob  # ship only binaries that actually changed
    if not patched:
        raise AioError("Los parches elegidos no modificaron ningún binario (¿ya incluidos en el firmware?)")
    ctx.add_firmware("kobopatch", _tgz(patched))
    ctx.txn.note(f"Firmware parcheado y validado: {', '.join(sorted(patched))}")


_featured_choices = [(f"{rel}::{name}", f"{label} ({name})") for rel, name, label in FEATURED]
register(Module(
    id="fw.kobopatch", category="Sistema", title="Parches de firmware (kobopatch, avanzado)",
    description="Modifica Nickel con parches de la comunidad: interlineado y márgenes finos, tamaños de fuente, "
                "encabezados limpios, tiempos de suspensión y más. Se pierden al actualizar el firmware. Si algo "
                "falla, «Restaurar originales» o reinstalar el firmware lo revierte.",
    risk="alto", reboot=True, model_ids=SUPPORTED_MODELS, exact_firmwares=SUPPORTED_FIRMWARES, max_fw=(5,),
    persistent=True,
    options=[
        Option("mode", "Acción", "choice", "patch", [("patch", "Aplicar parches"), ("restore", "Restaurar originales")]),
        Option("patches", "Parches", "multi", [], _featured_choices),
        Option("confirm", "Entiendo el riesgo y tengo respaldo", "bool", False),
    ],
    sources=["https://github.com/pgaskin/kobopatch", "https://www.mobileread.com/forums/showthread.php?t=373863",
             "https://github.com/nicoverbruggen/kobopatch-webui"],
    license="MIT (kobopatch)",
    apply_fn=_fw_apply,
))
