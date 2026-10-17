"""Validation matrix: every Kobo model x firmware line, real engine + cached pinned artifacts, mock devices."""
import io, json, os, shutil, sqlite3, sys, tarfile, tempfile, traceback, zipfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from PIL import Image
from kobo_aio import engine, txn as txnmod
from kobo_aio.checks import run_doctor
from kobo_aio.comics import ComicOptions, convert
from kobo_aio.core import validate_koboroot
from kobo_aio.device import KOBO_MODELS, parse_version_file
from kobo_aio.nickelmenu import lint_dir

BASE = ["maint.backup", "conf.exclude_sync", "mod.nickelmenu", "nm.menu", "mod.nickelhome", "mod.nickeltypefix",
        "fonts.ebook", "maint.clean_macos", "maint.no_spotlight"]
FULL = BASE + ["mod.nickelclock", "app.koreader", "app.koreader_plugins", "app.inkboy", "mod.nickeldissolve",
               "mod.nickelmodmanager", "conf.sideloaded", "nm.owner"]
FIRMWARES = ["4.6.9960", "4.38.23828", "4.45.23697", "6.0.274403"]
work = Path(tempfile.mkdtemp())
txnmod.HOST_BACKUP_ROOT = work / "backups"
import kobo_aio.modules.maintenance as maint
maint.HOST_LIBRARY = work / "library"

comic = work / "test.cbz"
with zipfile.ZipFile(comic, "w") as z:
    for i, (w, h) in enumerate([(1200, 1800), (2400, 1700)]):
        b = io.BytesIO(); Image.new("RGB", (w, h), (200, 50, 50)).save(b, "JPEG"); z.writestr(f"{i}.jpg", b.getvalue())

problems, rows = [], []
for mid, model in KOBO_MODELS.items():
    for fw in FIRMWARES:
        root = work / f"k{mid}_{fw}"
        (root / ".kobo/Kobo").mkdir(parents=True)
        (root / ".kobo/version").write_text(f"N000000000000,4.9.77,{fw},4.9.77,4.9.77,00000000-0000-0000-0000-000000000{mid}\n")
        (root / ".kobo/Kobo/Kobo eReader.conf").write_text("[ApplicationPreferences]\nfoo=1\n\n[FeatureSettings]\nScreenshots=false\n")
        db = sqlite3.connect(root / ".kobo/KoboReader.sqlite"); db.execute("CREATE TABLE content (ContentID TEXT, ContentType INT)"); db.commit(); db.close()
        tag = f"{model['name']} ({mid}) fw {fw}"
        try:
            dev = parse_version_file(root)
            assert dev.firmware == fw and dev.model_id == mid, "detección"
            rep = run_doctor(dev)
            cat = {m["id"]: m for m in engine.catalog(dev)}
            ok_ids = [i for i, m in cat.items() if m["compatible"]]
            major = int(fw.split(".")[0])
            # invariants
            for i in ("mod.nickelmenu", "mod.nickelhome", "mod.nickeltypefix", "mod.nickelclock", "mod.nickelmodmanager", "nm.menu"):
                if major >= 5 and cat[i]["compatible"]:
                    problems.append(f"{tag}: {i} compatible en firmware {fw}")
            if not model["bluetooth"] and cat["lib.audiobooks"]["compatible"]:
                problems.append(f"{tag}: audiolibros sin Bluetooth")
            if cat["mod.nickeldissolve"]["compatible"] and mid not in ("390", "391", "393", "395"):
                problems.append(f"{tag}: NickelDissolve fuera de MTK")
            if cat["fw.kobopatch"]["compatible"] and not (mid == "390" and fw == "4.45.23697"):
                problems.append(f"{tag}: kobopatch sin firmware soportado")
            # install everything that is compatible (real writes on the mock, pinned cached artifacts)
            sel = {i: {} for i in FULL if i in ok_ids}
            res = engine.apply(dev, sel, offline=os.environ.get("KOBO_AIO_ONLINE") != "1")
            applied = [r["id"] for r in res["results"] if r["action"] == "apply"]
            kr = root / ".kobo/KoboRoot.tgz"
            if kr.exists():
                validate_koboroot(kr.read_bytes(), "final")
            lint = [i for i in lint_dir(root / ".adds/nm") if i["level"] == "warn"]
            if lint:
                problems.append(f"{tag}: lint NickelMenu {lint[:2]}")
            conf = (root / ".kobo/Kobo/Kobo eReader.conf").read_text()
            if "ExcludeSyncFolders=(\\\\.(?!kobo|adobe)" not in conf:
                problems.append(f"{tag}: ExcludeSyncFolders mal escrito")
            if "foo=1" not in conf:
                problems.append(f"{tag}: se perdió una clave del conf")
            # dark mode / bluetooth shortcuts respect hardware
            menu = (root / ".adds/nm/kobo-aio-menu")
            if menu.exists():
                t = menu.read_text()
                if not model["dark_mode"] and "dark_mode" in t: problems.append(f"{tag}: atajo modo oscuro sin soporte")
                if not model["bluetooth"] and "nickel_bluetooth" in t: problems.append(f"{tag}: atajo Bluetooth sin hardware")
            # comics fit the exact panel
            name, data = convert(comic, mid, ComicOptions())
            zz = zipfile.ZipFile(io.BytesIO(data))
            for n in zz.namelist():
                if n.endswith(".jpg"):
                    im = Image.open(io.BytesIO(zz.read(n)))
                    if im.size[0] > model["width"] or im.size[1] > model["height"]:
                        problems.append(f"{tag}: página {im.size} excede {model['width']}x{model['height']}")
                    if not model["color"] and im.mode != "L":
                        problems.append(f"{tag}: color en pantalla B/N")
            # rollback leaves the original conf
            txnmod.rollback(Path(res["manifest"]), root) if res["manifest"] else None
            if res["manifest"] and "ExcludeSyncFolders" in (root / ".kobo/Kobo/Kobo eReader.conf").read_text():
                problems.append(f"{tag}: rollback no restauró el conf")
            rows.append((tag, len(ok_ids), len(applied), len(res["changes"]), rep["summary"].get("critical", 0)))
        except Exception as exc:  # noqa: BLE001
            problems.append(f"{tag}: EXCEPCIÓN {type(exc).__name__}: {exc}")
            traceback.print_exc(limit=2)
        shutil.rmtree(root, ignore_errors=True)

print(f"{len(rows)} combinaciones OK de {len(KOBO_MODELS) * len(FIRMWARES)}")
for r in rows[::4][:8]:
    print("  ", r)
print("PROBLEMAS:", len(problems))
for p in problems[:40]:
    print(" -", p)
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if problems else 0)
