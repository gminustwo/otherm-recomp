"""Draws the port's own emblem and makes the app icon, the menus' emblem and (on request) Steam artwork from it.

    python -I tools/make-app-icon.py [--steam-art <dir> <appid>]

The emblem is the port's own, drawn here from simple geometry: a ring broken into four arcs with four ticks pointing
in, around a hollow diamond with a solid one at its centre (a lock-on reticle around an energy core). It replaced the
developer's picture on 2026-11-24, a Nintendo trademark that must not ship with the port. Writes, under
runtime/assets:
  - icon/otherm_emblem.svg: the emblem's shape, black on transparent (the source of the pictures below);
  - icon/otherm_app_icon.svg and icon/otherm.png (512 px): the app icon, the emblem in cyan with an orange core on a
    rounded navy tile, as the menus colour things (hud_theme.cpp). The AppImage's icon (tools/package-linux.sh);
  - icon/otherm.ico: the same at 256-16 px, the Windows executable's icon (Explorer, the taskbar, the game window);
  - startup/emblem.png: the emblem's shape in white on transparent, 512 px, for the menus to tint
    (runtime/src/hud_theme.cpp). CMake copies startup/ beside the executable;
  - and android/res/mipmap-*: the Android app's launcher icon (an adaptive icon: a navy back layer, the emblem in
    front; the rounded tile for launchers without adaptive icons). --android writes only these.
--steam-art <dir> <appid> also writes Steam's artwork for a non-Steam shortcut into <dir> (Steam's
userdata/<user>/config/grid): <appid>.png (920x430), <appid>p.png (600x900) and <appid>_hero.png (1920x620), the
emblem on a navy backdrop with a cyan glow, no text.
Needs rsvg-convert (librsvg: SteamOS and most Linux desktops have it; MSYS2's mingw-w64-x86_64-librsvg on Windows).
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "runtime", "assets")
ICO_SIZES = [256, 64, 48, 32, 24, 16]
CX = CY = 256.0  # the emblem's own 512 x 512 space


def _pt(r, deg):
    return CX + r * math.cos(math.radians(deg)), CY + r * math.sin(math.radians(deg))


def _arc(a0, a1, ro, ri):
    x0, y0 = _pt(ro, a0)
    x1, y1 = _pt(ro, a1)
    x2, y2 = _pt(ri, a1)
    x3, y3 = _pt(ri, a0)
    return (f"M{x0:.2f},{y0:.2f} A{ro},{ro} 0 0 1 {x1:.2f},{y1:.2f} L{x2:.2f},{y2:.2f} "
            f"A{ri},{ri} 0 0 0 {x3:.2f},{y3:.2f} Z")


def _diamond(h):
    return f"M{CX:g},{CY - h:g} L{CX + h:g},{CY:g} L{CX:g},{CY + h:g} L{CX - h:g},{CY:g} Z"


def _tick(deg):
    # A small triangle on the ring's inner edge, pointing at the diamond's tip.
    bx, by = _pt(200, deg)
    tx, ty = _pt(170, deg)
    nx, ny = -math.sin(math.radians(deg)), math.cos(math.radians(deg))
    return f"M{bx + nx * 17:.2f},{by + ny * 17:.2f} L{tx:.2f},{ty:.2f} L{bx - nx * 17:.2f},{by - ny * 17:.2f} Z"


def emblem_paths():
    """The three parts as SVG path data: the ring with its ticks, the hollow diamond (even-odd), the core."""
    gap = 24  # degrees left open at each diagonal
    ring = " ".join(_arc(c + gap / 2, c + 90 - gap / 2, 238, 198) for c in (45, 135, 225, 315))
    ticks = " ".join(_tick(a) for a in (0, 90, 180, 270))
    return f"{ring} {ticks}", f"{_diamond(152)} {_diamond(106)}", _diamond(58)


def emblem_svg(fill):
    outer, frame, core = emblem_paths()
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512"><g fill="{fill}"><path d="{outer}"/>'
            f'<path fill-rule="evenodd" d="{frame}"/><path d="{core}"/></g></svg>\n')


GRADIENTS = """<linearGradient id="cyan" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="#b8f2ff"/><stop offset="1" stop-color="#36bfef"/></linearGradient>
<linearGradient id="orange" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="#ffc15a"/><stop offset="1" stop-color="#ff8a1e"/></linearGradient>"""


def coloured_emblem(x, y, size):
    """The emblem in the menus' colours, `size` px square with its top left at x, y (for an enclosing SVG)."""
    outer, frame, core = emblem_paths()
    s = size / 512.0
    return (f'<g transform="translate({x:g} {y:g}) scale({s:g})"><path fill="url(#cyan)" d="{outer}"/>'
            f'<path fill="url(#cyan)" fill-rule="evenodd" d="{frame}"/><path fill="url(#orange)" d="{core}"/></g>')


def app_icon_svg():
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512">
<defs>
<linearGradient id="tile" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#14304d"/><stop offset="1" stop-color="#081420"/></linearGradient>
<radialGradient id="glow" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="#3fd0ff" stop-opacity="0.35"/><stop offset="1" stop-color="#3fd0ff" stop-opacity="0"/></radialGradient>
{GRADIENTS}
</defs>
<rect x="8" y="8" width="496" height="496" rx="104" fill="url(#tile)"/>
<rect x="8" y="8" width="496" height="496" rx="104" fill="none" stroke="#3fd0ff" stroke-opacity="0.45" stroke-width="6"/>
<circle cx="256" cy="256" r="220" fill="url(#glow)"/>
{coloured_emblem(256 - 0.8 * 256, 256 - 0.8 * 256, 0.8 * 512)}
</svg>
"""


def backdrop_svg(width, height, emblem_size):
    """Steam artwork: a navy backdrop, a cyan glow and the emblem in the middle."""
    glow = emblem_size * 0.95
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
<defs>
<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#132a45"/><stop offset="1" stop-color="#070f1a"/></linearGradient>
<radialGradient id="glow" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="#3fd0ff" stop-opacity="0.30"/><stop offset="1" stop-color="#3fd0ff" stop-opacity="0"/></radialGradient>
{GRADIENTS}
</defs>
<rect width="{width}" height="{height}" fill="url(#bg)"/>
<circle cx="{width / 2:g}" cy="{height / 2:g}" r="{glow:g}" fill="url(#glow)"/>
{coloured_emblem(width / 2 - emblem_size / 2, height / 2 - emblem_size / 2, emblem_size)}
</svg>
"""


ANDROID_DENSITIES = [("mdpi", 1), ("hdpi", 1.5), ("xhdpi", 2), ("xxhdpi", 3), ("xxxhdpi", 4)]


def android_background_svg():
    """The Android adaptive icon's back layer: 108 dp, full bleed (the launcher cuts its own shape), navy and a glow."""
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 108 108">
<defs>
<linearGradient id="tile" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#14304d"/><stop offset="1" stop-color="#081420"/></linearGradient>
<radialGradient id="glow" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="#3fd0ff" stop-opacity="0.35"/><stop offset="1" stop-color="#3fd0ff" stop-opacity="0"/></radialGradient>
</defs>
<rect width="108" height="108" fill="url(#tile)"/>
<circle cx="54" cy="54" r="40" fill="url(#glow)"/>
</svg>
"""


def android_foreground_svg():
    """Its front layer: the emblem inside the 66 dp circle every launcher shape keeps."""
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 108 108">
<defs>
{GRADIENTS}
</defs>
{coloured_emblem(54 - 30, 54 - 30, 60)}
</svg>
"""


ANDROID_ADAPTIVE_XML = """<?xml version="1.0" encoding="utf-8"?>
<!-- tools/make-app-icon.py writes this and the pictures beside it. -->
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@mipmap/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
    <monochrome android:drawable="@mipmap/ic_launcher_foreground" />
</adaptive-icon>
"""


def write_android_icons():
    """android/res: the launcher icon, an adaptive icon (two layers) with the rounded tile as its fallback."""
    res = os.path.join(ROOT, "android", "res")
    for name, scale in ANDROID_DENSITIES:
        folder = os.path.join(res, f"mipmap-{name}")
        os.makedirs(folder, exist_ok=True)
        render(app_icon_svg(), int(48 * scale), int(48 * scale), os.path.join(folder, "ic_launcher.png"))
        layer = int(108 * scale)
        render(android_background_svg(), layer, layer, os.path.join(folder, "ic_launcher_background.png"))
        render(android_foreground_svg(), layer, layer, os.path.join(folder, "ic_launcher_foreground.png"))
    folder = os.path.join(res, "mipmap-anydpi-v26")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "ic_launcher.xml"), "w", newline="\n") as f:
        f.write(ANDROID_ADAPTIVE_XML)
    print("wrote android/res/mipmap-*/ic_launcher*.png (%s) and mipmap-anydpi-v26/ic_launcher.xml"
          % ", ".join(name for name, _ in ANDROID_DENSITIES))


def render(svg_text, width, height, out_path):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(svg_text)
        name = f.name
    try:
        subprocess.run(["rsvg-convert", "-w", str(width), "-h", str(height), name, "-o", out_path], check=True)
    finally:
        os.unlink(name)


def write_ico(pngs, out_path):
    """An .ico of PNG images (Windows Vista and later read these): pngs is [(size, bytes)]."""
    header = struct.pack("<HHH", 0, 1, len(pngs))
    offset = len(header) + 16 * len(pngs)
    entries, blobs = b"", b""
    for size, data in pngs:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    with open(out_path, "wb") as f:
        f.write(header + entries + blobs)


def main():
    args = sys.argv[1:]
    steam_dir = appid = None
    if args == ["--android"]:
        write_android_icons()
        return
    if args[:1] == ["--steam-art"] and len(args) == 3:
        steam_dir, appid = args[1], args[2]
    elif args:
        sys.exit(__doc__)
    icon_dir = os.path.join(ASSETS, "icon")
    startup_dir = os.path.join(ASSETS, "startup")
    os.makedirs(icon_dir, exist_ok=True)
    os.makedirs(startup_dir, exist_ok=True)

    with open(os.path.join(icon_dir, "otherm_emblem.svg"), "w") as f:
        f.write(emblem_svg("#000"))
    icon = app_icon_svg()
    with open(os.path.join(icon_dir, "otherm_app_icon.svg"), "w") as f:
        f.write(icon)
    render(icon, 512, 512, os.path.join(icon_dir, "otherm.png"))
    render(emblem_svg("#fff"), 512, 512, os.path.join(startup_dir, "emblem.png"))
    pngs = []
    with tempfile.TemporaryDirectory() as tmp:
        for size in ICO_SIZES:
            path = os.path.join(tmp, f"{size}.png")
            render(icon, size, size, path)
            with open(path, "rb") as f:
                pngs.append((size, f.read()))
    write_ico(pngs, os.path.join(icon_dir, "otherm.ico"))
    print("wrote icon/otherm_emblem.svg, otherm_app_icon.svg, otherm.png, otherm.ico (%s px), startup/emblem.png"
          % ", ".join(map(str, ICO_SIZES)))
    write_android_icons()

    if steam_dir:
        os.makedirs(steam_dir, exist_ok=True)
        for suffix, w, h, emblem in (("", 920, 430, 300), ("p", 600, 900, 380), ("_hero", 1920, 620, 420)):
            render(backdrop_svg(w, h, emblem), w, h, os.path.join(steam_dir, f"{appid}{suffix}.png"))
        print(f"wrote {appid}.png, {appid}p.png, {appid}_hero.png in {steam_dir}")


if __name__ == "__main__":
    main()
