"""Launch the GUI from source. sys.path fix: a script launch puts the
script's own directory first, not the repo root, so `gui` is unimportable
without this -- which is why pythonw kept dying silently on 2026-08-31."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from gui.main import main

main()
