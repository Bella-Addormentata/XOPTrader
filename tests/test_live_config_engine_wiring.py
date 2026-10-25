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


def test_reloaded_fee_bounds_reach_the_cached_execution_fees():
    body = _definition(APPLY)
    reconf = body.find("offer_mgr_->reconfigure(config_);")
    assert reconf != -1
    after = body[reconf:]
    assert "offer_mgr_->clamp_fees(config_.fees.min_fee_mojos,config_.fees.max_fee_mojos);" in after, (
        "a gated Step 8 leaves OfferManager's attached and cancel fees as "
        "they were, and the TTL sweep cancels at them: a lowered "
        "max_fee_mojos must reach them at the reload"
    )
    assert "offer_mgr_->set_dynamic_fee(config_.strategy.offer_fee_mojos);" in after, (
        "with the FeeTracker off the static fee is the fee"
    )


def test_a_tibetswap_change_withdraws_the_amm_observations():
    body = _definition(APPLY)
    start = body.find("if(tibetswap_changed){")
    assert start != -1
    block = body[start:]
    for needle in ("arb_detector_->set_tibetswap_reserves({});",
                   "tibetswap_reserves_at_=Timestamp{};",
                   "tibetswap_reserves_pending_=false;",
                   "market_data_->clear_amm_observation(pair.name);"):
        assert needle in block, (
            f"{needle} missing: a TibetSwap feed switched off kept feeding "
            "the fair-value solve from its last reserves"
        )


METRICS_CPP = REPO / "cpp" / "src" / "monitoring" / "metrics.cpp"


def test_a_dropped_peg_asset_leaves_the_peg_panel():
    text = METRICS_CPP.read_text(encoding="utf-8").replace("\r\n", "\n")
    start = text.find("void MetricsExporter::update_peg_status(")
    assert start != -1
    body = _code(text[start:text.find("\n}\n", start)])
    assert "peg_status_family_->Remove(" in body and \
        "peg_deviation_family_->Remove(" in body, (
        "a reloaded registry that drops or stops enforcing an asset left "
        "its gauges exported, so the Settings peg panel kept a stale row"
    )
    assert "peg_labels_=std::move(published);" in body


def test_a_peg_no_longer_enforced_releases_its_latch():
    body = _definition(APPLY)
    start = body.find("config_.pegged_assets=m.pegged_assets;")
    assert start != -1
    after = body[start:]
    assert "a!=nullptr&&a->enforce" in after and "asset_peg_rt_.erase(it)" in after, (
        "a suspension latched on a peg the saved registry drops or stops "
        "enforcing blocked its pairs for good, with no re-enable offered"
    )
    assert "publish_peg_status();" in body, (
        "the reload must republish the peg gauges so dropped rows go"
    )


def test_a_pair_leaving_isolated_revive_drops_its_crash_state():
    body = _definition(APPLY)
    assert "revive_crash_state_.erase(it)" in body, (
        "a Crash or Recovery left in revive_crash_state_ for a pair no "
        "longer isolated is never advanced, and keeps its quotes suppressed"
    )
    assert body.find("revive_crash_state_") > body.find("config_.strategy=m.strategy;"), (
        "the isolation flag must be read from the SAVED strategy section"
    )
