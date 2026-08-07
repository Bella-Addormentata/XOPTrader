"""XOPTrader warp.green bridge automation (Tier 2).

Fully-background bridging of USDC (Base) -> wUSDC.b (Chia CAT, the bot's primary
quote asset). The operator funds an app-controlled Base hot wallet from anywhere;
XOPTrader detects the deposit, performs the on-chain bridge, collects the
validator BLS attestations from Nostr, and pushes the Chia claim spend so the
wrapped CAT lands in the bot's Chia wallet -- no step-by-step instructions.

Everything in this package is Python/GUI-side. The C++ engine, its SQLite
database, and its build are untouched.

Heavy third-party imports (``clvm``, ``chia_rs``, ``web3``, ``websocket``) live
inside functions and submodules rather than at package import time, so importing
the top-level GUI never fails when warp is disabled or the optional bridge
dependencies are absent.
"""

__all__: list[str] = []
