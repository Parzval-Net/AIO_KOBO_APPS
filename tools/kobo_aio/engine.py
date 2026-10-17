"""Plan / apply / uninstall orchestration over the module registry."""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Callable

from . import __version__
from .core import REGISTRY, STATE_REL, AioError, Context, load_state, merge_koboroots, resolve_order, validate_koboroot
from .device import KoboDevice
from .txn import Transaction, rollback


def catalog(dev: KoboDevice | None = None) -> list[dict]:
    from . import modules  # noqa: F401  (registers built-in modules)
    return [m.to_dict(dev) for m in REGISTRY.values()]


def _really_installed(installed: dict, dev: KoboDevice) -> dict:
    """state.json entries whose module still looks installed on the device (a firmware update can wipe mods)."""
    out = {}
    for mid, info in installed.items():
        mod = REGISTRY.get(mid)
        if mod is None:
            continue
        status = mod.status(dev) if mod.status_fn else "instalado"
        if status.startswith("no ") or status in ("no instalado", "inactivo"):
            continue
        out[mid] = info
    return out


def _run(dev: KoboDevice, selection: dict[str, dict], uninstall: list[str], dry_run: bool,
         offline: bool, progress: Callable[[str], None], label: str) -> dict:
    from . import modules  # noqa: F401

    txn = Transaction(root=dev.root, device_serial=dev.serial, dry_run=dry_run, label=label)
    ctx = Context(device=dev, txn=txn, offline=offline, state=load_state(dev.root), progress=progress)
    try:
        return _run_txn(dev, selection, uninstall, dry_run, progress, txn, ctx)
    except Exception:
        if not dry_run and txn.changes:
            progress("Error: revirtiendo los cambios de esta corrida…")
            try:
                for line in rollback(txn.backup_dir, dev.root):
                    progress(f"  {line}")
            except Exception as rb_exc:  # noqa: BLE001 - report but keep the original error
                progress(f"  No se pudo revertir automáticamente: {rb_exc}. Usa Respaldos > Revertir.")
        raise


def _run_txn(dev, selection, uninstall, dry_run, progress, txn: Transaction, ctx: Context) -> dict:
    installed: dict = ctx.state.setdefault("installed", {})
    results: list[dict] = []

    for mid in uninstall:
        mod = REGISTRY.get(mid)
        if mod is None or mod.uninstall_fn is None:
            raise AioError(f"{mid} no se puede desinstalar automáticamente")
        dependents = [REGISTRY[d].title for d in installed if d in REGISTRY and mid in REGISTRY[d].requires
                      and d not in uninstall]
        if dependents:
            raise AioError(f"No se puede desinstalar {mod.title}: lo usan {', '.join(dependents)}. "
                           "Desinstálalos también o déjalo instalado.")
        progress(f"Desinstalando {mod.title}...")
        mod.uninstall_fn(mod, ctx)
        installed.pop(mid, None)
        results.append({"id": mid, "action": "uninstall", "ok": True})

    order = resolve_order(list(selection), _really_installed(installed, dev))
    for mid in order:
        mod = REGISTRY[mid]
        ok, why = mod.compatible(dev)
        if not ok:
            results.append({"id": mid, "action": "skip", "ok": False, "reason": why})
            continue
        opts = mod.resolved_options(selection.get(mid))
        progress(f"Aplicando {mod.title}...")
        before = len(txn.changes)
        mod.apply_fn(mod, ctx, opts)
        if mid == "fw.kobopatch" and opts.get("mode") == "restore":
            installed.pop(mid, None)
        elif mod.persistent:
            installed[mid] = {"options": opts, "time": time.strftime("%Y-%m-%dT%H:%M:%S"), "aio": __version__}
        results.append({"id": mid, "action": "apply", "ok": True, "changes": len(txn.changes) - before})

    parts = ctx.koboroot_parts + [(l, d) for l, d in ctx.firmware_parts if d]
    if ctx.firmware_parts and dry_run:
        txn.note("KoboRoot.tgz incluirá binarios de Nickel parcheados (se generan al aplicar)")
    if parts:
        labels = ", ".join(l for l, _ in parts)
        pending = txn.read_bytes(".kobo/KoboRoot.tgz")
        if pending:
            try:
                validate_koboroot(pending, "KoboRoot.tgz pendiente")
            except AioError as exc:
                raise AioError(f"Hay un KoboRoot.tgz pendiente que no es de mods ({exc}). Expulsa el Kobo y deja "
                               "que termine de instalarse antes de continuar.") from exc
            labels += " + lo que ya estaba pendiente"
        merged = merge_koboroots(parts, pending)
        txn.write_bytes(".kobo/KoboRoot.tgz", merged, note=f"se instala al expulsar y reiniciar: {labels}")

    if txn.changes:
        ctx.state["aio_version"] = __version__
        ctx.state["updated"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        txn.write_text(STATE_REL, json.dumps(ctx.state, ensure_ascii=False, indent=2), note="registro del AIO")
    manifest = txn.commit()
    return {
        "dry_run": dry_run,
        "results": results,
        "changes": [c.to_dict() for c in txn.changes],
        "log": txn.log,
        "manifest": str(manifest) if manifest else None,
        "needs_reboot": any(REGISTRY[r["id"]].reboot for r in results if r["action"] == "apply" and r["ok"])
                        or bool(ctx.koboroot_parts) or bool(ctx.firmware_parts),
    }


def plan(dev: KoboDevice, selection: dict[str, dict], uninstall: list[str] | None = None,
         offline: bool = False, progress: Callable[[str], None] = lambda m: None) -> dict:
    return _run(dev, selection, uninstall or [], True, offline, progress, "plan")


def apply(dev: KoboDevice, selection: dict[str, dict], uninstall: list[str] | None = None,
          offline: bool = False, progress: Callable[[str], None] = lambda m: None) -> dict:
    # Download everything during the dry run first so a network failure never leaves a half install.
    plan(dev, selection, uninstall, offline, progress)
    return _run(dev, selection, uninstall or [], False, offline, progress, "apply")


def installed_modules(root: Path) -> dict:
    return load_state(root).get("installed", {})
