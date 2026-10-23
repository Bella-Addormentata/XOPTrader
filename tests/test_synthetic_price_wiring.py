"""[SYNTH 2026-10-22] Synthetic-price wiring in the C++ engine, as a source scan.

LINT-CLASS GUARD, disclosed as such.  These tests read cpp/src/engine.cpp as
TEXT.  Engine is not constructible in xop_tests, so the call sites that decide
which revive pairs take a synthetic price, what the ladder centres on, and how
the revive loss budget marks a synthetically priced position are pinned here;
every DECISION they call is pinned by gtest (cpp/tests/test_synthetic_price.cpp).
A pass says the engine still CALLS those decisions where it must; it says
nothing about runtime behaviour.

Each test covers one finding from the first review of PR #180:
  * a listed pair whose feed prices are merely missing (a startup outage) must
    not fall back to the tape -- only a missing listing selects it;
  * the ladder must centre on the synthetic price, not the raw estimate;
  * the loss budget must mark a tape-only position conservatively, and treat
    a position it cannot value as over budget;
  * the estimate joins the synthetic blend only while the feed is fresh.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"

STEP1 = "asio::awaitable<void> Engine::step_update_market_state(BlockHeight block_height)"
STEP4 = "void Engine::step_compute_quotes(BlockHeight block_height)"
STEP7 = "void Engine::step_generate_ladder(BlockHeight block_height)"
STEP11 = "void Engine::step_update_pnl(BlockHeight block_height)"
REVIVE_MAINT = "asio::awaitable<void> Engine::step_revive_maintenance(BlockHeight block_height)"
SYNTH_PAIR = "std::optional<synth::PriceSource> Engine::synth_pair_price(const PairConfig& pc)"


def _code(text: str) -> str:
    """Drop `// ...` tails so a comment cannot satisfy a scan.  A `//` inside
    a string literal is left alone."""
    out = []
    for line in text.split("\n"):
        in_string = False
        cut = len(line)
        i = 0
        while i < len(line):
            ch = line[i]
            if in_string:
                if ch == "\\":
                    i += 2
                    continue
                if ch == '"':
                    in_string = False
            elif ch == '"':
                in_string = True
            elif line.startswith("//", i):
                cut = i
                break
            i += 1
        out.append(line[:cut])
    return "\n".join(out)


def _body(signature: str) -> str:
    """The brace-matched body of the function defined by `signature`, with
    line comments removed."""
    text = _code(ENGINE.read_text(encoding="utf-8").replace("\r\n", "\n"))
    start = text.find(signature)
    assert start >= 0, f"engine.cpp no longer defines {signature!r}"
    open_at = text.index("{", start + len(signature))
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_at : i + 1]
    raise AssertionError(f"unbalanced braces after {signature!r}")


def test_only_a_missing_listing_selects_the_synthetic_price():
    body = _body(STEP1)
    synth_call = body.find("synth_pair_price(")
    assert synth_call >= 0, "Step 1 no longer computes a synthetic price"
    # The gate in front of that call is the listing test, not a cache lookup.
    # It is the nearest `if (pair.revive_market ...` before the call.
    gate = body.rfind("if (pair.revive_market", 0, synth_call)
    assert gate >= 0, "the synthetic price is no longer gated on revive_market"
    window = body[gate:synth_call]
    assert re.search(r"revive_market\s*&&\s*pair_has_unlisted_leg\s*\(", window), (
        "the synthetic fallback must be selected by a missing feed LISTING; "
        "gating it on missing cached prices lets a listed pair skip the live "
        "feed during an outage"
    )
    assert "listed_cross_price" not in window


def test_the_ladder_centres_on_the_synthetic_price():
    body = _body(STEP7)
    blend = body.find("fv::blend_quote_center(")
    assert blend >= 0
    before = body[:blend]
    assert re.search(r"ext_price\s*=\s*pcs\.revive_synth_price\s*;", before), (
        "Step 7 must hand the synthetic price (not only its sigma) to the "
        "centre blend, in place of the estimate"
    )
    assert re.search(r"ext_sigma\s*=\s*pcs\.revive_synth_sigma_bps\s*;", before)
    call = body[blend : body.index(";", blend)]
    assert "ext_price" in call and "ext_sigma" in call


def test_step1_carries_the_synthetic_price_to_step7():
    body = _body(STEP1)
    assert re.search(r"\.revive_synth_price\s*=", body), (
        "Step 1 must record the synthetic price on the pair's cycle state"
    )


def test_the_budget_marks_a_synthetic_position_conservatively():
    body = _body(STEP11)
    record = body.find("revive_loss_budget_.record(")
    assert record >= 0
    before = body[:record]
    assert "synth::conservative_mark(" in before, (
        "a tape-only pair has no graded mid and no trusted fair value; its "
        "budget must mark at the synthetic price's unfavourable edge"
    )
    assert "synth_last_good_" in before


def test_an_unvaluable_position_fails_closed():
    step11 = _body(STEP11)
    assert re.search(r"revive_budget_blind_\.insert\s*\(", step11), (
        "a revive pair holding exposure its budget cannot value must be "
        "flagged"
    )
    assert re.search(r"revive_budget_blind_\.erase\s*\(", step11)
    step4 = _body(STEP4)
    assert re.search(r"revive_budget_blind_\.count\s*\(\s*pair_name\s*\)", step4), (
        "Step 4 must stop quoting a pair whose budget is blind"
    )
    maint = _body(REVIVE_MAINT)
    assert re.search(r"over_budget\s*=[^;]*revive_budget_blind_\.count\s*\(", maint), (
        "revive maintenance must cancel a blind pair's resting offers"
    )


def test_the_estimate_joins_the_blend_only_while_the_feed_is_fresh():
    body = _body(SYNTH_PAIR)
    est = body.find("get_fair_value_estimate(")
    assert est >= 0
    fresh = body.find("coingecko_feed_fresh_for_revival(")
    assert 0 <= fresh < est, (
        "the solve re-stamps its estimate from a retained cache during an "
        "outage; it must be gated on genuine feed freshness"
    )
    cond = body[est : body.index("{", est)]
    assert re.search(r"\bfeed_fresh\b", cond)


# ---------------------------------------------------------------------------
# Second review of PR #180: synthetic provenance must reach the ungraded
# readers, the budget must mark a synthetic quote leg consistently, and a
# trade timestamp must fit the clock before it becomes a time point.
# ---------------------------------------------------------------------------

USD_ANCHOR = "risk::XchUsdAnchor Engine::usd_per_xch_anchor() const"
ARB = "asio::awaitable<void> Engine::step_check_arbitrage("
DRIFT = "asio::awaitable<void> Engine::step_run_drift_corrector(BlockHeight block_height)"
TAPE = "asio::awaitable<void> Engine::refresh_synth_tape(const std::string& asset_id)"


def test_the_usd_anchor_fallback_refuses_a_synthetic_mid():
    body = _body(USD_ANCHOR)
    read = body.find("state_->get_market(")
    assert read >= 0
    use = body.find("usd_per_base_from_mid(", read)
    assert use > read
    assert re.search(r"if\s*\(\s*snap\.mid_synthetic\s*\)\s*return\s+std::nullopt\s*;",
                     body[read:use]), (
        "every XCH-quoted pair's USD factor is trusted on this anchor; a "
        "synthetic quoting price must not supply it"
    )


def test_neither_arbitrage_scan_prices_off_a_synthetic_mid():
    body = _body(ARB)
    tri = body.find("pair_prices[pair.name] = mid;")
    assert tri >= 0
    assert "mid_synthetic" in body[body.rfind("for (const auto& pair", 0, tri):tri], (
        "the triangular scan must skip synthetic mids"
    )
    rate = body.find("stable_cross_rate = mid;")
    assert rate >= 0
    loop = body.rfind("for (const auto& pc : config_.pairs)", 0, rate)
    window = body[loop:rate]
    assert "ssnap.mid_synthetic" in window and "xch_stable_pairs.clear()" in window, (
        "the cross-stable scan must not normalise real takes through a "
        "synthetic rate"
    )


def test_the_drift_corrector_does_not_fall_back_to_a_synthetic_mid():
    body = _body(DRIFT)
    fallback = body.find("market_data_->get_mid_price(pair.name)")
    assert fallback >= 0
    gate = body.rfind("if (", 0, fallback)
    assert re.search(r"mid\s*==\s*0\s*&&\s*!\s*snap\.mid_synthetic", body[gate:fallback])


def test_a_synthetic_quote_leg_is_marked_consistently():
    body = _body(STEP11)
    record = body.find("revive_loss_budget_.record(")
    marks = body.find("synth::synthetic_quote_marks(")
    assert 0 <= marks < record, (
        "a tape-priced quote leg must be valued with the mid derived from "
        "the same USD marks, not a par-backed or book mid"
    )
    after = body[marks:record]
    assert re.search(r"\bmid\s*=\s*m->mid\s*;", after)
    assert re.search(r"listed_quote_usd\s*=\s*m->quote_usd\s*;", after)


def test_trade_timestamps_are_bounded_before_conversion():
    body = _body(TAPE)
    assert "synth::time_from_epoch_ms(" in body
    assert not re.search(r"time_point\s*\(\s*std::chrono::milliseconds\s*\(", body), (
        "constructing the time point straight from milliseconds can "
        "overflow the clock's duration"
    )
