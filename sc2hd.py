#!/usr/bin/env python3
"""Entry point of the Soulcalibur II HD launcher (see launcher/sc2hd_launcher/cli.py).

    python3 sc2hd.py --pkg <game.zip|game.pkg> [--pkg <update.pkg>]   first run (the 01.01 update is optional)
    python3 sc2hd.py                                                  later runs

Kept free of newer syntax so that an old interpreter reaches the version check below.
"""
import os
import sys

MIN_PYTHON = (3, 10)

if sys.version_info < MIN_PYTHON:
    sys.exit("sc2hd: Python %d.%d or newer is needed (this is %d.%d); macOS: brew install python, Windows: python.org"
             % (MIN_PYTHON + sys.version_info[:2]))

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "launcher"))

from sc2hd_launcher.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
