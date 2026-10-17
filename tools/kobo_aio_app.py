"""Entry point of the Kobo AIO desktop app (PyInstaller): with no arguments it opens the web UI."""

import sys

from kobo_aio.__main__ import main

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["serve"]))
