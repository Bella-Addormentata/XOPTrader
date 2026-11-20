"""Circuit DAO (https://docs.circuitdao.com), read-only for now.

Issue #225 plans a Circuit page: a protocol monitor and keeper operations,
each behind its own switch, read-only first. This package starts it:

* :mod:`api` is a read-only client for Circuit's public API and ``probe``,
  which saves one answer from each read endpoint so the page can be built
  against real answers rather than guessed ones.

Nothing in the app imports this package yet. It reads no key, sends no
address and builds no transaction.
"""

__all__: list[str] = []
