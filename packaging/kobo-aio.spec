# PyInstaller spec for the Kobo AIO desktop app: one self-contained executable per OS, Python included.
#   pyinstaller packaging/kobo-aio.spec --distpath _app/dist --workpath _app/build
# Built for Windows, macOS (arm64 and x86_64) and Linux by .github/workflows/release.yml.
import os
from PyInstaller.utils.hooks import collect_submodules

ROOT = os.path.abspath(os.path.join(SPECPATH, ".."))

a = Analysis(
    [os.path.join(ROOT, "tools", "kobo_aio_app.py")],
    pathex=[os.path.join(ROOT, "tools")],
    datas=[(os.path.join(ROOT, "web", "index.html"), "web"), (os.path.join(ROOT, "web", "catalog.json"), "web")],
    hiddenimports=collect_submodules("kobo_aio") + ["fitz", "pymupdf"],
    excludes=["tkinter", "unittest", "pydoc_data"],
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz, a.scripts, a.binaries, a.datas,
    name="Kobo-AIO",
    console=True,          # the window shows what the engine is doing; closing it quits Kobo AIO
    upx=False,
    target_arch=None,
)
