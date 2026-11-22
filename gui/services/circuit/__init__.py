"""Circuit DAO (https://docs.circuitdao.com), read-only for now.

Issue #225 plans a Circuit page: a protocol monitor and keeper operations,
each behind its own switch, read-only first. This package starts it:

* :mod:`api` is a read-only client for Circuit's public API and ``probe``,
  which saves one answer from each read endpoint so the page can be built
  against real answers rather than guessed ones.
* :mod:`poll` is the page's eight reads and the loop that makes them.
* :mod:`model` turns their answers into what the page shows.

The page is :mod:`gui.widgets.circuit`, on by default for now (Settings >
Advanced > Subsystems switches it off). Nothing here reads a key, sends an
address or builds a transaction.
"""

__all__: list[str] = []
