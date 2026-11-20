"""``python -m gui.services.circuit probe`` -- see :mod:`gui.services.circuit.api`."""

from __future__ import annotations

import sys

from gui.services.circuit.api import main

if __name__ == "__main__":
    sys.exit(main())
