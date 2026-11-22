"""``python -m gui.services.circuit probe`` -- see :mod:`gui.services.circuit.api`.

This form runs in the app's environment: Python loads ``gui.services`` first,
which needs Qt and PyYAML. ``python api.py probe`` needs only Python.
"""

from __future__ import annotations

import sys

from gui.services.circuit.api import main

if __name__ == "__main__":
    sys.exit(main())
