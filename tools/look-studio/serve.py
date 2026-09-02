#!/usr/bin/env python3
"""Run the Nowtube Look Studio from the repository root.

No third-party packages are required. The server root remains the repository
root so the preview can use the approved SPIFFS artwork.
"""

from __future__ import annotations

import argparse
import functools
import http.server
import threading
import webbrowser
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
LOOK_STUDIO_PATH = "/tools/look-studio/"


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the Nowtube Look Studio")
    parser.add_argument("--port", type=int, default=8765,
                        help="local port to use (default: 8765)")
    parser.add_argument("--host", default="127.0.0.1",
                        help="address to listen on (default: 127.0.0.1)")
    parser.add_argument("--no-browser", action="store_true",
                        help="do not open the default browser automatically")
    args = parser.parse_args()

    handler = functools.partial(http.server.SimpleHTTPRequestHandler,
                                directory=str(REPOSITORY_ROOT))
    server = http.server.ThreadingHTTPServer((args.host, args.port), handler)
    url = f"http://{args.host}:{args.port}{LOOK_STUDIO_PATH}"

    print(f"Nowtube Look Studio is running at:\n  {url}\n")
    print("Press Ctrl+C to stop it.")
    if not args.no_browser:
        threading.Timer(0.25, webbrowser.open, args=(url,)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nLook Studio stopped.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
