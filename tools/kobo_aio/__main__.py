"""Command line interface: python3 -m kobo_aio <command> (run from the tools/ directory)."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import __version__, engine
from .checks import run_doctor
from .core import REGISTRY, AioError
from .device import find_device
from .nickelmenu import lint_dir
from .txn import list_backups, rollback

SEV_ICON = {"critical": "[CRÍTICO]", "warn": "[AVISO]", "info": "[INFO]", "ok": "[OK]"}


def _parse_value(v: str):
    if v.lower() in ("true", "yes", "si", "sí", "1"):
        return True
    if v.lower() in ("false", "no", "0"):
        return False
    if "," in v:
        return [x for x in v.split(",") if x]
    return v


def _selection(args) -> dict[str, dict]:
    sel: dict[str, dict] = {m: {} for m in (args.select or "").split(",") if m}
    for kv in args.opt or []:
        key, _, value = kv.partition("=")
        mid, _, oid = key.rpartition(".")
        sel.setdefault(mid, {})[oid] = _parse_value(value)
    return sel


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="kobo_aio", description=f"Kobo AIO {__version__}")
    ap.add_argument("--root", type=Path, help="Raíz del Kobo montado (por defecto: autodetectar)")
    ap.add_argument("--json", action="store_true", help="Salida JSON")
    ap.add_argument("--offline", action="store_true", help="Usar solo la caché de descargas")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("detect")
    sub.add_parser("doctor")
    sub.add_parser("catalog")
    ex = sub.add_parser("export-catalog", help="Catálogo sin Kobo para el modo exploración de la web")
    ex.add_argument("out", type=Path)
    for name in ("plan", "apply"):
        p = sub.add_parser(name)
        p.add_argument("--select", help="ids de módulos separados por coma")
        p.add_argument("--opt", action="append", help="modulo.opcion=valor (repetible)")
        p.add_argument("--uninstall", help="ids a desinstalar, separados por coma")
    sub.add_parser("backups")
    rb = sub.add_parser("rollback")
    rb.add_argument("manifest", type=Path)
    rb.add_argument("--dry-run", action="store_true")
    sub.add_parser("lint-nm")
    sv = sub.add_parser("serve", help="Abrir la interfaz web local")
    sv.add_argument("--port", type=int, default=8765)
    sv.add_argument("--no-browser", action="store_true")
    args = ap.parse_args(argv)

    if args.cmd == "serve":
        from .server import serve
        serve(args.port, not args.no_browser)
        return 0

    out = lambda obj: print(json.dumps(obj, ensure_ascii=False, indent=2))  # noqa: E731
    if args.cmd == "export-catalog":
        args.out.write_text(json.dumps(engine.demo_catalog(), ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        return 0
    if args.cmd == "catalog":
        dev = find_device(args.root)
        out(engine.catalog(dev))
        return 0

    dev = find_device(args.root)
    if dev is None:
        print("No se encontró un Kobo montado. Conéctalo y toca «Conectar» en la pantalla.", file=sys.stderr)
        return 2
    try:
        if args.cmd == "detect":
            out(dev.to_dict())
        elif args.cmd == "doctor":
            rep = run_doctor(dev)
            rep["nickelmenu_lint"] = lint_dir(dev.root / ".adds" / "nm")
            if args.json:
                out(rep)
            else:
                print(f"{dev.name} | firmware {dev.firmware} | {rep['books_on_disk']} libros en disco")
                for f in rep["findings"]:
                    print(f"{SEV_ICON[f['severity']]:10} {f['title']}" + (f"\n           {f['detail']}" if f["detail"] else ""))
                for i in rep["nickelmenu_lint"]:
                    print(f"[AVISO]    NickelMenu {i['file']}:{i['line']}: {i['error']}")
        elif args.cmd in ("plan", "apply"):
            sel = _selection(args)
            unin = [m for m in (args.uninstall or "").split(",") if m]
            fn = engine.plan if args.cmd == "plan" else engine.apply
            res = fn(dev, sel, unin, offline=args.offline, progress=lambda m: print("..", m, file=sys.stderr))
            if args.json:
                out(res)
            else:
                for line in res["log"]:
                    print(" -", line)
                for r in res["results"]:
                    if not r["ok"]:
                        print(f" ! {r['id']}: {r.get('reason', '')}")
                print(f"{len(res['changes'])} cambios" + (" (simulación)" if res["dry_run"] else ""))
                if res["manifest"]:
                    print(f"Respaldo y registro: {res['manifest']}")
                if res["needs_reboot"]:
                    print("Expulsa el Kobo: instalará los mods de sistema y se reiniciará solo.")
        elif args.cmd == "backups":
            for m in list_backups(dev.serial):
                print(m)
        elif args.cmd == "rollback":
            for a in rollback(args.manifest, dev.root, dry_run=args.dry_run, expected_serial=dev.serial):
                print(" -", a)
        elif args.cmd == "lint-nm":
            out(lint_dir(dev.root / ".adds" / "nm"))
    except AioError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
