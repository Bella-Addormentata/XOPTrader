"""The validators' unwrap spend policy, run against our own burn bundle.

Since warpdotgreen/cli#26 every validator re-derives a Chia -> Base message
from the burn's own spends before it signs, and writes a permanent
"rejected" for any deviation (``validate_chia_erc20_unwrap`` in
``commands/spend_policy.py``). A rejected burn has already melted the
wUSDC.b and no signature will ever arrive, so the job sits in
COLLECTING_EVM_SIGS until it expires.

``_validate_chia_erc20_unwrap`` below is a port of that function, check for
check, from warpdotgreen/cli@29f6bd1 (chia ``Program`` -> our clvm ``SExp``).
It runs on what :func:`build_burn_bundle` emits, so a builder change the
validators would refuse fails here, before it can cost an unwrap. Re-port it
when upstream changes the policy.
"""

from __future__ import annotations

import dataclasses

import pytest

pytest.importorskip("clvm")
pytest.importorskip("chia_rs")

from gui.services.warp import clvm_utils as cu  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import drivers  # noqa: E402
from gui.services.warp.drivers import Coin, build_burn_bundle  # noqa: E402

from .test_warp_burn_drivers import _synthetic_request  # noqa: E402

NET = C.MAINNET

CREATE_COIN = 51
CREATE_COIN_ANNOUNCEMENT = 60
ASSERT_COIN_ANNOUNCEMENT = 61


def _hx(s: str) -> bytes:
    return bytes.fromhex(s[2:] if s.startswith("0x") else s)


def _run(puzzle, solution):
    from clvm import run_program
    from clvm.operators import OPERATOR_LOOKUP

    _cost, out = run_program(puzzle, solution, OPERATOR_LOOKUP)
    return out


def _atom(node) -> bytes:
    if node.atom is None:
        raise ValueError("expected atom")
    return bytes(node.atom)


def _items(node) -> list:
    return list(node.as_iter())


def _uncurry(program):
    """cu.uncurry with every part lifted to SExp (it yields raw CLVMObjects)."""
    out = cu.uncurry(program)
    if out is None:
        return None
    mod, args = out
    return drivers.SExp.to(mod), [drivers.SExp.to(a) for a in args]


def _same(a, b) -> bool:
    return cu.sha256tree(a) == cu.sha256tree(b)


def _left_pad_32(value: bytes) -> bytes:
    if len(value) > 32:
        raise ValueError("value longer than 32 bytes")
    return b"\x00" * (32 - len(value)) + value


def _evm_address_from_bytes32(value: bytes) -> bytes:
    if len(value) == 20:
        return value
    if len(value) == 32:
        if value[:12] != b"\x00" * 12:
            raise ValueError("bytes32 address has non-zero prefix")
        return value[-20:]
    raise ValueError("address must be 20 bytes or left-padded 32 bytes")


def _as_int_word(value: bytes) -> int:
    return int.from_bytes(_left_pad_32(value), "big")


def _extract_bridging_memo(puzzle, solution, expected_amount: int):
    try:
        conditions = _run(puzzle, solution)
    except Exception:
        return None
    found = None
    for condition in conditions.as_iter():
        items = _items(condition)
        if not items or items[0].as_int() != CREATE_COIN or len(items) < 4:
            continue
        if _atom(items[1]) != drivers.BRIDGING_PUZZLE_HASH:
            continue
        if items[2].as_int() != expected_amount:
            continue
        memo_items = _items(items[3])
        if len(memo_items) < 2:
            return None
        candidate = (
            _atom(memo_items[0]),
            _atom(memo_items[1]),
            tuple(_atom(x) for x in memo_items[2:]),
        )
        if found is not None and found != candidate:
            return None
        found = candidate
    return found


def _is_full_melt(conditions) -> bool:
    saw_melt = False
    for condition in conditions.as_iter():
        items = _items(condition)
        if not items or items[0].as_int() != CREATE_COIN:
            continue
        if len(items) < 3:
            return False
        amount = items[2].as_int()
        if amount == -113:
            saw_melt = True
            continue
        if amount > 0:
            return False
    return saw_melt


def _has_announcement(conditions, opcode: int, payload: bytes) -> bool:
    for condition in conditions.as_iter():
        items = _items(condition)
        if not items or items[0].as_int() != opcode or len(items) < 2:
            continue
        if _atom(items[1]) == payload:
            return True
    return False


def _announcements_tie(burner_conds, cat_inner_conds, burner_id: bytes, cat_id: bytes) -> bool:
    return (
        _has_announcement(burner_conds, CREATE_COIN_ANNOUNCEMENT, cat_id)
        and _has_announcement(
            burner_conds, ASSERT_COIN_ANNOUNCEMENT, cu.sha256(cat_id, burner_id)
        )
        and _has_announcement(cat_inner_conds, CREATE_COIN_ANNOUNCEMENT, burner_id)
        and _has_announcement(
            cat_inner_conds, ASSERT_COIN_ANNOUNCEMENT, cu.sha256(burner_id, cat_id)
        )
    )


def _validate_chia_erc20_unwrap(bridging_coin, burner_spend, cat_spend, net):
    """Port of upstream validate_chia_erc20_unwrap.

    Returns ``("accept", verified_message)`` or ``("reject", reason)``. The
    route is the mainnet Base one: ``bse`` -> net.erc20_bridge_address.
    """
    if cat_spend is None:
        return "reject", "missing CAT spend"
    try:
        burner_puzzle = burner_spend.puzzle_reveal
        burner_solution = burner_spend.solution
        uncurried = _uncurry(burner_puzzle)
        if uncurried is None or not _same(uncurried[0], drivers.CAT_BURNER_MOD):
            return "reject", "parent is not CAT_BURNER_MOD"
        curry_args = uncurried[1]
        if len(curry_args) != 5:
            return "reject", "burner curry arity"
        cat_mod_hash, burn_inner_mod_hash, bridging_ph, dest_chain, dest_addr = (
            _atom(a) for a in curry_args
        )
        if cat_mod_hash != drivers.CAT_MOD_HASH:
            return "reject", "burner CAT_MOD_HASH mismatch"
        if burn_inner_mod_hash != drivers.BURN_INNER_PUZZLE_MOD_HASH:
            return "reject", "burner BURN_INNER_PUZZLE_MOD_HASH mismatch"
        if bridging_ph != drivers.BRIDGING_PUZZLE_HASH:
            return "reject", "burner BRIDGING_PUZZLE_HASH mismatch"

        bridge = _hx(net.erc20_bridge_address)
        if dest_chain != net.source_chain.encode() or _evm_address_from_bytes32(
            _left_pad_32(dest_addr)
        ) != _evm_address_from_bytes32(_left_pad_32(bridge)):
            return "reject", "burner destination is not a configured route"

        expected_burner = drivers.get_cat_burner_puzzle(dest_chain, dest_addr)
        if not _same(burner_puzzle, expected_burner):
            return "reject", "burner puzzle hash mismatch"
        if burner_spend.coin.puzzle_hash != cu.sha256tree(burner_puzzle):
            return "reject", "burner coin puzzle hash mismatch"

        sol_args = _items(burner_solution)
        if len(sol_args) != 8:
            return "reject", "burner solution arity"
        cat_parent_info = _atom(sol_args[0])
        tail_hash_hash = _atom(sol_args[1])
        cat_amount = sol_args[2].as_int()
        memo_asset = _atom(sol_args[3])
        memo_receiver = _atom(sol_args[4])
        my_amount = sol_args[5].as_int()
        my_puzzle_hash = _atom(sol_args[6])
        my_coin_id = _atom(sol_args[7])

        if my_amount != burner_spend.coin.amount:
            return "reject", "burner amount mismatch"
        if my_puzzle_hash != burner_spend.coin.puzzle_hash:
            return "reject", "burner solution puzzle hash mismatch"
        if my_coin_id != burner_spend.coin.name():
            return "reject", "burner solution coin id mismatch"

        memo = _extract_bridging_memo(burner_puzzle, burner_solution, burner_spend.coin.amount)
        if memo is None:
            return "reject", "burner missing bridging CREATE_COIN memo"
        memo_chain, memo_dest, memo_contents = memo
        if memo_chain != dest_chain or _left_pad_32(memo_dest) != _left_pad_32(dest_addr):
            return "reject", "memo destination does not match burner curry"
        if _evm_address_from_bytes32(_left_pad_32(memo_dest)) != _evm_address_from_bytes32(
            _left_pad_32(bridge)
        ):
            return "reject", "memo destination is not the route bridge"
        if len(memo_contents) != 3:
            return "reject", "unwrap memo must be exactly 3 words"
        if _left_pad_32(memo_contents[0]) != _left_pad_32(memo_asset):
            return "reject", "memo asset mismatch"
        if _left_pad_32(memo_contents[1]) != _left_pad_32(memo_receiver):
            return "reject", "memo receiver mismatch"
        if _as_int_word(memo_contents[2]) != cat_amount:
            return "reject", "memo amount mismatch"

        if bridging_coin.parent_coin_info != burner_spend.coin.name():
            return "reject", "bridging coin parent is not burner"
        if bridging_coin.puzzle_hash != drivers.BRIDGING_PUZZLE_HASH:
            return "reject", "bridging coin puzzle hash"
        if bridging_coin.amount != burner_spend.coin.amount:
            return "reject", "bridging coin amount"

        asset = _left_pad_32(memo_asset)
        wrapped_tail = drivers.get_wrapped_tail(
            _hx(net.portal_launcher_id), dest_chain, dest_addr, asset
        )
        if tail_hash_hash != cu.sha256(b"\x01", cu.sha256tree(wrapped_tail)):
            return "reject", "tail_hash_hash mismatch"

        expected_inner = drivers.get_cat_burn_inner_puzzle(
            dest_chain, dest_addr, asset, memo_receiver, burner_spend.coin.amount
        )
        expected_cat_puzzle = drivers.construct_cat_puzzle(
            cu.sha256tree(wrapped_tail), expected_inner
        )
        expected_cat_coin = Coin(
            cat_parent_info, cu.sha256tree(expected_cat_puzzle), cat_amount
        )
        if cat_spend.coin != expected_cat_coin:
            return "reject", "CAT coin does not match burner reconstruction"

        cat_uncurried = _uncurry(cat_spend.puzzle_reveal)
        if cat_uncurried is None or not _same(cat_uncurried[0], drivers.CAT_MOD):
            return "reject", "CAT spend is not CAT_MOD"
        cat_args = cat_uncurried[1]
        if len(cat_args) != 3:
            return "reject", "CAT curry arity"
        if _atom(cat_args[0]) != drivers.CAT_MOD_HASH:
            return "reject", "CAT inner CAT_MOD_HASH mismatch"
        if _atom(cat_args[1]) != cu.sha256tree(wrapped_tail):
            return "reject", "CAT tail hash mismatch"
        inner_puzzle = cat_args[2]
        if not _same(inner_puzzle, expected_inner):
            return "reject", "CAT burn-inner mismatch"

        inner_1 = _uncurry(inner_puzzle)
        if inner_1 is None or len(inner_1[1]) != 2:
            return "reject", "burn-inner second curry arity"
        if _left_pad_32(_atom(inner_1[1][0])) != _left_pad_32(memo_receiver):
            return "reject", "burn-inner receiver mismatch"
        if inner_1[1][1].as_int() != burner_spend.coin.amount:
            return "reject", "burn-inner fee mismatch"
        inner_0 = _uncurry(inner_1[0])
        if inner_0 is None or not _same(inner_0[0], drivers.BURN_INNER_PUZZLE_MOD):
            return "reject", "inner is not BURN_INNER_PUZZLE_MOD"
        if len(inner_0[1]) != 2:
            return "reject", "burn-inner first curry arity"
        if _atom(inner_0[1][0]) != burner_spend.coin.puzzle_hash:
            return "reject", "burn-inner burner puzzle hash mismatch"
        if _left_pad_32(_atom(inner_0[1][1])) != asset:
            return "reject", "burn-inner token mismatch"

        cat_sol_args = _items(cat_spend.solution)
        if len(cat_sol_args) < 7:
            return "reject", "CAT solution arity"
        inner_solution = cat_sol_args[0]
        extra_delta = cat_sol_args[6].as_int()
        inner_sol_args = _items(inner_solution)
        if len(inner_sol_args) != 3:
            return "reject", "burn-inner solution arity"
        if not _same(inner_sol_args[2], wrapped_tail):
            return "reject", "tail reveal is not reconstructed wrapped_tail"

        if cat_spend.coin.amount != cat_amount:
            return "reject", "CAT amount does not match message amount"
        if extra_delta != -cat_amount:
            return "reject", "CAT extra_delta is not full melt"

        try:
            inner_conds = _run(inner_puzzle, inner_solution)
        except Exception:
            return "reject", "burn-inner spend failed"
        if not _is_full_melt(inner_conds):
            return "reject", "CAT spend is not a full melt"

        try:
            burner_conds = _run(burner_puzzle, burner_solution)
        except Exception:
            return "reject", "burner spend failed"
        if not _announcements_tie(
            burner_conds, inner_conds, burner_spend.coin.name(), cat_spend.coin.name()
        ):
            return "reject", "burner/CAT announcements do not tie"

        if len(burner_spend.coin.puzzle_hash) != 32:
            return "reject", "memo content longer than 32 bytes"
        try:
            canonical = tuple(_left_pad_32(w) for w in memo_contents)
        except ValueError:
            return "reject", "memo content longer than 32 bytes"
        return "accept", {
            "source": burner_spend.coin.puzzle_hash,
            "destination_chain": memo_chain,
            "destination": memo_dest,
            "contents": canonical,
        }
    except Exception as exc:  # noqa: BLE001 -- mirrors upstream's catch-all
        return "reject", f"chia erc20 unwrap parse error: {exc}"


def _spends(bundle):
    _security, cat_spend, burner_spend, _bridging = bundle.coin_spends
    return cat_spend, burner_spend


def _validate(out):
    cat_spend, burner_spend = _spends(out.bundle)
    return _validate_chia_erc20_unwrap(out.bridging_coin, burner_spend, cat_spend, NET)


def test_our_burn_bundle_passes_the_validators_unwrap_policy():
    req, burn_cat = _synthetic_request()
    out = build_burn_bundle(req)

    kind, verified = _validate(out)
    assert kind == "accept", verified

    # And the message the validators verify is the one we sign-check and
    # relay: source == the cat_burner puzzle hash, contents byte-equal.
    assert verified["source"].hex() == NET.burn_puzzle_hash
    assert verified["destination_chain"] == b"bse"
    assert verified["contents"] == tuple(
        drivers.outbound_message_contents(NET, req.receiver, burn_cat.amount)
    )


def test_the_port_rejects_a_burn_that_does_not_fully_melt():
    """The port has teeth: a CAT spend that keeps its value is refused."""
    req, _ = _synthetic_request()
    out = build_burn_bundle(req)
    cat_spend, burner_spend = _spends(out.bundle)
    fields = _items(cat_spend.solution)
    fields[6] = drivers.SExp.to(0)
    kept = dataclasses.replace(cat_spend, solution=drivers.SExp.to(fields))

    kind, reason = _validate_chia_erc20_unwrap(out.bridging_coin, burner_spend, kept, NET)
    assert (kind, reason) == ("reject", "CAT extra_delta is not full melt")


def test_the_port_rejects_a_bridging_coin_of_the_wrong_amount():
    req, _ = _synthetic_request()
    out = build_burn_bundle(req)
    cat_spend, burner_spend = _spends(out.bundle)
    wrong = Coin(out.bridging_coin.parent_coin_info, out.bridging_coin.puzzle_hash, 1)

    kind, reason = _validate_chia_erc20_unwrap(wrong, burner_spend, cat_spend, NET)
    assert (kind, reason) == ("reject", "bridging coin amount")
