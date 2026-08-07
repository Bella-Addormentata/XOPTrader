"""Vendored CLVM puzzle reveals (serialized hex) for the warp.green bridge.

Sources and licences:

* warpdotgreen/cli (MIT): ``portal_receiver``, ``message_coin``,
  ``bridging_puzzle``, ``wrapped_tail``, ``cat_minter``, ``cat_mint_and_payout``.
* warp-ui ``drivers/erc20bridge.tsx`` (MIT): ``cat_burner``,
  ``burn_inner_puzzle`` (published as JS constants, not shipped as ``.hex``).
* chia-blockchain (Apache-2.0): ``singleton_top_layer_v1_1``, ``cat_v2``,
  ``p2_delegated_puzzle_or_hidden_puzzle``.

Each reveal is verified against :mod:`gui.services.warp.puzzles.hashes` at load
time (see :func:`gui.services.warp.clvm_utils.load_puzzle`) -- a corrupted or
substituted reveal raises before any funds can move.
"""
