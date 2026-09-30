"""
Turn the source artwork into the two forms Windows and Qt each need:

  resources/ratcam.ico   multi-size, used by the .exe and the taskbar
  resources/ratcam.png   256px, loaded at run time for the window icon

Windows picks a different size depending on context -- 16px in the title bar,
32px in Alt-Tab, 48px in Explorer, 256px for large icons -- and scaling one
large bitmap down on the fly looks muddy at 16px. An .ico carries all of them.

Usage (campy env, from cpp/gui):
    python make_icon.py [source.png]
"""

import sys
from pathlib import Path

from PIL import Image

SIZES = [16, 24, 32, 48, 64, 128, 256]


def main():
    here = Path(__file__).parent
    res = here / "resources"
    res.mkdir(exist_ok=True)

    src = Path(sys.argv[1]) if len(sys.argv) > 1 else res / "ratcam.png"
    if not src.is_file():
        print("Source artwork not found: {}".format(src))
        print("Save the icon there (or pass a path) and run this again.")
        return 1

    img = Image.open(src).convert("RGBA")
    print("source: {}  {}x{}".format(src, img.width, img.height))

    # Square it off if needed; a non-square source would distort at 16px.
    if img.width != img.height:
        side = max(img.width, img.height)
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.paste(img, ((side - img.width) // 2, (side - img.height) // 2))
        img = square
        print("padded to square {}x{}".format(side, side))

    png = res / "ratcam.png"
    img.resize((256, 256), Image.LANCZOS).save(png)
    print("wrote {}".format(png))

    ico = res / "ratcam.ico"
    img.save(ico, format="ICO", sizes=[(s, s) for s in SIZES])
    print("wrote {}  ({})".format(ico, ", ".join(str(s) for s in SIZES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
