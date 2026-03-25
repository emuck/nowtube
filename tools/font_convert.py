#!/usr/bin/env python3
"""
nowtube font converter
Fetches a Google Font by name, converts it to LVGL .c format,
and drops it into main/fonts/ ready to use.

Usage:
  python3 tools/font_convert.py "DSEG7 Classic" --sizes 40 60 100 120
  python3 tools/font_convert.py "Share Tech Mono" --sizes 100 --charset clock
  python3 tools/font_convert.py "Orbitron" --sizes 80 --charset full

Requirements:
  pip3 install requests
  npm install -g lv_font_conv
"""

import argparse
import io
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

# ---------------------------------------------------------------------------
# Character sets — keep font files small by only including what we need
# ---------------------------------------------------------------------------
CHARSETS = {
    # Clock-only: digits, colon, period, space, degree symbol
    "clock": "0x20,0x2E,0x30-0x39,0x3A,0xB0",
    # Clock + basic punctuation + AM/PM letters
    "clock+": "0x20,0x2E,0x30-0x39,0x3A,0x41,0x4D,0x50,0xB0",
    # Full printable ASCII (A-Z, a-z, 0-9, punctuation) — needed for scrolling text
    "full": "0x20-0x7E",
    # Uppercase + digits only (good middle ground for status displays)
    "upper": "0x20-0x40,0x41-0x5A,0x5B-0x60",
}

CHARSET_DESCRIPTIONS = {
    "clock":  "digits, colon, period, space, degree (smallest — ~5KB)",
    "clock+": "clock + AM/PM letters (for time display with suffix)",
    "full":   "all printable ASCII A-Z a-z 0-9 punct (largest — ~50KB+)",
    "upper":  "uppercase + digits + punctuation (good for status text)",
}

REPO_ROOT = Path(__file__).parent.parent
FONTS_DIR = REPO_ROOT / "main" / "fonts"
ARTWORK_DIR = REPO_ROOT / "fonts-src"


def check_lv_font_conv():
    """Verify lv_font_conv is installed."""
    if shutil.which("lv_font_conv") is None:
        print("Error: lv_font_conv not found.")
        print("Install it with:  npm install -g lv_font_conv")
        sys.exit(1)


def google_font_id(name: str) -> str:
    """Convert 'DSEG7 Classic' → 'dseg7-classic' for API URLs."""
    return name.lower().replace(" ", "-")


def download_google_font(font_name: str) -> Path:
    """
    Download a Google Font ZIP and extract TTF files.
    Returns the directory containing the extracted TTF files.
    """
    font_id = google_font_id(font_name)
    # Google Fonts direct download URL
    url = f"https://fonts.google.com/download?family={font_name.replace(' ', '+')}"

    dest_dir = ARTWORK_DIR / font_name.replace(" ", "_")
    dest_dir.mkdir(parents=True, exist_ok=True)

    # Check if already downloaded
    existing_ttfs = list(dest_dir.glob("**/*.ttf"))
    if existing_ttfs:
        print(f"  Font already downloaded: {dest_dir}")
        return dest_dir

    print(f"  Downloading {font_name} from Google Fonts...")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=30) as response:
            zip_data = response.read()
    except Exception as e:
        print(f"  Error downloading font: {e}")
        print(f"  Try downloading manually from https://fonts.google.com/specimen/{font_name.replace(' ', '+')}")
        print(f"  and place the TTF files in: {dest_dir}")
        sys.exit(1)

    # Extract ZIP
    with zipfile.ZipFile(io.BytesIO(zip_data)) as zf:
        for name in zf.namelist():
            if name.endswith(".ttf") or name.endswith(".otf"):
                data = zf.read(name)
                out_path = dest_dir / Path(name).name
                out_path.write_bytes(data)
                print(f"  Extracted: {out_path.name}")

    ttfs = list(dest_dir.glob("**/*.ttf")) + list(dest_dir.glob("**/*.otf"))
    if not ttfs:
        print(f"  No TTF/OTF files found in download.")
        sys.exit(1)

    return dest_dir


def pick_ttf(font_dir: Path, style: str = "Regular") -> Path:
    """Pick the best TTF from a font directory for a given style."""
    candidates = list(font_dir.glob(f"**/*{style}*.ttf"))
    if not candidates:
        candidates = list(font_dir.glob("**/*.ttf"))
    if not candidates:
        candidates = list(font_dir.glob("**/*.otf"))
    if not candidates:
        print(f"  No font files found in {font_dir}")
        sys.exit(1)

    # Prefer Regular, avoid Italic/Bold unless that's what was asked for
    non_italic = [f for f in candidates if "Italic" not in f.name]
    if non_italic:
        candidates = non_italic

    candidates.sort(key=lambda f: (
        0 if "Regular" in f.name else
        1 if "Medium" in f.name else
        2 if "Light" in f.name else 3
    ))
    return candidates[0]


def c_safe_name(font_name: str, size: int) -> str:
    """'DSEG7 Classic' + 120 → 'dseg7_classic_120'"""
    safe = re.sub(r"[^a-zA-Z0-9]", "_", font_name).lower()
    safe = re.sub(r"_+", "_", safe).strip("_")
    return f"{safe}_{size}"


def convert_font(ttf_path: Path, size: int, charset: str, output_name: str) -> Path:
    """Run lv_font_conv and return the output .c file path."""
    output_path = FONTS_DIR / f"{output_name}.c"

    cmd = [
        "lv_font_conv",
        "--font", str(ttf_path),
        "--range", charset,
        "--size", str(size),
        "--format", "lvgl",
        "--bpp", "4",          # 4-bit antialiasing — matches existing Oswald fonts
        "-o", str(output_path),
    ]

    print(f"  Converting {ttf_path.name} at {size}px → {output_path.name}")
    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode != 0:
        print(f"  lv_font_conv error:\n{result.stderr}")
        sys.exit(1)

    size_kb = output_path.stat().st_size / 1024
    print(f"  Done: {output_path.name} ({size_kb:.1f} KB)")
    return output_path


def print_integration_instructions(font_name: str, sizes: list[int]):
    """Print the C++ snippets the user needs to add to the project."""
    names = [c_safe_name(font_name, s) for s in sizes]

    print()
    print("=" * 60)
    print("Integration steps:")
    print("=" * 60)
    print()
    print("1. Add to main/CMakeLists.txt (in idf_component_register SRCS):")
    for name in names:
        print(f'        "fonts/{name}.c"')

    print()
    print("2. Add to main/gui.h:")
    for name in names:
        print(f"   LV_FONT_DECLARE({name})")

    print()
    print("3. Use in your code:")
    for name, size in zip(names, sizes):
        print(f"   lv_obj_set_style_text_font(obj, &{name}, LV_PART_MAIN);  // {size}px")

    print()
    print("4. Rebuild:")
    print("   idf.py build")


def main():
    parser = argparse.ArgumentParser(
        description="Convert a Google Font to LVGL format for nowtube",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s "DSEG7 Classic" --sizes 60 100 120
  %(prog)s "Share Tech Mono" --sizes 100 --charset clock
  %(prog)s "Orbitron" --sizes 80 120 --charset full
  %(prog)s "Segment7" --sizes 120 --ttf /path/to/font.ttf

Character sets:
""" + "\n".join(f"  {k:<10} {v}" for k, v in CHARSET_DESCRIPTIONS.items())
    )

    parser.add_argument("font_name",
                        help="Google Font name, e.g. 'DSEG7 Classic'")
    parser.add_argument("--sizes", nargs="+", type=int, default=[40, 60, 100, 120],
                        help="Point sizes to generate (default: 40 60 100 120)")
    parser.add_argument("--charset", choices=CHARSETS.keys(), default="clock",
                        help="Character set to include (default: clock)")
    parser.add_argument("--style", default="Regular",
                        help="Font style to prefer, e.g. Regular, Bold (default: Regular)")
    parser.add_argument("--ttf", type=Path, default=None,
                        help="Use a local TTF file instead of downloading")
    parser.add_argument("--list-fonts", action="store_true",
                        help="Print some recommended fonts and exit")

    args = parser.parse_args()

    if args.list_fonts:
        print("""
Recommended fonts for Nextube:

  DSEG7 Classic      — Realistic 7-segment LED display. Best for clock digits.
                       https://fonts.google.com/specimen/DSEG7+Classic
  Share Tech Mono    — Clean monospace, terminal feel.
  Orbitron           — Geometric, futuristic. Similar to NixieOY theme.
  Nova Square        — Retro digital, square segments.
  Segment7           — Another 7-segment style.
  Aldrich            — Clean, modern, readable at small sizes.

Already in project:
  Oswald             — Current default (40/60/100/120px)
""")
        return

    check_lv_font_conv()
    FONTS_DIR.mkdir(parents=True, exist_ok=True)

    charset_range = CHARSETS[args.charset]
    print(f"\nFont:     {args.font_name}")
    print(f"Sizes:    {args.sizes}")
    print(f"Charset:  {args.charset} ({CHARSET_DESCRIPTIONS[args.charset]})")
    print()

    if args.ttf:
        ttf_path = args.ttf
        if not ttf_path.exists():
            print(f"Error: {ttf_path} not found")
            sys.exit(1)
        print(f"  Using local TTF: {ttf_path}")
    else:
        font_dir = download_google_font(args.font_name)
        ttf_path = pick_ttf(font_dir, args.style)
        print(f"  Using: {ttf_path.name}")

    print()
    generated = []
    for size in args.sizes:
        name = c_safe_name(args.font_name, size)
        out = convert_font(ttf_path, size, charset_range, name)
        generated.append((name, size, out))

    print_integration_instructions(args.font_name, args.sizes)


if __name__ == "__main__":
    main()
