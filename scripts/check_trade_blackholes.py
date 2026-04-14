#!/usr/bin/env python3
"""Audit config-driven trade scenario blackholes.

This checker is intentionally config-first. It flags settings that leave
documented trade decision branches permanently dormant or mathematically dead.
Exit code is non-zero when findings at or above --fail-on exist.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

import yaml


SEVERITY_ORDER = {"low": 1, "medium": 2, "high": 3}
SUPPORTED_RECOVERY_CEX_PAIRS = {"XCH/wUSDC.b"}
XCH_MOJOS = 1_000_000_000_000


@dataclass(slots=True)
class Finding:
    finding_id: str
    severity: str
    title: str
    summary: str
    scenario_ids: list[str] = field(default_factory=list)
    details: list[str] = field(default_factory=list)
    recommendation: str = ""


def load_yaml_file(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = yaml.safe_load(handle) or {}
    if not isinstance(data, dict):
        raise ValueError(f"Expected YAML mapping at {path}")
    return data


def deep_merge(base: dict[str, Any], overlay: dict[str, Any]) -> dict[str, Any]:
    merged = dict(base)
    for key, value in overlay.items():
        if isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = deep_merge(merged[key], value)
        else:
            merged[key] = value
    return merged


def resolve_buyer_config(root: dict[str, Any], config_path: Path) -> tuple[dict[str, Any], Path | None]:
    buyer = root.get("buyer")
    if not isinstance(buyer, dict):
        return {}, None

    merged_buyer = dict(buyer)
    config_ref = buyer.get("config_path")
    if isinstance(config_ref, str) and config_ref.strip():
        buyer_path = (config_path.parent / config_ref).resolve()
        if buyer_path.exists():
            external = load_yaml_file(buyer_path)
            external_body = external.get("buyer") if isinstance(external.get("buyer"), dict) else external
            if isinstance(external_body, dict):
                merged_buyer = deep_merge(merged_buyer, external_body)
            return merged_buyer, buyer_path
        return merged_buyer, buyer_path

    return merged_buyer, None


def enabled_pair_map(root: dict[str, Any]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    pairs = root.get("pairs")
    if not isinstance(pairs, list):
        return result
    for pair in pairs:
        if not isinstance(pair, dict):
            continue
        name = str(pair.get("name", "")).strip()
        if not name or not pair.get("enabled", False):
            continue
        result[name] = pair
    return result


def normalized_buyer_rules(buyer: dict[str, Any]) -> list[dict[str, Any]]:
    node = buyer.get("pair_rules")
    if not isinstance(node, list):
        node = buyer.get("pairs")
    if not isinstance(node, list):
        return []

    rules: list[dict[str, Any]] = []
    for raw_rule in node:
        if not isinstance(raw_rule, dict):
            continue
        pair_name = str(raw_rule.get("pair_name") or raw_rule.get("name") or "").strip()
        rules.append(
            {
                "pair_name": pair_name,
                "enabled": bool(raw_rule.get("enabled", True)),
                "side": str(raw_rule.get("side", "ask")).strip().lower() or "ask",
                "band_bps": float(raw_rule.get("band_bps", 30.0)),
                "min_edge_bps": float(raw_rule.get("min_edge_bps", 12.0)),
                "min_take_units": float(raw_rule.get("min_take_units", 0.05)),
                "max_take_units": float(raw_rule.get("max_take_units", 0.25)),
                "daily_cap_units": float(raw_rule.get("daily_cap_units", 5.0)),
                "max_premium_over_cex_bps": float(raw_rule.get("max_premium_over_cex_bps", 50.0)),
                "inventory_ratio_cap": float(raw_rule.get("inventory_ratio_cap", 0.65)),
            }
        )
    return rules


def add_finding(findings: list[Finding], **kwargs: Any) -> None:
    findings.append(Finding(**kwargs))


def analyze_config(root: dict[str, Any], config_path: Path, buyer_path: Path | None) -> list[Finding]:
    findings: list[Finding] = []
    pairs = enabled_pair_map(root)
    recovery = root.get("recovery") if isinstance(root.get("recovery"), dict) else {}
    arbitrage = root.get("arbitrage") if isinstance(root.get("arbitrage"), dict) else {}
    strategy = root.get("strategy") if isinstance(root.get("strategy"), dict) else {}
    fees = root.get("fees") if isinstance(root.get("fees"), dict) else {}
    buyer, _ = resolve_buyer_config(root, config_path)
    buyer_rules = [rule for rule in normalized_buyer_rules(buyer) if rule["enabled"]]

    if not pairs:
        add_finding(
            findings,
            finding_id="CFG-PAIR-01",
            severity="high",
            title="No enabled trading pairs",
            summary="The engine has no enabled pairs, so maker, recovery, and arbitrage paths stay dormant.",
            scenario_ids=["SYS-01", "SYS-02", "ARB-T01", "REC-T01"],
            recommendation="Enable at least one supported trading pair in config.yaml.",
        )

    if not bool(arbitrage.get("enabled", True)):
        add_finding(
            findings,
            finding_id="CFG-ARB-01",
            severity="high",
            title="Arbitrage master switch disabled",
            summary="The top-level arbitrage switch is off, so the crossed-book scenario tree cannot trigger.",
            scenario_ids=["ARB-T01", "ARB-T02"],
            recommendation="Set arbitrage.enabled=true if crossed-book trading should stay active.",
        )
    elif not bool(arbitrage.get("crossed_book_enabled", True)):
        add_finding(
            findings,
            finding_id="CFG-ARB-02",
            severity="high",
            title="Crossed-book arbitrage disabled",
            summary="Step 9c is configured off, so the crossed-book scenario family is dormant.",
            scenario_ids=["ARB-T01", "ARB-T02"],
            recommendation="Set arbitrage.crossed_book_enabled=true to keep Step 9c active.",
        )

    midpoint_enabled = bool(arbitrage.get("midpoint_recycling_enabled", False))
    midpoint_pairs_raw = arbitrage.get("midpoint_recycling_pairs")
    midpoint_pairs = []
    if isinstance(midpoint_pairs_raw, list):
        midpoint_pairs = [str(item).strip() for item in midpoint_pairs_raw if str(item).strip()]

    if not midpoint_enabled:
        add_finding(
            findings,
            finding_id="CFG-MID-01",
            severity="high",
            title="Midpoint recycling disabled",
            summary="The midpoint recycling branch is configured off, so Step 9d remains dormant.",
            scenario_ids=["MID-T01", "MID-B01"],
            recommendation="Set arbitrage.midpoint_recycling_enabled=true to keep Step 9d active.",
        )
    else:
        if not midpoint_pairs:
            add_finding(
                findings,
                finding_id="CFG-MID-02",
                severity="high",
                title="Midpoint recycling has no active pair universe",
                summary="midpoint_recycling_enabled=true but midpoint_recycling_pairs is empty, so Step 9d has no eligible pairs.",
                scenario_ids=["MID-T01", "MID-B08"],
                recommendation="Add at least one enabled XCH-base pair to arbitrage.midpoint_recycling_pairs.",
            )
        else:
            invalid_midpoint_pairs = [name for name in midpoint_pairs if name not in pairs]
            if invalid_midpoint_pairs:
                add_finding(
                    findings,
                    finding_id="CFG-MID-PAIR",
                    severity="high",
                    title="Midpoint recycling targets a disabled or unknown pair",
                    summary="At least one midpoint recycling pair is not enabled in config.yaml, so that branch cannot trigger for the configured target.",
                    scenario_ids=["MID-B08"],
                    details=[f"Unknown midpoint pair: {name}" for name in invalid_midpoint_pairs],
                    recommendation="Enable the midpoint pair in config.yaml or remove the orphaned midpoint target.",
                )

            unsupported_midpoint_pairs = [
                name for name in midpoint_pairs
                if name in pairs and name not in SUPPORTED_RECOVERY_CEX_PAIRS
            ]
            if unsupported_midpoint_pairs:
                add_finding(
                    findings,
                    finding_id="CFG-MID-ANCHOR",
                    severity="high",
                    title="Midpoint recycling scans pairs without a trusted anchor",
                    summary="The midpoint path currently depends on the same trusted external anchor surface as recovery, which only exists for XCH/wUSDC.b in live config.",
                    scenario_ids=["MID-B13"],
                    details=[f"Unsupported midpoint pair: {name}" for name in unsupported_midpoint_pairs],
                    recommendation="Restrict midpoint_recycling_pairs to trusted anchored pairs or add anchor support for the extra pairs.",
                )

            non_xch_base_midpoint_pairs = [
                name for name in midpoint_pairs
                if name in pairs and str(pairs[name].get("base_asset_id", "")).strip().lower() != "xch"
            ]
            if non_xch_base_midpoint_pairs:
                add_finding(
                    findings,
                    finding_id="CFG-MID-BASE",
                    severity="high",
                    title="Midpoint recycling targets a non-XCH-base pair",
                    summary="The current midpoint implementation is ask-side only on XCH-base pairs, so non-XCH-base pairs are unreachable.",
                    scenario_ids=["MID-B08"],
                    details=[f"Non-XCH-base midpoint pair: {name}" for name in non_xch_base_midpoint_pairs],
                    recommendation="Restrict midpoint_recycling_pairs to XCH-base pairs until bid-side or non-XCH-base midpoint support exists.",
                )

            midpoint_min_take_xch = float(arbitrage.get("midpoint_recycling_min_take_xch", 0.10))
            midpoint_band_bps = float(arbitrage.get("midpoint_recycling_band_bps", 20.0))
            midpoint_min_edge_bps = float(arbitrage.get("midpoint_recycling_min_expected_edge_bps", 5.0))
            midpoint_fee_buffer_bps = float(arbitrage.get("midpoint_recycling_fee_buffer_bps", 2.0))
            midpoint_toxicity_bps = float(arbitrage.get("midpoint_recycling_toxicity_buffer_bps", 6.0))
            midpoint_slippage_bps = float(arbitrage.get("midpoint_recycling_slippage_buffer_bps", 2.0))
            min_fee_mojos = float(fees.get("min_fee_mojos", 0.0))
            midpoint_fee_bps_roundtrip = 0.0
            if midpoint_min_take_xch > 0.0:
                midpoint_fee_bps_roundtrip = (
                    (2.0 * min_fee_mojos) / (midpoint_min_take_xch * XCH_MOJOS) * 10000.0
                )
            midpoint_effective_min_discount_bps = (
                midpoint_min_edge_bps
                + midpoint_fee_buffer_bps
                + midpoint_toxicity_bps
                + midpoint_slippage_bps
                + midpoint_fee_bps_roundtrip
            )
            if midpoint_band_bps <= 0.0:
                add_finding(
                    findings,
                    finding_id="CFG-MID-SLACK",
                    severity="high",
                    title="Midpoint discount slack collapsed",
                    summary="midpoint_recycling_band_bps must stay positive because midpoint now treats it as actionable slack above the derived minimum discount floor.",
                    scenario_ids=["MID-B19", "MID-T01"],
                    recommendation="Set midpoint_recycling_band_bps to a positive slack width above the derived minimum discount floor.",
                )

            maker_floor_units = float(strategy.get("min_offer_size_units", 0.1))
            if midpoint_min_take_xch < maker_floor_units:
                add_finding(
                    findings,
                    finding_id="BH-MID-RELIST-FLOOR",
                    severity="medium",
                    title="Midpoint takes can fall below the maker repost floor",
                    summary=f"midpoint_recycling_min_take_xch={midpoint_min_take_xch:.4f} is below strategy.min_offer_size_units={maker_floor_units:.4f}, so small midpoint fills may not re-enter the normal maker ladder until inventory aggregates.",
                    scenario_ids=["MID-T01"],
                    recommendation="Raise midpoint_recycling_min_take_xch to the maker floor or lower strategy.min_offer_size_units if intentional dust recycling is desired.",
                )

    if not bool(recovery.get("enabled", True)):
        add_finding(
            findings,
            finding_id="CFG-REC-01",
            severity="high",
            title="Recovery mode disabled",
            summary="The XCH recovery branch is turned off, so recovery scenarios cannot trigger.",
            scenario_ids=["REC-T01", "REC-T02"],
            recommendation="Set recovery.enabled=true if XCH recovery should remain active.",
        )
    else:
        if bool(recovery.get("cancel_on_enter", True)):
            add_finding(
                findings,
                finding_id="BH-REC-ENTRY",
                severity="medium",
                title="Same-block recovery entry take disabled",
                summary="recovery.cancel_on_enter=true makes REC-T02 unreachable because entry blocks defer acquisition until the next block.",
                scenario_ids=["REC-T02"],
                recommendation="Set recovery.cancel_on_enter=false if same-block recovery entry takes should stay live.",
            )

        xch_base_pairs = [name for name in pairs if name.startswith("XCH/")]
        allowlist_raw = recovery.get("pair_allowlist")
        allowlist = []
        if isinstance(allowlist_raw, list):
            allowlist = [str(item).strip() for item in allowlist_raw if str(item).strip()]

        evaluated_pairs = [name for name in xch_base_pairs if not allowlist or name in allowlist]
        unsupported_pairs = [name for name in evaluated_pairs if name not in SUPPORTED_RECOVERY_CEX_PAIRS]
        if unsupported_pairs:
            add_finding(
                findings,
                finding_id="BH-REC-ANCHOR",
                severity="high",
                title="Recovery scans pairs without a trusted anchor",
                summary="Recovery only has a trusted external anchor for XCH/wUSDC.b, so other evaluated XCH-base pairs become structurally unreachable take paths.",
                scenario_ids=["REC-B05"],
                details=[f"Unsupported recovery pair: {name}" for name in unsupported_pairs],
                recommendation="Restrict recovery.pair_allowlist to trusted pairs or add anchor support for the extra pairs.",
            )

    buyer_enabled = bool(buyer.get("enabled", False))
    if not buyer_enabled:
        add_finding(
            findings,
            finding_id="BH-BUY-01",
            severity="high",
            title="Buyer master switch disabled",
            summary="buyer.enabled=false leaves the buyer subtree dormant and shadows most BUY-* scenarios behind BUY-B01.",
            scenario_ids=["BUY-T01", "BUY-T02", "BUY-B01"],
            details=[f"Inline buyer config path: {buyer_path}" if buyer_path else "Buyer uses inline config only."],
            recommendation="Set buyer.enabled=true in config.yaml or buyer.yaml to activate Step 9e.",
        )
    else:
        if not buyer_rules:
            add_finding(
                findings,
                finding_id="BH-BUY-02",
                severity="high",
                title="Buyer enabled without active rules",
                summary="Step 9e is turned on but has no enabled pair rules, so buyer triggers remain unreachable.",
                scenario_ids=["BUY-T01", "BUY-T02"],
                recommendation="Add at least one enabled buyer pair rule.",
            )
        else:
            ask_rules = [rule for rule in buyer_rules if rule["side"] == "ask"]
            bid_rules = [rule for rule in buyer_rules if rule["side"] == "bid"]
            if not ask_rules:
                add_finding(
                    findings,
                    finding_id="BH-BUY-ASK",
                    severity="high",
                    title="No ask-side buyer rule",
                    summary="BUY-T01 cannot trigger because no enabled ask-side buyer rule exists.",
                    scenario_ids=["BUY-T01"],
                    recommendation="Add at least one enabled buyer rule with side=ask.",
                )
            if not bid_rules:
                add_finding(
                    findings,
                    finding_id="BH-BUY-BID",
                    severity="medium",
                    title="No bid-side buyer rule",
                    summary="BUY-T02 cannot trigger because no enabled bid-side buyer rule exists.",
                    scenario_ids=["BUY-T02"],
                    recommendation="Add at least one enabled buyer rule with side=bid if sell-side buyer coverage is desired.",
                )

            for rule in buyer_rules:
                pair_name = rule["pair_name"]
                side_tag = str(rule["side"]).upper()
                if pair_name not in pairs:
                    add_finding(
                        findings,
                        finding_id="BH-BUY-PAIR",
                        severity="medium",
                        title="Buyer rule targets a disabled or unknown pair",
                        summary=f"Buyer rule {pair_name} is enabled but the pair is not enabled in config.yaml.",
                        scenario_ids=["BUY-B10", "BUY-B13"],
                        recommendation="Enable the pair in config.yaml or remove the orphaned buyer rule.",
                    )
                    continue

                pair_cfg = pairs[pair_name]
                base_asset_id = str(pair_cfg.get("base_asset_id", ""))
                if base_asset_id != "xch":
                    continue

                min_take_units = max(rule["min_take_units"], 0.0)
                if min_take_units <= 0.0:
                    continue

                min_fee_mojos = int(fees.get("min_fee_mojos", 5000))
                maker_min_margin_bps = float(strategy.get("min_profit_margin_bps", 35.0))
                relist_fill_probability = float(buyer.get("relist_fill_probability", 0.5))
                include_relist_credit = bool(buyer.get("include_relist_credit", True))
                toxicity_buffer_bps = float(buyer.get("toxicity_buffer_bps", 8.0))
                slippage_buffer_bps = float(buyer.get("slippage_buffer_bps", 3.0))

                notional_mojos = min_take_units * XCH_MOJOS
                fee_bps_per_leg = (min_fee_mojos / notional_mojos) * 10000.0
                relist_credit_bps = (
                    maker_min_margin_bps * relist_fill_probability
                    if include_relist_credit
                    else 0.0
                )
                effective_min_discount_bps = max(
                    0.0,
                    rule["min_edge_bps"]
                    + toxicity_buffer_bps
                    + slippage_buffer_bps
                    + (2.0 * fee_bps_per_leg)
                    - relist_credit_bps,
                )

                if rule["band_bps"] <= 0.0:
                    add_finding(
                        findings,
                        finding_id=f"CFG-BUY-SLACK-{side_tag}",
                        severity="high",
                        title="Buyer discount slack collapsed",
                        summary=f"{pair_name} {rule['side']}-side buyer now treats band_bps as actionable slack above the derived minimum discount floor, so non-positive values make the window inert.",
                        scenario_ids=["BUY-B23", "BUY-T01" if rule["side"] == "ask" else "BUY-T02"],
                        recommendation="Set band_bps to a positive slack width above the derived minimum discount floor.",
                    )

    return findings


def summarize(findings: list[Finding]) -> dict[str, int]:
    counts = {"high": 0, "medium": 0, "low": 0}
    for finding in findings:
        counts[finding.severity] += 1
    return counts


def overall_status(findings: list[Finding]) -> str:
    counts = summarize(findings)
    if counts["high"]:
        return "FAIL"
    if counts["medium"] or counts["low"]:
        return "WARN"
    return "PASS"


def should_fail(findings: list[Finding], fail_on: str) -> bool:
    if fail_on == "none":
        return False
    threshold = SEVERITY_ORDER[fail_on]
    return any(SEVERITY_ORDER[finding.severity] >= threshold for finding in findings)


def print_text_report(
    findings: list[Finding],
    config_path: Path,
    buyer_path: Path | None,
) -> None:
    counts = summarize(findings)
    print("XOPTrader trade blackhole audit")
    print(f"Config: {config_path}")
    print(f"Buyer config: {buyer_path if buyer_path else 'inline only'}")
    print(
        "Status: "
        f"{overall_status(findings)} "
        f"(high={counts['high']}, medium={counts['medium']}, low={counts['low']})"
    )

    if not findings:
        print("No config-driven blackholes detected.")
        return

    for finding in findings:
        print()
        print(f"[{finding.severity.upper()}] {finding.finding_id} - {finding.title}")
        print(f"  {finding.summary}")
        if finding.scenario_ids:
            print(f"  Scenarios: {', '.join(finding.scenario_ids)}")
        for detail in finding.details:
            print(f"  Detail: {detail}")
        if finding.recommendation:
            print(f"  Action: {finding.recommendation}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="config.yaml", help="Path to config.yaml")
    parser.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
        help="Output format",
    )
    parser.add_argument(
        "--fail-on",
        choices=("none", "medium", "high"),
        default="high",
        help="Return non-zero when findings at or above this severity exist",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    config_path = Path(args.config).resolve()
    if not config_path.exists():
        print(f"Config not found: {config_path}", file=sys.stderr)
        return 1

    try:
        root = load_yaml_file(config_path)
        buyer, buyer_path = resolve_buyer_config(root, config_path)
        if buyer:
            root = dict(root)
            root["buyer"] = buyer
        findings = analyze_config(root, config_path, buyer_path)
    except Exception as exc:  # noqa: BLE001
        print(f"Blackhole audit failed: {exc}", file=sys.stderr)
        return 1

    payload = {
        "status": overall_status(findings),
        "config_path": str(config_path),
        "buyer_config_path": str(buyer_path) if buyer_path else None,
        "summary": summarize(findings),
        "findings": [asdict(finding) for finding in findings],
    }

    if args.format == "json":
        print(json.dumps(payload, indent=2))
    else:
        print_text_report(findings, config_path, buyer_path)

    return 2 if should_fail(findings, args.fail_on) else 0


if __name__ == "__main__":
    raise SystemExit(main())