"""Option contracts (CHIP-0045) for the Chia reference wallet.

The reference wallet has no option wallet: it cannot mint an option, offer one
or exercise one, and its ``create_offer_for_ids`` knows nothing about them. The
Sage wallet can, and its implementation is a thin layer over the
``chia-wallet-sdk`` action system. This package ports that layer so XOPTrader can
do the same work against the wallet it already trades through:

* :mod:`builder` builds the unsigned coin spends -- mint, offer, transfer,
  exercise, cancel and post-expiry clawback -- with ``chia_wallet_sdk``.
* :mod:`signing` gets them signed. Every signature an option needs belongs to an
  ordinary wallet address, so the reference wallet's signer RPCs
  (``gather_signing_info`` / ``execute_signing_instructions``) can produce them.
  XOPTrader holds no key.
* :mod:`chain` reads an option back from the chain and applies the CHIP-0045
  validity checks, for a contract we minted before a restart or one somebody
  else is offering.

STATUS: library only. Nothing here is wired into the GUI or the engine, nothing
reads config, and no code path in XOPTrader calls it yet.

What has been checked, and what has not:

* Every spend path runs in ``tests/`` on the SDK's simulator, which runs each
  puzzle and checks conditions, timelocks and the aggregate signature. It does
  not model mempool fee or cost policy.
* The same paths were signed by chia-blockchain 2.7.5's own RPC handlers and
  ``WalletSigner`` (``tests/test_reference_wallet_real_signer.py``; skipped in
  CI, where chia-blockchain is not installed).
* The read-only parts were run by hand against mainnet on 2026-11-14:
  :func:`chain.load_option` loaded all 20 options then listed on
  xchoptions.com through warp's ``CoinsetClient`` and derived the terms the
  site shows, and :func:`signing.verify` accepted the signatures of four live
  offers, which have the same structure as the offers built here.
* NOT checked: a live wallet daemon over its HTTPS RPC, any broadcast to
  mainnet, xchoptions.com accepting an offer built here, and how the reference
  wallet reacts to the hinted option coin it does not recognise. Do those on a
  throwaway key with dust amounts before trusting this with funds.

``chia_wallet_sdk`` is imported inside functions, as warp does with its heavy
dependencies, so importing the GUI never fails when it is absent.
"""

__all__: list[str] = []
