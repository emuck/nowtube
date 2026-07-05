#!/usr/bin/env python3
"""Upload release SPIFFS assets to a nowtube device over Wi-Fi.

Uploads the web UI and image assets from main/spiffs/ using the firmware's
POST /api/spiffs/upload?name=<filename> endpoint.
"""

from __future__ import annotations

import argparse
import mimetypes
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
SPIFFS_DIR = REPO_ROOT / "main" / "spiffs"
ALLOWED_SUFFIXES = {".png", ".html", ".js", ".css", ".z3"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Upload SPIFFS assets from main/spiffs to a nowtube device."
    )
    parser.add_argument(
        "device",
        help="Device host or IP, e.g. 192.168.88.25 or nowtube.local",
    )
    parser.add_argument(
        "--dir",
        default=str(SPIFFS_DIR),
        help=f"Asset directory to upload (default: {SPIFFS_DIR})",
    )
    parser.add_argument(
        "--only",
        nargs="+",
        help="Upload only these filenames from the asset directory",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the files that would be uploaded without sending them",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=10.0,
        help="HTTP timeout in seconds (default: 10)",
    )
    return parser.parse_args()


def asset_files(asset_dir: Path, only: list[str] | None) -> list[Path]:
    if only:
        files = [asset_dir / name for name in only]
    else:
        files = sorted(
            path for path in asset_dir.iterdir()
            if path.is_file() and path.suffix in ALLOWED_SUFFIXES
        )

    missing = [path.name for path in files if not path.is_file()]
    if missing:
        raise FileNotFoundError(f"Missing asset files: {', '.join(missing)}")

    disallowed = [path.name for path in files if path.suffix not in ALLOWED_SUFFIXES]
    if disallowed:
        raise ValueError(
            "Unsupported asset types requested: " + ", ".join(disallowed)
        )

    too_long = [path.name for path in files if len(path.name) > 32]
    if too_long:
        raise ValueError(
            "Device upload endpoint rejects filenames longer than 32 chars: "
            + ", ".join(too_long)
        )

    return files


def content_type_for(path: Path) -> str:
    guessed, _ = mimetypes.guess_type(path.name)
    return guessed or "application/octet-stream"


def upload_one(base_url: str, path: Path, timeout: float) -> None:
    target = f"{base_url}/api/spiffs/upload?name={urllib.parse.quote(path.name)}"
    data = path.read_bytes()
    req = urllib.request.Request(
        target,
        data=data,
        method="POST",
        headers={"Content-Type": content_type_for(path)},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8", errors="replace").strip()
        if resp.status != 200:
            raise RuntimeError(f"{path.name}: HTTP {resp.status}: {body}")
        if '"status":"ok"' not in body and '"status": "ok"' not in body:
            raise RuntimeError(f"{path.name}: unexpected response body: {body}")


def main() -> int:
    args = parse_args()
    asset_dir = Path(args.dir).resolve()
    base_url = args.device
    if "://" not in base_url:
        base_url = "http://" + base_url
    base_url = base_url.rstrip("/")

    try:
        files = asset_files(asset_dir, args.only)
    except (FileNotFoundError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if not files:
        print("No assets selected.", file=sys.stderr)
        return 2

    print(f"Uploading {len(files)} SPIFFS assets to {base_url}")
    for path in files:
        rel = path.relative_to(asset_dir)
        if args.dry_run:
            print(f"DRY RUN  {rel}")
            continue
        print(f"UPLOAD   {rel}")
        try:
            upload_one(base_url, path, args.timeout)
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", errors="replace").strip()
            print(f"FAILED   {rel}  HTTP {exc.code}: {body}", file=sys.stderr)
            return 1
        except urllib.error.URLError as exc:
            print(f"FAILED   {rel}  {exc.reason}", file=sys.stderr)
            return 1
        except Exception as exc:  # pragma: no cover - defensive CLI fallback
            print(f"FAILED   {rel}  {exc}", file=sys.stderr)
            return 1
        print(f"OK       {rel}")

    if args.dry_run:
        print("Dry run complete.")
    else:
        print("All SPIFFS assets uploaded successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
