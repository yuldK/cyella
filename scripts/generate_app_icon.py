"""Generate Cyella's Windows icon (assets/cyella.ico) and caption icon
(assets/cyella-caption.png) from assets/cyella.svg.

Pillow cannot read SVG, so the artwork is rendered once at its native 1024 px by
headless Microsoft Edge (bundled with Windows 11) with a transparent background,
then downscaled to each icon size. The caption icon is one PNG that the app
decodes at startup; luil's Skia build has no SVG module, so the caption cannot
draw the SVG itself. Run from the repository root (Python 3.9 or newer with
Pillow):

    py -I .\\scripts\\generate_app_icon.py --svg .\\assets\\cyella.svg --output .\\assets\\cyella.ico --caption-output .\\assets\\cyella-caption.png
"""

from __future__ import annotations

import argparse
import io
import struct
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image


ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
RENDER_SIZE = 1024
# The caption draws the icon at 16 logical px, so 64 px covers up to 400% scaling.
CAPTION_SIZE = 64
DEFAULT_BROWSER = Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe")
SCREENSHOT_TIMEOUT_SECONDS = 60


def render_svg(svg_path: Path, browser_path: Path, work_directory: Path) -> Image.Image:
    screenshot_path = work_directory / "render.png"
    subprocess.run(
        [
            str(browser_path),
            "--headless=new",
            "--disable-gpu",
            "--hide-scrollbars",
            "--no-first-run",
            f"--user-data-dir={work_directory / 'profile'}",
            "--force-device-scale-factor=1",
            f"--window-size={RENDER_SIZE},{RENDER_SIZE}",
            "--default-background-color=00000000",
            f"--screenshot={screenshot_path}",
            svg_path.resolve().as_uri(),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # The Edge launcher can return before the headless child writes the file.
    deadline = time.monotonic() + SCREENSHOT_TIMEOUT_SECONDS
    while True:
        try:
            with Image.open(screenshot_path) as image:
                image.load()
                rendered = image.convert("RGBA")
            break
        except (FileNotFoundError, OSError):
            if time.monotonic() > deadline:
                raise RuntimeError("Edge did not produce the screenshot in time.")
            time.sleep(0.2)

    if rendered.size != (RENDER_SIZE, RENDER_SIZE):
        raise RuntimeError(f"Unexpected render size: {rendered.size}")
    # An opaque corner means the browser ignored the transparent background flag.
    if rendered.getpixel((0, 0))[3] != 0:
        raise RuntimeError("The rendered background is not transparent.")
    return rendered


def write_ico(images: list[Image.Image], output_path: Path) -> None:
    png_data: list[bytes] = []
    for image in images:
        buffer = io.BytesIO()
        image.save(buffer, format="PNG", optimize=True)
        png_data.append(buffer.getvalue())

    header_size = 6
    directory_size = 16 * len(images)
    offset = header_size + directory_size
    directory = bytearray()
    for image, data in zip(images, png_data):
        width = image.width if image.width < 256 else 0
        height = image.height if image.height < 256 else 0
        directory.extend(struct.pack("<BBBBHHII", width, height, 0, 0, 1, 32, len(data), offset))
        offset += len(data)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(struct.pack("<HHH", 0, 1, len(images)) + directory + b"".join(png_data))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--svg", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--caption-output", type=Path)
    parser.add_argument("--browser", type=Path, default=DEFAULT_BROWSER)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="cyella-icon-") as work_directory:
        source = render_svg(arguments.svg, arguments.browser, Path(work_directory))
    images = [source.resize((size, size), Image.Resampling.LANCZOS) for size in ICON_SIZES]
    write_ico(images, arguments.output)
    if arguments.caption_output is not None:
        arguments.caption_output.parent.mkdir(parents=True, exist_ok=True)
        caption = source.resize((CAPTION_SIZE, CAPTION_SIZE), Image.Resampling.LANCZOS)
        caption.save(arguments.caption_output, format="PNG", optimize=True)


if __name__ == "__main__":
    main()
