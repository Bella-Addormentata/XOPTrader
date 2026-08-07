"""Expected CLVM tree hashes for every vendored puzzle reveal.

These are the SHA-256 tree hashes (``sha256tree``) of the ``.hex`` files in this
directory. :func:`gui.services.warp.clvm_utils.load_puzzle` asserts a freshly
parsed reveal against the matching entry here before returning it, so a
tampered, truncated, or wrong-version puzzle fails closed instead of silently
producing an unspendable (funds-losing) bundle.

Cross-checked this build against ``chia_rs.Program.get_tree_hash`` for the nine
on-disk reveals, and against the published warp-ui constants for the two burn
reveals (``cat_burner`` ``cf574348...``, ``burn_inner_puzzle`` ``69b9ac68...``).
"""

from __future__ import annotations

EXPECTED_TREE_HASHES: dict[str, str] = {
    # warpdotgreen/cli (MIT)
    "portal_receiver": "6a91ad29cc7d8d16514316d20e154a393faa35d6474cec7c597a15fd017101d9",
    "message_coin": "331e8775ee22bbb793b411071ba6d49739d8d496dff7d9eb059ac787ef7e89fc",
    "bridging_puzzle": "a09eb1ea8c6e83c0166801dabcf4a70d361cc7f6d89c4a46bcd400ac57719037",
    "wrapped_tail": "2d7e6fd2e8dd27536ebba2cf6b9fde09493fa10037aa64e14b201762c902f013",
    "cat_minter": "9e1c1ca30ea296accdbd046b91b40f7907f86e33d41b57102211c48a3f9aa0f1",
    "cat_mint_and_payout": "2c78140b52765a1c063062775d31a33a452410e9777c01270c1001db6e821f37",
    # warp-ui drivers/erc20bridge.tsx (MIT)
    "cat_burner": "cf5743483ed4d0f5536a11877f5b15629b04e6bf34943843c5cad97ce61f2505",
    "burn_inner_puzzle": "69b9ac68db61a9941ff537cbb69158a7e1015ad44c42cff905159909cd8e1f90",
    # chia-blockchain (Apache-2.0)
    "singleton_top_layer_v1_1": "7faa3253bfddd1e0decb0906b2dc6247bbc4cf608f58345d173adb63e8b47c9f",
    "cat_v2": "37bef360ee858133b69d595a906dc45d01af50379dad515eb9518abb7c1d2a7a",
    "p2_delegated_puzzle_or_hidden_puzzle": "e9aaa49f45bad5c889b86ee3341550c155cfdd10c3a6757de618d20612fffd52",
}
