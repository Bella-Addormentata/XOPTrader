"""[S70-S72 2026-09-20] Call-site wiring of the three cancel-reduction switches.

LINT-CLASS GUARD, disclosed as such.  These tests read cpp/src/engine.cpp and
cpp/src/execution/offer_manager.cpp as TEXT.  Engine and OfferManager are not
constructible in xop_tests, so the DECISIONS are pinned by gtest
(test_offer_expiry.cpp, test_exposure_gate.cpp, test_cross_guard.cpp,
test_price_cancel_replay.cpp) and only the WIRING is pinned here: that each
call site still asks the shared decision, with the inputs that make it safe.
A pass says nothing about runtime behaviour.

What each scan guards:

  S70  ttl_cancel_mode: expire sends the one INSECURE cancel this engine sends
       on purpose.  It must stay behind the expiry verdict, read the chain
       clock from the wallet, leave the offer_log row to the wallet verdict,
       and must not leak into the stopped-engine sweep.
  S71  the two exposure sites must share ONE verdict and the unified inputs.
  S72  the canceller must be handed Step 7's OWN centre and floor, and the
       anchor override must not survive into margin mode.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"
OFFER_MANAGER = REPO / "cpp" / "src" / "execution" / "offer_manager.cpp"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def _code(text: str) -> str:
    """`text` with every // comment removed, so prose cannot satisfy a scan."""
    return "\n".join(line.split("//", 1)[0] for line in text.split("\n"))


def _region(text: str, start_marker: str, end_marker: str) -> str:
    start = text.index(start_marker)
    return text[start:text.index(end_marker, start)]


def _squash(text: str) -> str:
    return re.sub(r"\s+", "", text)


# --------------------------------------------------------------------------
# S70
# --------------------------------------------------------------------------

def _retire() -> str:
    return _code(_region(_read(OFFER_MANAGER),
                         "OfferManager::retire_expired_offers(BlockHeight current_block)",
                         "// post_quotes -- create multi-tier bid + ask offers on-chain"))


def test_the_hard_ttl_asks_the_expire_mode_predicate():
    classify = _code(_region(_read(OFFER_MANAGER),
                             "std::vector<TierClassification> OfferManager::classify_tier_staleness(",
                             "// [T5-01] selective_cancel -- cancel only stale/expired tiers"))
    assert ("if(past_hard_ttl&&age_limit_cancel_applies(expire_mode,po.expiry_max_time)){"
            in _squash(classify)), (
        "the unconditional hard-TTL cancel must be gated by age_limit_cancel_applies"
    )
    assert "strategy_cfg_.ttl_cancel_mode == TtlCancelMode::Expire" in classify


def test_the_local_cancel_sits_behind_the_expiry_verdict():
    retire = _retire()
    assert retire.count("/*secure=*/false") == 1, "exactly one insecure cancel"
    verdict = retire.index("decide_expired_retire(")
    gate = retire.index("if (verdict != ExpiredRetire::RetireLocal)")
    cancel = retire.index("/*secure=*/false")
    assert verdict < gate < cancel, (
        "the insecure cancel must come after decide_expired_retire and after the "
        "non-RetireLocal verdicts have left the loop"
    )
    # Fee 0 through the one charged choke point, like every other cancel.
    assert "cancel_offer_charged(po.offer_id, 0, /*secure=*/false)" in retire
    assert "expired_at_depth(po.expiry_max_time, chain_time_s)" in retire


def test_the_retire_reads_the_wallets_chain_clock_not_the_hosts():
    retire = _retire()
    height = retire.index("wallet_->get_height_info()")
    depth = retire.index("expired_retire_clock_height(synced_height)")
    clock = retire.index("wallet_->get_timestamp_for_height(clock_height)")
    first_offer_read = retire.index("wallet_->get_offer(")
    assert height < depth < clock < first_offer_read, (
        "the chain clock is read once, kExpiredRetireDepthBlocks below the "
        "wallet's finished-sync height, before any offer is judged"
    )
    # [review #164] A clock read at the TIP proves no confirmation depth: the
    # first block stamped past max_time can be the tip itself.
    assert "get_timestamp_for_height(synced_height)" not in retire
    assert retire.count("get_timestamp_for_height(") == 1
    # The host clock feeds the pre-filter and nothing else.
    assert retire.count("host_now_s") == 2, retire.count("host_now_s")
    assert "expiry_worth_checking(po.expiry_max_time" in retire
    assert "decide_expired_retire(pending_accept" in retire
    assert "trade_record_max_time(rec)" in retire


def test_the_retire_is_off_unless_the_operator_chose_expire():
    retire = _retire()
    guard = retire.index("if (strategy_cfg_.ttl_cancel_mode != TtlCancelMode::Expire)")
    first_state_read = retire.index("state_->get_all_offers()")
    assert guard < first_state_read, "the default mode must return before it reads anything"


def test_a_retired_offer_waits_for_the_wallet_verdict():
    retire = _retire()
    assert "state_->mark_cancel_pending(po.offer_id)" in retire
    assert "remove_offer" not in retire, (
        "the offer stays in State: only the wallet's CANCELLED verdict, seen by "
        "detect_fills, completes its offer_log row"
    )
    assert "state_->set_offer_expiry(po.offer_id, 0)" in retire, (
        "an unverified expiry must be dropped so the hard TTL applies again"
    )
    engine = _read(ENGINE)
    site = _code(_region(engine, "// [S70 2026-09-20] ttl_cancel_mode: expire -- free the coins",
                         "// [XCH-LOCK-LEDGER 2026-08-23] Seed the per-cycle XCH coin-lock budget"))
    assert 'mark_offer_cancel_submitted(oid, block_height,' in site
    assert '"expired_onchain"' in site
    assert "update_offer_status" not in site, "an accepted local cancel is a submission"


def test_the_engine_retires_below_the_sync_gate_and_above_the_lock_ledger():
    engine = _code(_read(ENGINE))
    assert engine.count("retire_expired_offers(") == 1
    sync_gate = engine.index("wallet_->get_sync_status();",
                             engine.index("Engine::step_manage_offers(BlockHeight block_height)"))
    retire = engine.index("retire_expired_offers(")
    ledger = engine.index("offer_mgr_->begin_xch_lock_cycle()")
    assert sync_gate < retire < ledger
    call = _squash(engine[retire - 400:retire])
    assert "config_.strategy.ttl_cancel_mode==TtlCancelMode::Expire" in call
    assert 'wallet_step_may_run("Step8expired-offerretire")' in call


def test_only_a_verified_echo_or_a_wallet_record_sets_the_expiry():
    manager = _code(_read(OFFER_MANAGER))
    assert len(re.findall(r"!expiry_echo_ok\(", manager)) == 3
    sets = [m.start() for m in re.finditer(
        r"expiry_max_time\s*=\s*expiry_max_time\.value_or\(0\);", manager)]
    assert len(sets) == 3, "each of the three posting paths records the echoed expiry"
    for at in sets:
        echo = manager.rfind("!expiry_echo_ok(", 0, at)
        assert echo != -1
        assert "continue;" in manager[echo:at] or "co_return 0;" in manager[echo:at], (
            "the expiry may be recorded only past the echo check's early exit"
        )
    parse = _region(manager, "OfferManager::try_parse_wallet_offer(",
                    "OfferManager::parse_settled_fill(")
    assert "po.expiry_max_time = trade_record_max_time(trade_record);" in parse
    fills = _region(manager, "OfferManager::detect_fills(", "OfferManager::recheck_terminal(")
    backfill = _squash(fills)
    assert "if(strategy_cfg_.ttl_cancel_mode==TtlCancelMode::Expire){" in backfill
    assert "state_->set_offer_expiry(id,max_time)" in backfill
    assert "!=trade_status::kPendingAccept" in backfill


def test_only_the_stuck_pass_spares_expiring_offers():
    engine = _code(_read(ENGINE))
    assert engine.count("/*spare_expiring=*/true") == 1
    stuck = _code(_region(_read(ENGINE), "// -- Stuck offer detection",
                          "// -- Spendable reserve & pending-change gating"))
    assert "/*spare_expiring=*/true" in stuck
    assert "execution::age_limit_cancel_applies(stuck_expire_mode" in stuck, (
        "the stuck COUNTER and cancel_stale must apply the same exemption"
    )
    stopdrain = _region(engine, "Engine::step_sweep_stale_offers(BlockHeight block_height)",
                        "stopdrain_failed_cycles_ = 0")
    assert stopdrain.count("offer_mgr_->cancel_stale(") == 2
    assert "spare_expiring" not in stopdrain, (
        "a stopped book is aged out at the soft TTL whatever the mode"
    )
    manager = _code(_read(OFFER_MANAGER))
    stale = _region(manager, "OfferManager::cancel_stale(", "OfferManager::cancel_all(")
    assert ("if(spare_expiring&&!age_limit_cancel_applies(expire_mode,po.expiry_max_time)){continue;}"
            in _squash(stale))


def test_the_insecure_cancel_has_exactly_two_homes():
    manager = _code(_read(OFFER_MANAGER))
    assert manager.count("/*secure=*/false") == 2, (
        "emergency_cancel's last resort and the expired-offer retire; a third "
        "insecure cancel needs review"
    )


# --------------------------------------------------------------------------
# S71
# --------------------------------------------------------------------------

def test_both_exposure_sites_ask_the_one_verdict():
    engine = _code(_read(ENGINE))
    calls = [m.start() for m in re.finditer(r"execution::decide_exposure\(", engine)]
    assert len(calls) == 2, "the resting-offer check and the pre-post projection"
    for at in calls:
        args = _squash(engine[at:at + 260])
        assert "exposure_unified,in," in args
        assert "config_.strategy.exposure_cancel_hysteresis_pct" in args
    assert "execution::exposure_breaches_reserve(" not in engine, (
        "a private copy of the old arithmetic at either site is the defect"
    )
    assert ("constboolexposure_unified=config_.strategy.exposure_rule==ExposureRule::Unified;"
            in _squash(engine))


def test_the_unified_inputs_are_owned_and_the_asset_wide_resting_sum():
    engine = _code(_read(ENGINE))
    gate = _code(_region(_read(ENGINE), "struct SideBalance {",
                         "// Gate 2: spendable reserve too low"))
    assert 'bal_json.contains("unconfirmed_wallet_balance")' in gate
    assert ".is_number_integer()" in gate
    claims = _code(_region(_read(ENGINE),
                           "const auto exposure_resting_claims = [this]() {",
                           "// XCH-buy-only mode:"))
    assert "state_->get_all_offers()" in claims
    assert "pair_name !=" not in claims and "pair_name ==" not in claims, (
        "owned is wallet-wide, so resting must span every pair that spends the asset"
    )
    assert "claim.cancel_pending = po.cancel_pending;" in claims
    assert "find_pair_config(po.pair_name)" in claims
    assert engine.count("execution::resting_spend_on_asset(") == 2
    assert engine.count("exposure_resting_claims()") == 2, (
        "rebuilt from State at each site, so a cancel the first site sent is "
        "out of the second site's sum"
    )


def test_an_unmappable_offer_is_recorded_as_unknown_not_dropped():
    """[review #164] The claims lambda used to `continue` on a pair it could
    not resolve.  `owned` is the whole wallet and still counts the coins that
    offer has locked, so dropping it told the wallet-wide projection its spend
    was ZERO -- a fail-open, on an offer that is still takeable.  It must be
    recorded as unquantifiable instead, carrying its cancel_pending state.
    """
    claims = _code(_region(_read(ENGINE),
                           "const auto exposure_resting_claims = [this]() {",
                           "// XCH-buy-only mode:"))
    body = _squash(claims)
    assert "if(!claim_pc){" in body
    assert "unknown.pair_unmapped=true;" in body
    assert "unknown.cancel_pending=po.cancel_pending;" in body
    assert "claims.push_back(std::move(unknown));" in body
    # Nothing may leave the iteration before the claim is recorded: a bare
    # `continue` there IS the defect.
    unresolved = body.index("if(!claim_pc){")
    recorded = body.index("claims.push_back(std::move(unknown));")
    assert "continue;" not in body[unresolved:recorded], (
        "an unresolvable pair must record an unquantifiable claim before it "
        "skips the offer, never drop it silently"
    )


def test_both_exposure_sites_fail_closed_on_an_unquantifiable_claim():
    """Both sites must derive resting_incomplete from the claims they just
    built -- not from a constant, and not only at one of the two."""
    engine = _code(_read(ENGINE))
    assert engine.count("execution::has_unmapped_live_claim(") == 2, (
        "the resting-offer check and the pre-post projection"
    )
    body = _squash(engine)
    assert body.count(
        "in.resting_incomplete=execution::has_unmapped_live_claim(") == 2
    # The resting-offer site builds the claims ONCE and reads both the asset
    # sum and the flag off that same vector, so the two cannot disagree.
    plan = _squash(_code(_region(_read(ENGINE),
                                 "auto plan_exposure = [&](Side side) -> ExposurePlan {",
                                 "const auto suppress_instead = [&]")))
    assert ("conststd::vector<execution::RestingSpend>claims="
            "exposure_unified?exposure_resting_claims()" in plan)
    assert "execution::resting_spend_on_asset(claims," in plan
    assert "execution::has_unmapped_live_claim(claims);" in plan
    # The pre-post site reads the vector it already had.
    assert "execution::has_unmapped_live_claim(prepost_claims);" in body


def test_unified_spares_young_offers_and_suppresses_instead():
    engine = _code(_read(ENGINE))
    plan = _region(engine, "auto plan_exposure = [&](Side side) -> ExposurePlan {",
                   "const auto suppress_instead = [&]")
    assert "execution::exposure_cancel_candidate(" in plan
    assert "config_.strategy.exposure_cancel_min_age_blocks" in plan
    assert "cand.created_block, block_height" in plan
    body = _squash(engine)
    assert body.count("&&plan.cancel_ids.empty()){") == 2
    assert "can_ask=false;suppress_instead(plan,\"ask\"" in body
    assert "can_bid=false;suppress_instead(plan,\"bid\"" in body
    assert body.count('"exposure_floor_rebalance"') == 2


# --------------------------------------------------------------------------
# S72
# --------------------------------------------------------------------------

def test_the_canceller_gets_step_sevens_own_centre_and_floor():
    engine = _code(_read(ENGINE))
    calls = [m.start() for m in re.finditer(r"offer_mgr_->classify_tier_staleness\(", engine)]
    assert len(calls) == 2, "the pace pass and Step 8's main loop"
    pace, main = (_squash(engine[at:at + 900]) for at in calls)
    assert "pcs.quote_mid_mojos" not in pace.split(";")[0], (
        "the pace pass has no ladder and sends no reference: the deviation zones decide"
    )
    main_call = main.split(";")[0]
    assert ("static_cast<double>(pcs.quote_mid_mojos),pcs.quote_min_half_spread_bps,"
            "static_cast<double>(pcs.quote_fair_centre_mojos))") in main_call, (
        "[review #164] the shifted centre alone is the wrong frame for edge, and the "
        "fair centre alone rebuilds the post/cancel loop: the canceller needs both"
    )
    assert "execution::margin_breach_reason(tc.edge_bps,tc.required_edge_bps)" in _squash(engine)


def test_margin_mode_replaces_the_zones_and_the_anchor_override():
    classify = _code(_region(_read(OFFER_MANAGER),
                             "std::vector<TierClassification> OfferManager::classify_tier_staleness(",
                             "// [T5-01] selective_cancel -- cancel only stale/expired tiers"))
    assert classify.count("classify_tier_refresh_margin(") == 1, (
        "one call, in the shared lambda both branches use"
    )
    body = _squash(classify)
    assert "margin_centre,margin_fair_centre,margin_min_edge_bps,edge_retain);" in body
    assert "margin_edge_bps(po.side==Side::Ask,static_cast<double>(po.price),margin_centre,margin_fair_centre);" in body, (
        "the recorded edge must be the one the verdict was reached on"
    )
    assert "age<kMinRefreshAgeBlocks,po.side==Side::Ask" in body, "kMinRefreshAgeBlocks stays"
    assert "if(!margin_decided){" in body, "NoReference must fall back to the deviation zones"
    assert "caseMarginRefresh::NoReference:break;" in body
    assert "if(anchor_active&&!margin_decided&&tc.staleness==TierStaleness::Fresh" in body, (
        "the anchor override cancels FAVOURABLE drift; margin mode promises it never will"
    )
    assert "strategy_cfg_.price_cancel_mode == PriceCancelMode::Margin" in classify
    assert "strategy_cfg_.price_cancel_edge_retain" in classify
