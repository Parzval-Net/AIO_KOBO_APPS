"""Kobo AIO test suite (stdlib unittest). Run: python3 -m unittest discover -s tests -p 'test_kobo_aio.py'"""

from __future__ import annotations

import io
import json
import shutil
import sqlite3
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from kobo_aio import engine, txn as txnmod  # noqa: E402
from kobo_aio.checks import EXCLUDE_SYNC_RAW, run_doctor  # noqa: E402
from kobo_aio.core import AioError, merge_koboroots, validate_koboroot  # noqa: E402
from kobo_aio.device import parse_version_file  # noqa: E402
from kobo_aio.nickelmenu import CATALOG, lint_dir, lint_line  # noqa: E402
from kobo_aio.qtini import QtIni, qt_escape, qt_unescape  # noqa: E402

BROKEN_CONF = """[ApplicationPreferences]
SideloadedMode=true

[FeatureSettings]
ExcludeSyncFolders=((?!kobo|adobe).+|([^.][^/]*/)+.+)
Screenshots=false

[PowerOptions]
AutoOffMinutes=30
"""


def make_kobo(root: Path, model="390", conf=BROKEN_CONF, books=("Verne, Julio/Viaje.kepub.epub",)) -> Path:
    (root / ".kobo" / "Kobo").mkdir(parents=True)
    (root / ".kobo" / "version").write_text(
        f"N428000000000,4.9.77,4.45.23697,4.9.77,4.9.77,00000000-0000-0000-0000-000000000{model}\n")
    (root / ".kobo" / "Kobo" / "Kobo eReader.conf").write_text(conf)
    db = sqlite3.connect(root / ".kobo" / "KoboReader.sqlite")
    db.execute("CREATE TABLE content (ContentID TEXT, ContentType INT)")
    db.commit()
    db.close()
    for b in books:
        p = root / b
        p.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(p, "w") as z:
            z.writestr("mimetype", "application/epub+zip")
    return root


def tgz(members: dict[str, bytes]) -> bytes:
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w:gz") as tf:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    return out.getvalue()


class TempKobo(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.root = make_kobo(self.tmp / "KOBOeReader")
        self._old_backup = txnmod.HOST_BACKUP_ROOT
        txnmod.HOST_BACKUP_ROOT = self.tmp / "backups"

    def tearDown(self):
        txnmod.HOST_BACKUP_ROOT = self._old_backup
        shutil.rmtree(self.tmp, ignore_errors=True)


class QtIniTests(unittest.TestCase):
    def test_exclude_sync_escaping_matches_community_value(self):
        plain = r"(\.(?!kobo|adobe).+|([^.][^/]*/)+\..+)"
        self.assertEqual(qt_escape(plain), EXCLUDE_SYNC_RAW)
        self.assertEqual(qt_unescape(EXCLUDE_SYNC_RAW), plain)

    def test_set_only_touches_target_line(self):
        ini = QtIni(BROKEN_CONF)
        self.assertTrue(ini.set("FeatureSettings", "ExcludeSyncFolders", qt_unescape(EXCLUDE_SYNC_RAW)))
        out = ini.dumps()
        diff = [(a, b) for a, b in zip(BROKEN_CONF.splitlines(), out.splitlines()) if a != b]
        self.assertEqual(len(diff), 1)
        self.assertIn(EXCLUDE_SYNC_RAW, diff[0][1])
        self.assertTrue(out.endswith("\n"))

    def test_set_new_key_and_section(self):
        ini = QtIni(BROKEN_CONF)
        ini.set("Reading", "webkitTextRendering", "optimizeLegibility")
        ini.set("FeatureSettings", "Foo", "a;b")
        self.assertEqual(ini.get("Reading", "webkitTextRendering"), "optimizeLegibility")
        self.assertEqual(ini.get_raw("FeatureSettings", "Foo"), '"a;b"')
        self.assertFalse(ini.set("Reading", "webkitTextRendering", "optimizeLegibility"))

    def test_crlf_preserved(self):
        ini = QtIni("[A]\r\nx=1\r\n")
        ini.set("A", "y", "2")
        self.assertEqual(ini.dumps(), "[A]\r\nx=1\r\ny=2\r\n")


class DeviceTests(TempKobo):
    def test_parse_version(self):
        dev = parse_version_file(self.root)
        self.assertEqual(dev.model_id, "390")
        self.assertEqual(dev.firmware, "4.45.23697")
        self.assertEqual(dev.name, "Kobo Libra Colour")


class TransactionTests(TempKobo):
    def test_denylist_and_allowlist(self):
        t = txnmod.Transaction(self.root, "S")
        with self.assertRaises(txnmod.SafetyError):
            t.write_text(".kobo/KoboReader.sqlite", "x")
        with self.assertRaises(txnmod.SafetyError):
            t.write_text("Verne, Julio/Viaje.kepub.epub", "x")
        with self.assertRaises(txnmod.SafetyError):
            t.write_text("../escape", "x")

    def test_rollback_restores_and_removes(self):
        conf = self.root / ".kobo/Kobo/Kobo eReader.conf"
        t = txnmod.Transaction(self.root, "S")
        t.write_text(".kobo/Kobo/Kobo eReader.conf", "changed")
        t.write_text(".adds/new/file.txt", "new")
        manifest = t.commit()
        self.assertEqual(conf.read_text(), "changed")
        txnmod.rollback(manifest, self.root)
        self.assertEqual(conf.read_text(), BROKEN_CONF)
        self.assertFalse((self.root / ".adds/new/file.txt").exists())

    def test_rollback_after_interruption_uses_journal(self):
        t = txnmod.Transaction(self.root, "S")
        t.write_text(".adds/a.txt", "a")
        t.write_text(".kobo/Kobo/Kobo eReader.conf", "half")
        # simulate a crash: no commit()
        self.assertFalse((t.backup_dir / "manifest.json").exists())
        txnmod.rollback(t.backup_dir / "manifest.json", self.root)
        self.assertEqual((self.root / ".kobo/Kobo/Kobo eReader.conf").read_text(), BROKEN_CONF)
        self.assertFalse((self.root / ".adds/a.txt").exists())

    def test_dry_run_writes_nothing(self):
        t = txnmod.Transaction(self.root, "S", dry_run=True)
        t.write_text(".adds/x.txt", "x")
        self.assertEqual(t.read_text(".adds/x.txt"), "x")
        self.assertFalse((self.root / ".adds/x.txt").exists())
        self.assertIsNone(t.commit())

    def test_junk_requires_appledouble_magic(self):
        fake = self.root / "Verne, Julio" / "._notjunk.epub"
        fake.write_bytes(b"PK\x03\x04 real data")
        real = self.root / "Verne, Julio" / "._junk.epub"
        real.write_bytes(b"\x00\x05\x16\x07xx")
        t = txnmod.Transaction(self.root, "S")
        with self.assertRaises(txnmod.SafetyError):
            t.delete_junk(fake)
        t.delete_junk(real)
        self.assertTrue(fake.exists())
        self.assertFalse(real.exists())


class KoboRootTests(unittest.TestCase):
    def test_rejects_system_paths(self):
        with self.assertRaises(AioError):
            validate_koboroot(tgz({"./usr/local/Kobo/nickel": b"x"}), "evil")
        with self.assertRaises(AioError):
            validate_koboroot(tgz({"./etc/init.d/rcS": b"x"}), "evil")
        validate_koboroot(tgz({"./usr/local/Kobo/imageformats/libx.so": b"x", "./mnt/onboard/.adds/x/doc": b"d"}), "ok")

    def test_merge_and_conflict(self):
        a = tgz({"./usr/local/Kobo/imageformats/liba.so": b"a"})
        b = tgz({"./usr/local/Kobo/imageformats/libb.so": b"b"})
        merged = merge_koboroots([("a", a), ("b", b)])
        with tarfile.open(fileobj=io.BytesIO(merged)) as tf:
            self.assertEqual(sorted(m.name for m in tf.getmembers()),
                             ["./usr/local/Kobo/imageformats/liba.so", "./usr/local/Kobo/imageformats/libb.so"])
        with self.assertRaises(AioError):
            merge_koboroots([("a", a), ("a2", a)])


class NickelMenuTests(unittest.TestCase):
    def test_catalog_is_valid(self):
        for item in CATALOG:
            for line in item.render().splitlines():
                self.assertIsNone(lint_line(line), f"{item.id}: {line}")

    def test_lint_catches_common_mistakes(self):
        self.assertIsNotNone(lint_line("menu_item :main :WiFi :nickel_setting :toggle :wifi"))
        self.assertIsNotNone(lint_line("menu_item :home :X :power :reboot"))
        self.assertIsNotNone(lint_line("menu_item :main :X :power :restart"))
        self.assertIsNotNone(lint_line("menu_item :main :X :cmd_output :uname -a"))
        self.assertIsNone(lint_line("menu_item :main :IP :cmd_output :500:/sbin/ifconfig"))
        self.assertIsNone(lint_line("  chain_success :dbg_toast :Hola"))

    def test_duplicate_experimental_detected(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "a").write_text("experimental :menu_main_15505_label :A\n")
            Path(d, "b").write_text("experimental :menu_main_15505_label :B\n")
            issues = lint_dir(Path(d))
            self.assertTrue(any("ya está definido" in i["error"] for i in issues))


class DoctorTests(TempKobo):
    def test_detects_broken_exclude_and_missing_books(self):
        (self.root / "metadata.calibre").write_text(json.dumps(
            [{"lpath": "Verne, Julio/Viaje.kepub.epub"}, {"lpath": "Gone/Book.epub"}]))
        rep = run_doctor(parse_version_file(self.root))
        sev = {f["id"]: f["severity"] for f in rep["findings"]}
        self.assertEqual(sev["exclude_sync"], "critical")
        self.assertEqual(sev["calibre_missing"], "warn")
        self.assertEqual(rep["books_on_disk"], 1)


class EngineTests(TempKobo):
    """Offline install of modules that need no downloads."""

    def test_settings_menu_and_maintenance(self):
        (self.root / "Verne, Julio" / "._Viaje.kepub.epub").write_bytes(b"\x00\x05\x16\x07j")
        dev = parse_version_file(self.root)
        sel = {"conf.exclude_sync": {}, "nm.menu": {}, "maint.clean_macos": {}, "maint.no_spotlight": {}}
        # nm.menu requires mod.nickelmenu (download); pretend it is installed by skipping dependency resolution
        from kobo_aio.core import REGISTRY
        engine.catalog()
        saved = REGISTRY["nm.menu"].requires
        REGISTRY["nm.menu"].requires = []
        try:
            res = engine.apply(dev, sel, offline=True)
        finally:
            REGISTRY["nm.menu"].requires = saved
        conf = (self.root / ".kobo/Kobo/Kobo eReader.conf").read_text()
        self.assertIn("ExcludeSyncFolders=" + EXCLUDE_SYNC_RAW, conf)
        self.assertIn("SideloadedMode=true", conf)
        menu = (self.root / ".adds/nm/kobo-aio-menu").read_text()
        for line in menu.splitlines():
            self.assertIsNone(lint_line(line), line)
        self.assertFalse((self.root / "Verne, Julio" / "._Viaje.kepub.epub").exists())
        self.assertTrue((self.root / ".metadata_never_index").exists())
        self.assertTrue((self.root / "Verne, Julio/Viaje.kepub.epub").exists())
        state = json.loads((self.root / ".adds/kobo-aio/state.json").read_text())
        self.assertIn("nm.menu", state["installed"])
        self.assertNotIn("maint.clean_macos", state["installed"])
        # Second run is idempotent
        res2 = engine.plan(dev, {"conf.exclude_sync": {}}, offline=True)
        self.assertEqual([c for c in res2["changes"] if c["rel"] != ".adds/kobo-aio/state.json"], [])
        self.assertTrue(res["manifest"])

    def test_dark_mode_skipped_on_clara_hd(self):
        shutil.rmtree(self.root)
        root = make_kobo(self.tmp / "KOBOeReader", model="376")
        from kobo_aio.core import REGISTRY
        engine.catalog()
        saved = REGISTRY["nm.menu"].requires
        REGISTRY["nm.menu"].requires = []
        try:
            engine.apply(parse_version_file(root), {"nm.menu": {}}, offline=True)
        finally:
            REGISTRY["nm.menu"].requires = saved
        self.assertNotIn("dark_mode", (root / ".adds/nm/kobo-aio-menu").read_text())

    def test_incompatible_module_is_skipped(self):
        shutil.rmtree(self.root)
        root = make_kobo(self.tmp / "KOBOeReader", model="376")
        res = engine.plan(parse_version_file(root), {"mod.nickeldissolve": {}}, offline=True)
        self.assertEqual(res["results"][0]["action"], "skip")


def silent_mp3(seconds: float) -> bytes:
    """MPEG-1 Layer III, 128 kbps, 44.1 kHz frames of silence (417 bytes, 1152 samples each)."""
    frame = b"\xff\xfb\x90\x64" + b"\x00" * 413
    return frame * int(seconds * 44100 / 1152)


class AudiobookTests(TempKobo):
    def test_join_chapters_and_cover(self):
        from PIL import Image
        from kobo_aio.audiobooks import build, collect, embedded_cover, mp3_duration_ms, strip_tags
        src = self.tmp / "Julio Verne - Viaje al centro"
        src.mkdir()
        (src / "01 - Inicio.mp3").write_bytes(b"ID3\x03\x00\x00\x00\x00\x00\x0a" + b"\x00" * 10 + silent_mp3(3))
        (src / "02 - El volcan.mp3").write_bytes(silent_mp3(2) + b"TAG" + b"\x00" * 125)
        Image.new("RGB", (1400, 1400), (200, 30, 30)).save(src / "cover.png")
        book = collect(src)
        self.assertEqual((book.author, book.title), ("Julio Verne", "Viaje al centro"))
        name, data, fmt = build(book)
        self.assertEqual((name, fmt), ("Viaje al centro.mp3", "mp3"))
        self.assertTrue(b"CHAP" in data[:300000], "falta CHAP")
        self.assertTrue(b"CTOC" in data[:300000], "falta CTOC")
        cover = embedded_cover(data)
        self.assertTrue(cover.startswith(b"\xff\xd8"))
        self.assertLessEqual(max(Image.open(io.BytesIO(cover)).size), 1000)
        self.assertAlmostEqual(mp3_duration_ms(strip_tags(data)) / 1000, 5.0, delta=0.1)

    def test_nickel_mp3z(self):
        from kobo_aio.audiobooks import build_mp3z, collect, embedded_cover
        src = self.tmp / "Autor - Libro"
        src.mkdir()
        (src / "01 Uno.mp3").write_bytes(silent_mp3(2))
        (src / "02 Dos.mp3").write_bytes(silent_mp3(1))
        from PIL import Image
        Image.new("RGB", (300, 300), "blue").save(src / "folder.jpg")
        name, data, fmt = build_mp3z(collect(src))
        self.assertEqual((name, fmt), ("Libro.mp3z", "mp3z"))
        zf = zipfile.ZipFile(io.BytesIO(data))
        self.assertEqual(zf.namelist(), ["001 - Uno.mp3", "002 - Dos.mp3"])
        self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in zf.infolist()))
        self.assertTrue(embedded_cover(zf.read("001 - Uno.mp3")).startswith(b"\xff\xd8"))
        t = txnmod.Transaction(self.root, "S")
        self.assertEqual(t.add_book("Audiobooks/Autor/Libro/Libro.mp3z", data), "Audiobooks/Autor/Libro/Libro.mp3z")

    def test_mp3z_matches_stock_reader(self):
        """Rules recovered from libmp3z.so (4.45): ASCII names (toLatin1 lookup), sorted order == play order,
        only MP3 entries, long tracks split so the per-track bookmark resumes close by."""
        from kobo_aio.audiobooks import build_mp3z, collect, mp3_duration_ms, strip_tags
        src = self.tmp / "Autora - Canción de otoño"
        src.mkdir()
        (src / "1 - Capítulo único ¿Ñandú?.mp3").write_bytes(silent_mp3(150))
        (src / "2 - Épílogo.mp3").write_bytes(silent_mp3(20))
        name, data, _ = build_mp3z(collect(src), part_minutes=1)
        self.assertEqual(name, "Canción de otoño.mp3z")  # Nickel title = file name (Unicode OK there)
        zf = zipfile.ZipFile(io.BytesIO(data))
        names = zf.namelist()
        self.assertTrue(all(n.isascii() and n.endswith(".mp3") and "/" not in n for n in names), names)
        self.assertEqual(names, sorted(names))
        self.assertEqual(names[0], "001 - Capitulo unico Nandu (1-3).mp3")
        self.assertEqual(names[-1], "004 - Epilogo.mp3")
        self.assertEqual(len(names), 4)  # 150 s -> 60 + 60 + 30 s, plus the 20 s epilogue
        durs = [mp3_duration_ms(strip_tags(zf.read(n))) / 1000 for n in names]
        self.assertAlmostEqual(sum(durs), 170, delta=0.5)
        self.assertLessEqual(max(durs[:1]), 61)
        _, one, _ = build_mp3z(collect(src), part_minutes=0)
        self.assertEqual(len(zipfile.ZipFile(io.BytesIO(one)).namelist()), 2)

    def test_split_drops_xing_header(self):
        from kobo_aio.audiobooks import split_mp3
        xing = bytearray(b"\xff\xfb\x90\x64" + b"\x00" * 413)
        xing[36:40] = b"Xing"
        parts = split_mp3(bytes(xing) + silent_mp3(4), 2000)
        self.assertNotIn(b"Xing", b"".join(p for p, _ in parts))
        self.assertEqual(len(parts), 2)

    def test_audio_only_in_audiobooks_folder(self):
        t = txnmod.Transaction(self.root, "S")
        with self.assertRaises(txnmod.SafetyError):
            t.add_book("Libros/a.mp3", b"x")
        t.add_book("Audiobooks/A/B/b.mp3", b"x")
        self.assertEqual(t.add_book("Audiobooks/A/B/b.mp3", b"y"), "Audiobooks/A/B/b (2).mp3")
        with self.assertRaises(txnmod.SafetyError):
            t.add_book(".adds/x.epub", b"x")


class PendingKoboRootTests(unittest.TestCase):
    def test_pending_kept_and_new_wins(self):
        pending = tgz({"./usr/local/Kobo/imageformats/libhome.so": b"old-home",
                       "./usr/local/Kobo/imageformats/libnm.so": b"old-nm"})
        new = tgz({"./usr/local/Kobo/imageformats/libnm.so": b"new-nm"})
        merged = merge_koboroots([("nickelmenu", new)], pending)
        with tarfile.open(fileobj=io.BytesIO(merged)) as tf:
            got = {m.name.lstrip("./"): tf.extractfile(m).read() for m in tf.getmembers() if m.isfile()}
        self.assertEqual(got["usr/local/Kobo/imageformats/libnm.so"], b"new-nm")
        self.assertEqual(got["usr/local/Kobo/imageformats/libhome.so"], b"old-home")


class ComicTests(unittest.TestCase):
    def test_color_spread_split_rtl(self):
        from PIL import Image, ImageDraw
        from kobo_aio.comics import ComicOptions, convert
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "Serie 01.cbz"
            with zipfile.ZipFile(src, "w") as z:
                for i, (w, h, col) in enumerate([(1200, 1800, (220, 40, 40)), (2400, 1700, (90, 90, 90))]):
                    im = Image.new("RGB", (w, h), "white")
                    ImageDraw.Draw(im).ellipse([100, 100, w - 100, h - 100], fill=col)
                    b = io.BytesIO(); im.save(b, "JPEG"); z.writestr(f"{i}.jpg", b.getvalue())
            name, data = convert(src, "390", ComicOptions(manga=True))
            self.assertEqual(name, "Serie 01.kepub.epub")
            zz = zipfile.ZipFile(io.BytesIO(data))
            self.assertEqual(zz.namelist()[0], "mimetype")
            imgs = sorted(n for n in zz.namelist() if n.endswith(".jpg"))
            self.assertEqual(len(imgs), 3)  # 1 page + spread split in 2
            modes = [Image.open(io.BytesIO(zz.read(n))).mode for n in imgs]
            self.assertEqual(modes[0], "RGB")  # colour kept on Kaleido 3
            self.assertIn('page-progression-direction="rtl"', zz.read("OEBPS/content.opf").decode())
            for n in imgs:
                w, h = Image.open(io.BytesIO(zz.read(n))).size
                self.assertTrue(w <= 1264 and h <= 1680)
            _, gray = convert(src, "376", ComicOptions())
            zz = zipfile.ZipFile(io.BytesIO(gray))
            self.assertTrue(all(Image.open(io.BytesIO(zz.read(n))).mode == "L"
                                for n in zz.namelist() if n.endswith(".jpg")))


class ReviewRegressionTests(TempKobo):
    """Regression tests for the independent code review findings."""

    def test_rollback_keeps_files_changed_after_install(self):
        conf = self.root / ".kobo/Kobo/Kobo eReader.conf"
        t = txnmod.Transaction(self.root, "S")
        t.write_text(".kobo/Kobo/Kobo eReader.conf", "aio")
        m = t.commit()
        conf.write_text("nickel later")             # Nickel rewrote it weeks later
        actions = txnmod.rollback(m, self.root)
        self.assertEqual(conf.read_text(), "nickel later")
        self.assertTrue(any("Conservar" in a for a in actions))

    def test_delete_then_write_restores_original(self):
        f = self.root / ".adds/inkboy/inkboy.sh"
        f.parent.mkdir(parents=True)
        f.write_text("original")
        t = txnmod.Transaction(self.root, "S")
        t.delete(".adds/inkboy/inkboy.sh")
        t.write_text(".adds/inkboy/inkboy.sh", "new")
        m = t.commit()
        txnmod.rollback(m, self.root)
        self.assertEqual(f.read_text(), "original")

    def test_created_then_deleted_leaves_nothing(self):
        t = txnmod.Transaction(self.root, "S")
        t.write_text(".adds/tmp.txt", "x")
        t.delete(".adds/tmp.txt")
        self.assertFalse((self.root / ".adds/tmp.txt").exists())
        self.assertEqual(t.changes, [])

    def test_truncated_journal_is_still_rollbackable(self):
        t = txnmod.Transaction(self.root, "S")
        t.write_text(".adds/a.txt", "a")
        with open(t.backup_dir / "journal.jsonl", "a") as fh:
            fh.write('{"op": "write", "rel": ".adds/b.t')   # power cut mid-line
        txnmod.rollback(t.backup_dir, self.root)
        self.assertFalse((self.root / ".adds/a.txt").exists())

    def test_rollback_refuses_other_device_and_bad_paths(self):
        t = txnmod.Transaction(self.root, "OTHER")
        t.write_text(".adds/a.txt", "a")
        m = t.commit()
        with self.assertRaises(txnmod.SafetyError):
            txnmod.rollback(m, self.root, expected_serial="MINE")
        data = json.loads(m.read_text())
        data["changes"][0]["rel"] = "../../etc/evil"
        m.write_text(json.dumps(data))
        with self.assertRaises(txnmod.SafetyError):
            txnmod.rollback(m, self.root)

    def test_failed_apply_rolls_back_automatically(self):
        from kobo_aio.core import REGISTRY, Module, register
        engine.catalog()

        def boom(mod, ctx, opts):
            ctx.txn.write_text(".adds/nm/half", "x")
            raise RuntimeError("fallo simulado")
        if "test.boom" not in REGISTRY:
            register(Module(id="test.boom", category="Test", title="Boom", description="", apply_fn=boom))
        dev = parse_version_file(self.root)
        with self.assertRaises(RuntimeError):
            engine.apply(dev, {"conf.exclude_sync": {}, "test.boom": {}}, offline=True)
        self.assertFalse((self.root / ".adds/nm/half").exists())
        self.assertEqual((self.root / ".kobo/Kobo/Kobo eReader.conf").read_text(), BROKEN_CONF)
        del REGISTRY["test.boom"]

    def test_koboroot_links_rejected(self):
        out = io.BytesIO()
        with tarfile.open(fileobj=out, mode="w:gz") as tf:
            link = tarfile.TarInfo("./usr/local/Kobo/imageformats/x")
            link.type, link.linkname = tarfile.SYMTYPE, "/etc"
            tf.addfile(link)
        with self.assertRaises(AioError):
            validate_koboroot(out.getvalue(), "evil")
        with self.assertRaises(AioError):
            validate_koboroot(tgz({"../usr/local/Kobo/imageformats/x.so": b"x"}), "evil")

    def test_firmware_5_and_unknown_are_incompatible(self):
        from kobo_aio.core import REGISTRY
        engine.catalog()
        (self.root / ".kobo/version").write_text(
            "N428000000000,4.9.77,5.1.12345,4.9.77,4.9.77,00000000-0000-0000-0000-000000000390\n")
        dev = parse_version_file(self.root)
        self.assertEqual(dev.firmware, "5.1.12345")
        self.assertFalse(REGISTRY["mod.nickelmenu"].compatible(dev)[0])
        dev.firmware = ""
        self.assertFalse(REGISTRY["mod.nickelmenu"].compatible(dev)[0])

    def test_uninstall_blocked_by_dependents(self):
        state = {"installed": {"mod.nickelmenu": {}, "nm.menu": {}}}
        (self.root / ".adds/kobo-aio").mkdir(parents=True)
        (self.root / ".adds/kobo-aio/state.json").write_text(json.dumps(state))
        with self.assertRaises(AioError):
            engine.plan(parse_version_file(self.root), {}, ["mod.nickelmenu"], offline=True)

    def test_server_file_option_comma_bypass(self):
        from kobo_aio.server import _check_file_options
        engine.catalog()
        with self.assertRaises(AioError):
            _check_file_options({"lib.books": {"files": str(Path.home() / "Documents/x") + ",/etc/hosts"}})
        with self.assertRaises(AioError):
            _check_file_options({"lib.books": {"files": ["~/.ssh/id_rsa"]}})

    def test_dry_run_validates_audiobook_destination(self):
        src = self.tmp / ".."  # noqa: F841 - title below is the attack vector
        d = self.tmp / "pistas"
        d.mkdir()
        (d / "01.mp3").write_bytes(silent_mp3(1))
        res = engine.plan(parse_version_file(self.root),
                          {"lib.audiobooks": {"files": [str(d)], "title": "..", "author": "../../x"}}, offline=True)
        rels = [c["rel"] for c in res["changes"]]
        self.assertTrue(any(r.startswith("Audiobooks/x/Audiolibro/") for r in rels), rels)


class UxImprovementTests(TempKobo):
    def test_more_comic_inputs(self):
        from PIL import Image
        from kobo_aio.comics import ComicOptions, convert, expand_inputs, read_pages
        src = self.tmp / "s.cbt"
        with tarfile.open(src, "w") as tf:
            for i in range(2):
                b = io.BytesIO(); Image.new("RGB", (800, 1200), "red").save(b, "JPEG")
                ti = tarfile.TarInfo(f"{i}.jpg"); ti.size = len(b.getvalue()); tf.addfile(ti, io.BytesIO(b.getvalue()))
        self.assertEqual(len(read_pages(src)), 2)
        name, data = convert(src, "390", ComicOptions())
        epub = self.tmp / "c.epub"
        epub.write_bytes(data)
        self.assertEqual(len(read_pages(epub)), 2)  # EPUB comic input, spine order
        img = self.tmp / "one.png"
        Image.new("RGB", (600, 900), "blue").save(img)
        self.assertEqual(len(read_pages(img)), 1)
        batch = self.tmp / "lote"; batch.mkdir()
        (batch / "a.cbt").write_bytes(src.read_bytes()); (batch / "b.epub").write_bytes(data)
        self.assertEqual([p.name for p in expand_inputs([batch])], ["a.cbt", "b.epub"])

    def test_nickelmenu_lint_finds_real_problems(self):
        nm = self.root / ".adds/nm"
        nm.mkdir(parents=True)
        (nm / "a").write_text("menu_item :main :KOReader :cmd_spawn :quiet:exec /mnt/onboard/.adds/koreader/koreader.sh\n"
                              "menu_item :main :Invertir :nickel_setting :toggle:invert\n")
        (nm / "b").write_text("menu_item :main :koreader :power :reboot\n"
                              "menu_item :main :FTP :cmd_spawn :quiet:/usr/sbin/ftpd -w /mnt/onboard\n")
        titles = {i["title"] for i in lint_dir(nm)}
        self.assertTrue(any("no está instalado" in t for t in titles), titles)
        self.assertTrue(any("repetido" in t for t in titles), titles)
        self.assertTrue(any("sin contraseña" in t for t in titles), titles)
        self.assertTrue(any("reiniciar" in t for t in titles), titles)

    def test_own_pending_koboroot_is_merged(self):
        from kobo_aio.core import REGISTRY, Module, register
        engine.catalog()
        firmware_like = tgz({"./usr/local/Kobo/libnickel.so.1.0.0": b"patched"})

        def fw(mod, ctx, opts):
            ctx.add_firmware("kobopatch", firmware_like)
        if "test.fw" not in REGISTRY:
            register(Module(id="test.fw", category="Test", title="FW", description="", apply_fn=fw, reboot=True))
        dev = parse_version_file(self.root)
        engine.apply(dev, {"test.fw": {}}, offline=True)
        # a second run that adds another plugin must merge with our own pending package
        def plug(mod, ctx, opts):
            ctx.add_koboroot("plug", tgz({"./usr/local/Kobo/imageformats/libx.so": b"x"}))
        if "test.plug" not in REGISTRY:
            register(Module(id="test.plug", category="Test", title="P", description="", apply_fn=plug, reboot=True))
        engine.apply(dev, {"test.plug": {}}, offline=True)
        with tarfile.open(self.root / ".kobo/KoboRoot.tgz") as tf:
            names = sorted(m.name.lstrip("./") for m in tf.getmembers())
        self.assertEqual(names, ["usr/local/Kobo/imageformats/libx.so", "usr/local/Kobo/libnickel.so.1.0.0"])
        del REGISTRY["test.fw"], REGISTRY["test.plug"]


class ModelAwarenessTests(TempKobo):
    def test_all_models_have_complete_specs(self):
        from kobo_aio.device import KOBO_MODELS
        for mid, m in KOBO_MODELS.items():
            for k in ("name", "soc", "width", "height", "ppi", "color", "page_buttons", "bluetooth", "dark_mode"):
                self.assertIn(k, m, f"{mid} sin {k}")
            self.assertLess(m["width"], m["height"])

    def test_old_glo_hides_dependents_and_skips_them(self):
        shutil.rmtree(self.root)
        root = make_kobo(self.tmp / "KOBOeReader", model="330")
        (root / ".kobo/version").write_text("N000,4.9.77,4.1.15102,4.9.77,4.9.77,00000000-0000-0000-0000-000000000330\n")
        dev = parse_version_file(root)
        cat = {m["id"]: m for m in engine.catalog(dev)}
        self.assertFalse(cat["mod.nickelmenu"]["compatible"])
        self.assertFalse(cat["nm.menu"]["compatible"])
        self.assertIn("Necesita", cat["nm.menu"]["incompatible_reason"])
        res = engine.plan(dev, {"nm.menu": {}}, offline=True)
        self.assertTrue(all(r["action"] == "skip" for r in res["results"]))
        self.assertEqual([c for c in res["changes"] if c["rel"] != ".adds/kobo-aio/state.json"], [])

    def test_no_bluetooth_hides_audiobooks(self):
        shutil.rmtree(self.root)
        root = make_kobo(self.tmp / "KOBOeReader", model="376")
        cat = {m["id"]: m for m in engine.catalog(parse_version_file(root))}
        self.assertFalse(cat["lib.audiobooks"]["compatible"])
        self.assertTrue(cat["lib.comics"]["compatible"])

    def test_nickelhome_uninstall_creates_flag(self):
        from kobo_aio.core import REGISTRY
        engine.catalog()
        state = {"installed": {"mod.nickelhome": {}}}
        (self.root / ".adds/kobo-aio").mkdir(parents=True)
        (self.root / ".adds/kobo-aio/state.json").write_text(json.dumps(state))
        (self.root / ".adds/nickel-home").mkdir(parents=True)
        (self.root / ".adds/nickel-home/config").write_text("nhm_enabled:1\n")
        engine.apply(parse_version_file(self.root), {}, ["mod.nickelhome"], offline=True)
        self.assertTrue((self.root / ".adds/nickel-home/uninstall").exists())  # nh_info.uninstall_flag (REA)


if __name__ == "__main__":
    unittest.main()
