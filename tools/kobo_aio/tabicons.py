"""Icons for the NickelMenu tab in the bottom bar (firmware 4.23.15505+).

Matches Nickel's own tab icons, read from the 4.45.23697 firmware (Qt resources in /usr/local/Kobo/nickel,
:/images/home/main_nav_*.png): 50x50 PNG, grayscale + alpha, pure black ink; a ~3 px outline when idle and a
filled shape when the tab is active (main_nav_home.png / main_nav_home_active.png). NickelMenu loads them with
QPixmap from experimental :menu_main_15505_icon / _icon_active (NickelMenu res/doc).

Each icon is one list of primitives in a 50x50 box, rendered both as PNG (Pillow, for the Kobo) and as SVG
(for the web preview), so both always look the same.
  role "shape":  outlined when idle, filled when active
  role "detail": drawn line when idle, cut out of the filled shape when active
  role "knob":   hollow ring when idle (hides what is under it), solid when active
"""

from __future__ import annotations

import io
import math

SIZE = 50
STROKE = 3.2


def _star(cx=25, cy=26.5, r1=20, r2=8.5, n=5):
    return [(cx + (r1 if i % 2 == 0 else r2) * math.sin(math.pi * i / n),
             cy - (r1 if i % 2 == 0 else r2) * math.cos(math.pi * i / n)) for i in range(2 * n)]


def _heart():
    pts = []
    for i in range(48):
        t = 2 * math.pi * i / 48
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((25 + x * 1.22, 24 - y * 1.22))
    return pts


ICONS: dict[str, tuple[str, list]] = {
    "aio": ("Lector (Kobo AIO)", [
        ("rrect", (13, 4, 24, 42, 4.5), "shape"),
        ("line", (18.5, 13.5, 31.5, 13.5), "detail"), ("line", (18.5, 20, 31.5, 20), "detail"),
        ("line", (18.5, 26.5, 27, 26.5), "detail")]),
    "star": ("Estrella", [("poly", _star(), "shape")]),
    "bolt": ("Rayo", [("poly", [(29, 3.5), (11, 28), (23, 28), (19.5, 46.5), (39, 20), (27, 20), (31, 3.5)], "shape")]),
    "grid": ("Cuadrícula", [("rrect", (6, 6, 16, 16, 3.5), "shape"), ("rrect", (28, 6, 16, 16, 3.5), "shape"),
                            ("rrect", (6, 28, 16, 16, 3.5), "shape"), ("rrect", (28, 28, 16, 16, 3.5), "shape")]),
    "heart": ("Corazón", [("poly", _heart(), "shape")]),
    "bookmark": ("Marcador", [("poly", [(13, 4.5), (37, 4.5), (37, 46), (25, 36), (13, 46)], "shape")]),
    "sliders": ("Ajustes", [
        ("line", (6, 13, 44, 13), "shape"), ("line", (6, 25, 44, 25), "shape"), ("line", (6, 37, 44, 37), "shape"),
        ("circle", (31, 13, 5), "knob"), ("circle", (17, 25, 5), "knob"), ("circle", (34, 37, 5), "knob")]),
    "moon": ("Luna", [("crescent", (25, 25, 18.5, 33.5, 17, 15.5), "shape")]),
}
DEFAULT = "aio"


# -- PNG (Pillow) -----------------------------------------------------------------------------------------
def _draw(icon: str, active: bool, scale: int = 8):
    from PIL import Image, ImageDraw
    s = scale
    mask = Image.new("L", (SIZE * s, SIZE * s), 0)
    d = ImageDraw.Draw(mask)
    w = round(STROKE * s * (1.3 if active else 1))

    def line(x1, y1, x2, y2, fill, width):
        d.line([(x1 * s, y1 * s), (x2 * s, y2 * s)], fill=fill, width=width)
        for x, y in ((x1, y1), (x2, y2)):   # round caps
            r = width / 2
            d.ellipse([x * s - r, y * s - r, x * s + r, y * s + r], fill=fill)

    for kind, g, role in ICONS[icon][1]:
        cut = active and role == "detail"
        if kind == "line":
            line(*g, fill=0 if cut else 255, width=round(STROKE * s * (1.15 if cut else 1.3 if active else 1)))
        elif kind == "rrect":
            x, y, ww, hh, r = g
            box = [x * s, y * s, (x + ww) * s, (y + hh) * s]
            if active:
                d.rounded_rectangle(box, r * s, fill=255)
            else:
                d.rounded_rectangle(box, r * s, outline=255, width=w)
        elif kind == "poly":
            pts = [(x * s, y * s) for x, y in g]
            if active:
                d.polygon(pts, fill=255)
            else:
                d.line(pts + pts[:2], fill=255, width=w, joint="curve")
        elif kind == "circle":
            cx, cy, r = g
            box = [(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s]
            if active:
                d.ellipse(box, fill=255)
            else:
                d.ellipse(box, fill=0)
                d.ellipse(box, outline=255, width=round(STROKE * s))
        elif kind == "crescent":   # outer disk minus an offset disk
            ox, oy, orr, cx, cy, cr = g
            outer = [(ox - orr) * s, (oy - orr) * s, (ox + orr) * s, (oy + orr) * s]
            cutter = [(cx - cr) * s, (cy - cr) * s, (cx + cr) * s, (cy + cr) * s]
            if active:
                d.ellipse(outer, fill=255)
                d.ellipse(cutter, fill=0)
            else:
                from PIL import ImageChops
                d.ellipse(outer, outline=255, width=w)
                d.ellipse(cutter, fill=0)
                ring = Image.new("L", mask.size, 0)
                ImageDraw.Draw(ring).ellipse(cutter, outline=255, width=w)
                disk = Image.new("L", mask.size, 0)
                ImageDraw.Draw(disk).ellipse(outer, fill=255)
                mask.paste(ImageChops.lighter(mask, ImageChops.multiply(ring, disk)))
                d = ImageDraw.Draw(mask)
    return mask.resize((SIZE, SIZE), Image.LANCZOS)


def _la_png(alpha) -> bytes:
    from PIL import Image
    img = Image.merge("LA", (Image.new("L", alpha.size, 0), alpha))
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def render_png(icon: str, active: bool = False) -> bytes:
    if icon not in ICONS:
        raise KeyError(icon)
    return _la_png(_draw(icon, active))


def from_image(data: bytes) -> tuple[bytes, bytes]:
    """A user's picture (PNG/JPG/WebP) as a tab icon: black silhouette, 50x50, same look idle and active.

    Transparent images keep their shape; opaque ones use darkness as ink (logos on white work well).
    """
    from PIL import Image, ImageChops, ImageOps
    img = ImageOps.exif_transpose(Image.open(io.BytesIO(data)))
    if img.mode in ("RGBA", "LA", "PA") or (img.mode == "P" and "transparency" in img.info):
        rgba = img.convert("RGBA")
        alpha = ImageChops.multiply(rgba.getchannel("A"), ImageOps.invert(rgba.convert("L")).point(lambda v: 255 if v > 40 else v * 6))
    else:
        alpha = ImageOps.autocontrast(ImageOps.invert(img.convert("L")))
    bbox = alpha.getbbox() or (0, 0, *alpha.size)
    alpha = alpha.crop(bbox)
    alpha.thumbnail((44, 44), Image.LANCZOS)
    canvas = Image.new("L", (SIZE, SIZE), 0)
    canvas.paste(alpha, ((SIZE - alpha.width) // 2, (SIZE - alpha.height) // 2))
    png = _la_png(canvas)
    return png, png


# -- SVG (web preview) ------------------------------------------------------------------------------------
def svg(icon: str, active: bool = False) -> str:
    f = lambda v: f"{v:.2f}".rstrip("0").rstrip(".")  # noqa: E731
    w = f(STROKE * (1.3 if active else 1))
    body, holes, extra = [], [], []
    for kind, g, role in ICONS[icon][1]:
        cut = active and role == "detail"
        if kind == "line":
            el = (f'<line x1="{f(g[0])}" y1="{f(g[1])}" x2="{f(g[2])}" y2="{f(g[3])}" stroke-width="'
                  f'{f(STROKE * (1.15 if cut else 1.3 if active else 1))}" stroke-linecap="round"')
            (holes if cut else body).append(el + (' stroke="#000"/>' if cut else ' stroke="currentColor"/>'))
        elif kind == "rrect":
            x, y, ww, hh, r = g
            attrs = f'x="{f(x)}" y="{f(y)}" width="{f(ww)}" height="{f(hh)}" rx="{f(r)}"'
            body.append(f'<rect {attrs} fill="currentColor"/>' if active else
                        f'<rect {attrs} fill="none" stroke="currentColor" stroke-width="{w}"/>')
        elif kind == "poly":
            pts = " ".join(f"{f(x)},{f(y)}" for x, y in g)
            body.append(f'<polygon points="{pts}" fill="currentColor"/>' if active else
                        f'<polygon points="{pts}" fill="none" stroke="currentColor" stroke-width="{w}" stroke-linejoin="round"/>')
        elif kind == "circle":
            cx, cy, r = g
            if active:
                body.append(f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}" fill="currentColor"/>')
            else:
                holes.append(f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}" fill="#000"/>')
                body.append(f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}" fill="none" stroke="currentColor" stroke-width="{f(STROKE)}"/>')
        elif kind == "crescent":
            ox, oy, orr, cx, cy, cr = g
            holes.append(f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(cr)}" fill="#000"/>')
            if active:
                body.append(f'<circle cx="{f(ox)}" cy="{f(oy)}" r="{f(orr)}" fill="currentColor"/>')
            else:
                body.append(f'<circle cx="{f(ox)}" cy="{f(oy)}" r="{f(orr)}" fill="none" stroke="currentColor" stroke-width="{w}"/>')
                clip = f"c-{icon}"
                extra.append(f'<clipPath id="{clip}"><circle cx="{f(ox)}" cy="{f(oy)}" r="{f(orr)}"/></clipPath>'
                             f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(cr)}" fill="none" stroke="currentColor" stroke-width="{w}" clip-path="url(#{clip})"/>')
    rings = [b for b in body if b.startswith("<circle") and 'fill="none"' in b]
    rest = [b for b in body if b not in rings]
    mid = f"m-{icon}-{int(active)}"
    mask = (f'<mask id="{mid}"><rect width="50" height="50" fill="#fff"/>{"".join(holes)}</mask>' if holes else "")
    masked = f' mask="url(#{mid})"' if holes else ""
    group = f'<g{masked}>{"".join(rest)}</g>{"".join(rings)}{"".join(extra)}'
    return f'<svg viewBox="0 0 50 50" width="50" height="50" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">{mask}{group}</svg>'


def choices() -> list[tuple[str, str]]:
    return [(k, v[0]) for k, v in ICONS.items()]


def previews() -> dict[str, list[str]]:
    return {k: [svg(k, False), svg(k, True)] for k in ICONS}
