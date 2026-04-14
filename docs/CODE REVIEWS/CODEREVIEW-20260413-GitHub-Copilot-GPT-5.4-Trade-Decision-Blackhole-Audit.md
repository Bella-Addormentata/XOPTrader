# Trade Decision Blackhole Audit

Date: 2026-04-13

## Purpose

This document looks for decision-tree endpoints that are mathematically or
structurally unreachable under the current code and config.

A blackhole in this document means one of two things:

1. A trigger or block endpoint can never be reached in the live configuration.
2. A region of candidate space passes an earlier gate but can never reach the
   final trade trigger because a later gate always kills it.

These are the cases worth fixing, removing, or merging into another strategy so
the tree stays honest.

## Method

The audit uses:

1. Live config values from `config.yaml` and `buyer.yaml`.
2. Engine control flow in recovery, Step 9c, and Step 9e.
3. Direct arithmetic on the live buyer edge formula.

## Status Update

The static findings below were taken before the latest config cleanup.

Current live config has since resolved the major config-side branch blackholes:

1. Buyer is enabled.
2. Bid-side buyer coverage now exists on `XCH/wUSDC.b`.
3. `recovery.cancel_on_enter = false`, so `REC-T02` is no longer blocked by config.
4. `recovery.pair_allowlist = [XCH/wUSDC.b]`, so recovery no longer scans
   unsupported XCH-base pairs as if they were actionable.
5. Buyer and midpoint now derive a minimum actionable discount floor from live
   economics and treat `band_bps` / `midpoint_recycling_band_bps` as slack
   above that floor, so the old buyer dead-zone finding is obsolete.

Use `scripts/check_trade_blackholes.py` as the current source of truth. The
historical findings below were useful root-cause notes during rollout, but they
are no longer the current live status.

## Automated Check

Run the checker directly:

1. `py -3 scripts/check_trade_blackholes.py --config config.yaml --fail-on high`

Run the Windows wrapper that also appends to a log file:

1. `powershell -NoProfile -ExecutionPolicy Bypass -File packaging/windows/run_trade_blackhole_check.ps1 -RepoRoot C:\GitHub\XOPTrader -ConfigPath C:\GitHub\XOPTrader\config.yaml -LogPath C:\GitHub\XOPTrader\logs\trade-blackhole-check.log -FailOn high`

Recommended cadence:

1. Hourly while you are actively tuning strategy flags or buyer rules.
2. Daily once the configuration is stable.

Example Windows Task Scheduler commands:

1. Hourly: `schtasks /Create /SC HOURLY /MO 1 /TN "XOPTrader Trade Blackhole Check" /TR "powershell -NoProfile -ExecutionPolicy Bypass -File C:\GitHub\XOPTrader\packaging\windows\run_trade_blackhole_check.ps1 -RepoRoot C:\GitHub\XOPTrader -ConfigPath C:\GitHub\XOPTrader\config.yaml -LogPath C:\GitHub\XOPTrader\logs\trade-blackhole-check.log -FailOn high" /F`
2. Daily: `schtasks /Create /SC DAILY /ST 09:00 /TN "XOPTrader Trade Blackhole Check" /TR "powershell -NoProfile -ExecutionPolicy Bypass -File C:\GitHub\XOPTrader\packaging\windows\run_trade_blackhole_check.ps1 -RepoRoot C:\GitHub\XOPTrader -ConfigPath C:\GitHub\XOPTrader\config.yaml -LogPath C:\GitHub\XOPTrader\logs\trade-blackhole-check.log -FailOn high" /F`

## Current Result

Latest verified checker result against the live repo config:

1. `Status: PASS (high=0, medium=0, low=0)`
2. `No config-driven blackholes detected.`

This means the current live configuration does not leave any documented trade
branch structurally unreachable under the checker’s config-first model.

## Current Operational Constraints

These are still real implementation constraints, but they are intentional and
currently reflected by config rather than hidden blackholes:

1. Recovery is intentionally restricted to `XCH/wUSDC.b` through
   `recovery.pair_allowlist`.
2. Buyer currently supports only pairs involving XCH.
3. Buyer and midpoint now derive a minimum actionable discount floor from fees,
   buffers, and required edge, then use `band_bps` /
   `midpoint_recycling_band_bps` as slack above that floor.
4. Ask-side and bid-side buyer rules on the same pair still share cooldown and
   daily-cap state.

## Resolved Historical Issues

These were the major rollout blackholes that have now been fixed in the live
repo state:

1. Buyer disabled and shadowed behind `BUY-B01`: resolved by enabling buyer.
2. No bid-side buyer coverage: resolved by adding the `XCH/wUSDC.b` bid rule.
3. Same-block recovery entry blocked by config: resolved by setting
   `recovery.cancel_on_enter = false`.
4. Recovery scanning unsupported XCH-base pairs: resolved by restricting
   `recovery.pair_allowlist` to `XCH/wUSDC.b`.
5. Buyer dead-zone inside the nominal band: resolved by replacing the old hard
   band-plus-late-edge design with the actionable-window model.

## What The Checker Will Still Flag

The automated audit is now aligned with the actionable-window semantics. It
will raise findings when any of these regressions are introduced:

1. `buyer.band_bps <= 0` for an enabled rule.
2. `midpoint_recycling_band_bps <= 0` when midpoint recycling is enabled.
3. Unsupported pairs are added back into recovery or midpoint pair universes.
4. Buyer is reconfigured onto non-XCH pairs that the current engine cannot
   trade.

## Follow-Up Guidance

1. Keep the checker scheduled while strategy flags or buyer rules are being
   tuned.
2. Treat any new high-severity checker result as config drift first, then code
   drift if the config is unchanged.
3. Use the scenario runbook and decision-tree docs for runtime diagnosis; use
   this audit doc for config reachability only.
