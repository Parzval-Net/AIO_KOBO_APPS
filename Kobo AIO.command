#!/bin/sh
# Doble clic en macOS para abrir Kobo AIO en el navegador.
cd "$(dirname "$0")/tools" || exit 1
if ! command -v python3 >/dev/null 2>&1; then
  echo "Necesitas Python 3: instala las Command Line Tools con 'xcode-select --install' o desde python.org"
  read -r _; exit 1
fi
python3 -c "import PIL" 2>/dev/null || python3 -m pip install --user --quiet Pillow
exec python3 -m kobo_aio serve "$@"
