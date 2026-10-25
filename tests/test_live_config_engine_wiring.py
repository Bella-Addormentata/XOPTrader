"""[LIVECFG] What Engine::apply_live_config() does with engine-held state.
Source scan.

LINT-CLASS GUARD, disclosed as such (precedent: tests/test_stop_keep_wiring.py).
Nothing in cpp/tests can construct an Engine, so the setters are pinned by
gtest (cpp/tests/test_config_live_apply.cpp) and the engine's use of them is
pinned here, over the text of cpp/src/engine.cpp. A pass proves the calls are
present; it says nothing about run-time behaviour.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE_CPP = REPO / "cpp" / "src" / "engine.cpp"


def _code(text: str) -> str:
    """Line comments removed and all whitespace squeezed out."""
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"\s+", "", text)


def _definition(signature: str) -> str:
    text = ENGINE_CPP.read_text(encoding="utf-8").replace("\r\n", "\n")
    start = text.find(signature)
    assert start != -1, f"{signature!r} not found in engine.cpp"
    end = text.find("\n}\n", start)
    assert end != -1, f"end of {signature!r} not found in engine.cpp"
    return _code(text[start:end])


APPLY = "void Engine::apply_live_config(const AppConfig& m)"


def test_a_restarted_fee_controller_drops_the_open_tickets():
    body = _definition(APPLY)
    assert "fee_tracker_->set_config(config_.fees)&&" in body, (
        "the restart flag set_config() returns must gate the ticket reset: "
        "tickets carry submit levels against the old anchor and class costs"
    )
    assert "fee_tickets_.clear();" in body


def test_a_dropped_reserve_releases_its_coins():
    body = _definition(APPLY)
    assert "m.strategy.asset_reserve_units.count(it->first)" in body, (
        "assets dropped from strategy.asset_reserve_units must be found "
        "against the SAVED config"
    )
    assert "offer_mgr_->set_reserved_coins(it->second,{})" in body, (
        "Step 8 refreshes only the reserves still listed: a dropped one "
        "kept its coins excluded from every offer until a restart"
    )
    assert body.find("reserve_wallet_ids_") < body.find(
        "config_.strategy=m.strategy;"), (
        "the dropped reserves are found before config_.strategy is replaced"
    )


def test_step_8_records_the_wallet_of_each_reserve_it_sets():
    code = _code(ENGINE_CPP.read_text(encoding="utf-8"))
    set_call = code.find("offer_mgr_->set_reserved_coins(wid,")
    assert set_call != -1, "Step 8's reserve refresh not found"
    assert code.find("reserve_wallet_ids_[asset_id]=wid;", set_call) != -1, (
        "without the record, a reserve dropped by a save has no wallet "
        "for apply_live_config() to clear"
    )
