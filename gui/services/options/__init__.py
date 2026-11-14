"""Option contracts (CHIP-0045) for the Chia reference wallet.

The reference wallet has no option wallet: it cannot mint an option, offer one
or exercise one, and its ``create_offer_for_ids`` knows nothing about them. The
Sage wallet can, and its implementation is a thin layer over the
``chia-wallet-sdk`` action system. This package ports that layer so XOPTrader can
do the same work against the wallet it already trades through:

* :mod:`builder` builds the unsigned coin spends -- mint, offer, transfer,
  exercise, cancel and post-expiry clawback -- with ``chia_wallet_sdk``.
* :mod:`signing` and :mod:`reference_wallet` get them signed. Every signature an
  option needs belongs to an ordinary wallet address, so the reference wallet's
  signer RPCs (``gather_signing_info`` / ``execute_signing_instructions``) can
  produce them. XOPTrader holds no key.
* :mod:`safety` works out what a bundle does to our money before it is signed
  and refuses one that differs from what the operation declared.
* :mod:`chain` reads an option, or the outcome of a broadcast, back from chain.
* :mod:`pricing`, :mod:`market` and :mod:`quoting` decide what to offer and at
  what price; :mod:`desk` talks to xchoptions.com; :mod:`store` is the durable
  record; :mod:`service` is the loop that ties them together and
  :mod:`monitor` is its read-only counterpart for other makers' offers.
* :mod:`runtime` and ``python -m gui.services.options`` wire it to a real
  wallet and run it from a terminal.

STATUS: runs from the command line against a DEDICATED reference wallet. There
is no GUI page, and ``wallet.mode: trading`` is refused: the engine does not yet
keep option coins out of its own offers or book option events, and nothing in
the engine reads this package's ``events`` table. It is off unless config.yaml
has ``options.enabled: true``, and it rehearses (``dry_run``) by default.

What has been checked, and what has not:

* Every spend path runs in ``tests/`` on the SDK's simulator, which runs each
  puzzle and checks conditions, timelocks and the aggregate signature. It does
  not model mempool fee or cost policy.
* The same paths were signed by chia-blockchain 2.7.5's own RPC handlers and
  ``WalletSigner`` (``tests/test_reference_wallet_real_signer.py``; skipped in
  CI, where chia-blockchain is not installed). The same module runs that
  wallet's coin-recognition code on a real option mint: it dismisses the
  launcher and matches the option coin against nothing.
* The read-only parts were run by hand against mainnet on 2026-11-14:
  :func:`chain.load_option` loaded all 20 options then listed on
  xchoptions.com through warp's ``CoinsetClient`` and derived the terms the
  site shows, and :func:`signing.verify` accepted the signatures of four live
  offers, which have the same structure as the offers built here.
* The desk client and monitor were run read-only against the live site.
* NOT checked: a live wallet daemon over its HTTPS RPC, any broadcast to
  mainnet, xchoptions.com accepting an offer built here, the wallet's full sync
  loop meeting an option mint against a live node, and the CoinGecko feed in
  :mod:`market` (tested against scripted answers only). ``docs/options-live-test.md``
  is the checklist for the first of these; follow it on a throwaway key with
  dust amounts before trusting this with funds.

One thing to know before wiring this in: the wallet's ``push_tx`` reports no
verdict. A bundle its node refuses looks the same to the caller as one it
accepts, so outcomes are read from the chain with :func:`chain.bundle_status`.

``chia_wallet_sdk`` is imported inside functions, as warp does with its heavy
dependencies, so importing the GUI never fails when it is absent.
"""

__all__: list[str] = []
