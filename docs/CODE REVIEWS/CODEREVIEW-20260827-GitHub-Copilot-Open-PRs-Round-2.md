# Open Pull Requests Review - Round 2

**Review date:** 2026-08-27
**Repository:** `Bella-Addormentata/XOPTrader`
**Reviewer:** GitHub Copilot

This is a fresh review of every open pull request after fetching all remote
refs. It supersedes the earlier review only for the head SHAs listed below.

## GitHub snapshot

| PR | Head | Base | Scope | GitHub status | Recommendation |
| --- | --- | --- | --- | --- | --- |
| [#115](https://github.com/Bella-Addormentata/XOPTrader/pull/115) Peg registry | `54b2caf` | `main` | 10 files, +1346/-62 | Open, clean, mergeable; checks green | Changes requested |
| [#116](https://github.com/Bella-Addormentata/XOPTrader/pull/116) Permuto research | `f36ffff` | `main` | 5 files, +1918/-0 | Open, clean, mergeable; checks green | Changes requested |
| [#117](https://github.com/Bella-Addormentata/XOPTrader/pull/117) S27 equity blindness | `5c69390` | `feat/peg-registry` | 7 files, +1460/-14 | Open, clean, mergeable; checks green | Changes requested |
| [#118](https://github.com/Bella-Addormentata/XOPTrader/pull/118) Emergency consolidation planner | `7293e83` | `main` | 6 files, +1000/-1 | Open, clean, mergeable; checks green | Changes requested before wiring execution |
| [#119](https://github.com/Bella-Addormentata/XOPTrader/pull/119) Permuto identity | `93d06a3` | `main` | 12 files, +1950/-7 | Open, clean, mergeable; checks green | **Do not merge** |

All five report successful C++ compilation/tests, Python syntax/tests, and GUI
lock installation on Ubuntu, Windows, and macOS. The findings below exercise
paths not covered by those checks.

## Executive findings

1. **Critical, #119:** a normal Settings save copies the DPAPI-wrapped Permuto
   private key into tracked `config.yaml` because `permuto` is absent from the
   secret split policy. This was reproduced with the selected VS Code Python
   interpreter.
2. **High, #119:** closing the one-time recovery dialog before confirmation
   leaves the identity persisted while discarding its only displayed mnemonic;
   the page later allows the operator to assert that it was backed up.
3. **High, #119:** registration is permanent before leaderboard lookup, but a
   lookup failure produces only `{ok: false}` and persists no registration
   metadata, inviting the operator to retry a link that already succeeded.
4. **High, #117:** the new external XCH/USD source is not a direct XCH pricing
   path. If XCH pairs are disabled or temporarily ungradable, held XCH can be
   written off or treated as unpriced despite a fresh CoinGecko value.
5. **Medium, #118:** one tiny direct offer prevents use of a viable two-hop
   route, contradicting the stated goal of moving as much balance as possible.
6. **Medium, #118:** the same slippage cap is applied independently to each hop;
   two accepted 10% legs can produce 20.88% end-to-end slippage.
7. **Medium, #116:** the analyzer merges appended probe sessions and treats
   unobserved gaps as continuous evidence. A synthetic two-session file was
   incorrectly reported as `VERDICT: CONFIRMED`.
8. **High, #115:** the new registry is optional, but the existing tracked live
   configuration is not migrated. Merging #115 alone can remove every old
   symbol-based USD anchor on upgrade.

## PR #115 - Peg registry

### 115-1 - High: existing configurations silently lose their peg anchors

**Locations:**
[`cpp/src/config.cpp:416-420`](https://github.com/Bella-Addormentata/XOPTrader/blob/54b2caf749976dd62e148905ffca654c8e46e6e8/cpp/src/config.cpp#L416-L420),
[`cpp/src/engine.cpp:11288-11340`](https://github.com/Bella-Addormentata/XOPTrader/blob/54b2caf749976dd62e148905ffca654c8e46e6e8/cpp/src/engine.cpp#L11288-L11340)

An absent `pegged_assets` section is accepted as an empty registry. Existing
deployments were created before that section existed, and this PR deliberately
does not update the tracked live `config.yaml`. The replacement engine code no
longer recognizes the legacy stable symbols, so `is_par_wrapper_quote()` is
false for every pair and `usd_per_xch()` can return zero. That collapses quote
conversion and equity protection during the upgrade that is intended to make
those paths safer.

**Suggested change:** add an explicit migration instead of accepting this state
silently. Reasonable options are:

- synthesize legacy declarations once from known canonical asset IDs and emit a
  deprecation warning;
- migrate `config.yaml` before deploying the binary; or
- reject startup when enabled valuation pairs require a USD anchor but no usable
  declaration exists.

Add an upgrade test that loads a pre-PR configuration and proves it either
migrates safely or fails with an actionable message. Do not merge #115 alone
while its normal predecessor configuration boots with an empty registry.

### 115-2 - Medium: currency codes are accepted case-sensitively

**Locations:**
[`cpp/src/config.cpp:461`](https://github.com/Bella-Addormentata/XOPTrader/blob/54b2caf749976dd62e148905ffca654c8e46e6e8/cpp/src/config.cpp#L461),
[`cpp/include/xop/peg_registry.hpp:253`](https://github.com/Bella-Addormentata/XOPTrader/blob/54b2caf749976dd62e148905ffca654c8e46e6e8/cpp/include/xop/peg_registry.hpp#L253)

The parser accepts arbitrary non-empty `peg_currency`, while `usd_par_value()`
recognizes only exact `"USD"`. `peg_currency: usd` is therefore accepted but
behaves like an unsupported foreign currency and silently removes the par.

**Suggested change:** trim and uppercase ISO-style codes in the parser, or
reject anything outside the documented canonical format. Add `usd`, mixed-case,
and whitespace tests.

### 115-3 - Low: production valuation wiring still lacks regression tests

The registry and parser tests are extensive, but no test invokes
`usd_per_xch()`, `market_cross_for()`, `quote_usd_factor()`, or
`asset_usd_pseudo_price()`. The recent fixes for non-unit par conversion and
cross orientation could regress while all current tests remain green.

Add Engine-boundary tests for direct and inverse crosses, unavailable FX,
non-unit par, `enforce: false`, and multiple eligible cross candidates.

**Re-review result:** duplicate declarations, asset-ID canonicalization,
inclusive thresholds, non-finite values, dead APIs, cross orientation, and
declared-par conversion from earlier review rounds are fixed at this head.

## PR #116 - Permuto research and probes

### 116-1 - Medium: appended probe runs are analyzed as one continuous window

**Locations:**
[`scripts/permuto_depth_probe.py:94`](https://github.com/Bella-Addormentata/XOPTrader/blob/f36ffffc0e5c4a0696070eaa6eae2bbaf6bc25b8/scripts/permuto_depth_probe.py#L94),
[`scripts/permuto_depth_analyze.py:29-42`](https://github.com/Bella-Addormentata/XOPTrader/blob/f36ffffc0e5c4a0696070eaa6eae2bbaf6bc25b8/scripts/permuto_depth_analyze.py#L29-L42),
[`scripts/permuto_depth_analyze.py:52-70`](https://github.com/Bella-Addormentata/XOPTrader/blob/f36ffffc0e5c4a0696070eaa6eae2bbaf6bc25b8/scripts/permuto_depth_analyze.py#L52-L70)

The probe appends and writes a new `probe_start` header each time. `load()`
keeps every data row across all headers, then the analyzer compares the first
and final rows and checks only observed oracle values. Process downtime or a
restart is therefore counted as observed carried time.

This was reproduced with two one-row sessions an hour apart, the same observed
oracle on both sides, and an unobserved depth increase in the gap. The analyzer
reported:

```text
oracle       : FROZEN throughout (carried), 2 samples
VERDICT: CONFIRMED. 1 of 1 accounts gained depth while the oracle was frozen.
```

**Suggested change:** analyze exactly one session. Reset `rows` when a new
header is read, or reject files with multiple headers unless a session is
selected explicitly. Also require adjacent timestamps to stay within a small
multiple of `interval_s`; observed equality on both sides of a gap does not
prove the oracle stayed frozen inside it.

### 116-2 - Medium: the documented across-close run cannot confirm its claim

The probe says to sample before the close until well after it, but the analyzer
requires one distinct oracle value over the entire file. Any normal pre-close
oracle movement makes `frozen` false, while the first-to-last depth delta still
includes live-session accrual.

**Suggested change:** identify the longest complete frozen suffix after the
last oracle transition, then choose both leaderboard endpoints from that suffix.
Alternatively accept an explicit carried-window start. Require complete oracle
coverage and bounded sample gaps inside the selected window.

### 116-3 - Low: leaderboard sampling has a silent 100-row ceiling

**Location:**
[`scripts/permuto_depth_probe.py:59-76`](https://github.com/Bella-Addormentata/XOPTrader/blob/f36ffffc0e5c4a0696070eaa6eae2bbaf6bc25b8/scripts/permuto_depth_probe.py#L59-L76)

The documentation emphasizes pagination, but the probe requests `limit=100`
once and records `market_makers_total` without checking it. Page past the first
100 when the total is larger. Store full user IDs internally as well; eight
characters are suitable for display, not identity joins.

### 116-4 - Low: current documents still contradict their own corrections

The current head still contains all of these statements:

- [`TODO-COMPETITION.md:220`](https://github.com/Bella-Addormentata/XOPTrader/blob/f36ffffc0e5c4a0696070eaa6eae2bbaf6bc25b8/TODO-COMPETITION.md#L220)
  calls carried accrual unresolved before later sections call it confirmed.
- [`docs/advanced-trading-methods.md:335-336`](https://github.com/Bella-Addormentata/XOPTrader/blob/f36ffffc0e5c4a0696070eaa6eae2bbaf6bc25b8/docs/advanced-trading-methods.md#L335-L336)
  says no hedge exists and positions can only be flattened on Permuto, before
  later text correctly scopes that claim to current/on-venue capabilities.
- [`docs/advanced-trading-methods.md:479`](https://github.com/Bella-Addormentata/XOPTrader/blob/f36ffffc0e5c4a0696070eaa6eae2bbaf6bc25b8/docs/advanced-trading-methods.md#L479)
  again declares the oracle a jump process after explaining that estimator
  discontinuities do not prove jumps in underlying returns.
- The PR description still says "No code" and "Three documents" although the
  diff now includes two executable scripts.

Remove the superseded claims rather than leaving the correction and the old
operational conclusion active simultaneously.

## PR #117 - S27 equity blindness

### 117-1 - High: fresh external XCH/USD is not a direct XCH valuation path

**Locations:**
[`cpp/src/engine.cpp:11695-11713`](https://github.com/Bella-Addormentata/XOPTrader/blob/5c69390cd40a19c20353d837d2c799267b34335a/cpp/src/engine.cpp#L11695-L11713),
[`cpp/src/engine.cpp:11715-11758`](https://github.com/Bella-Addormentata/XOPTrader/blob/5c69390cd40a19c20353d837d2c799267b34335a/cpp/src/engine.cpp#L11715-L11758)

`usd_per_xch()` now correctly prefers a freshness-gated CoinGecko value, but
`asset_usd_pseudo_price("xch")` only calls conversion logic while iterating an
enabled pair. `asset_has_pricing_path()` likewise recognizes only enabled pairs.
If all XCH pairs are disabled, held XCH is written off at zero despite a fresh
external price. Even with an enabled pair, a missing/ungraded pair snapshot can
hide the independent XCH feed.

That defeats the purpose of making external XCH/USD the anchor that survives
disabled or compromised quote-asset markets.

**Suggested change:** price XCH directly before pair traversal:

```cpp
if (asset_id == "xch") {
    const double usd = usd_per_xch();
    if (std::isfinite(usd) && usd > 0.0) {
        return static_cast<Mojo>(std::llround(
            usd * static_cast<double>(kMojosPerXch)));
    }
}
```

Teach `asset_has_pricing_path()` that XCH has a configured external route when
CoinGecko is enabled and `coin_ids` contains `chia`; freshness remains a runtime
availability decision. Add tests for no enabled XCH pair, an ungraded XCH pair,
fresh external data, and stale external data with/without a DEX fallback.

### 117-2 - Medium: the safety composition still has no Engine-level test

The pure predicates now have substantial tests, but the production interaction
among `asset_usd_pseudo_price`, carry timestamps, `valuation_degraded_`,
`valuation_all_unpriced_`, the authority gate, grace decrement, and the pause
latch remains untested. The number of review-driven fixes in this exact path is
evidence that isolated predicates are insufficient.

Add a narrow Engine fixture or extract one state-transition function receiving
per-asset states (`live`, `fresh carry`, `expired carry`, `no configured path`).
Test mixed assets, all written off, all expired, one fresh carry plus one
expired carry, TTL zero, and XCH's external route.

**Re-review result:** the stale CoinGecko cache, zero-HWM fail-open, grace
off-by-one, all-unpriced-after-peak case, transient one-tick latch, and S33
cross-asset carry issue from earlier rounds are fixed at this head.

## PR #118 - Emergency consolidation planner

### 118-1 - Medium: any tiny direct fill suppresses a viable two-hop route

**Locations:**
[`gui/services/consolidate/planner.py:364-370`](https://github.com/Bella-Addormentata/XOPTrader/blob/7293e8334ea16095daef0511cc6f569cc6ad1243/gui/services/consolidate/planner.py#L364-L370),
[`gui/services/consolidate/planner.py:404-422`](https://github.com/Bella-Addormentata/XOPTrader/blob/7293e8334ea16095daef0511cc6f569cc6ad1243/gui/services/consolidate/planner.py#L404-L422)

The function returns as soon as the direct route selects one offer, regardless
of coverage. A reproduced plan with a 1-unit direct offer and a complete
two-hop route returned one direct leg and left 999 of 1,000 source units
untouched. That conflicts with the feature promise to move as much balance as
possible and gives the caller no way to choose the safer-versus-complete tradeoff.

**Suggested change:** compute direct and two-hop candidates independently and
return both, or make route policy explicit (`prefer_direct`, `maximize_coverage`,
or a minimum direct coverage threshold). At minimum, do not let dust liquidity
silently make the only viable consolidation route unreachable.

### 118-2 - Medium: per-leg caps compound beyond the operator's route cap

**Locations:**
[`gui/services/consolidate/planner.py:433-457`](https://github.com/Bella-Addormentata/XOPTrader/blob/7293e8334ea16095daef0511cc6f569cc6ad1243/gui/services/consolidate/planner.py#L433-L457)

The same `max_slippage_frac` is independently applied to both hops. A concrete
10% test accepted both legs while the combined give/receive rate was 20.88%
worse than the product of the two anchors. In general the route bound is
$(1+s)^2-1$, not $s$.

**Suggested change:** either label the input and future UI as a per-leg cap and
show the larger worst-case route bound, or enforce the operator's cap on the
composite route rate. Add a regression using two near-cap legs.

### 118-3 - Medium: duplicate offer IDs can be planned more than once

**Location:**
[`gui/services/consolidate/planner.py:273-317`](https://github.com/Bella-Addormentata/XOPTrader/blob/7293e8334ea16095daef0511cc6f569cc6ad1243/gui/services/consolidate/planner.py#L273-L317)

Shape validation requires a non-empty ID but not a unique one. Duplicate API
rows therefore produce duplicate takes in the plan; the first execution consumes
the offer and the second fails after the plan has partially executed.

**Suggested change:** reject repeated IDs while normalizing candidates, ideally
across the entire plan, and expose a duplicate diagnostic. Add same-instance and
distinct-instance/same-ID tests.

### 118-4 - Low: provenance accepts invisible text

`Anchor(source="   ")` passes validation even though the confirmation surface
cannot show meaningful provenance. Require `source.strip()` and test whitespace.

**Validation:** the current head's 37 focused tests pass and the package compiles.
The reproduced route cases above are not covered by those tests.

## PR #119 - Permuto identity

### 119-1 - Critical: Settings writes the wrapped private key to tracked config

**Locations:**
[`gui/services/permuto/identity.py:218-237`](https://github.com/Bella-Addormentata/XOPTrader/blob/93d06a39172fb635d20362c8f607343589d0b0f9/gui/services/permuto/identity.py#L218-L237),
[`gui/services/config_split.py:302-342`](https://github.com/Bella-Addormentata/XOPTrader/blob/93d06a39172fb635d20362c8f607343589d0b0f9/gui/services/config_split.py#L302-L342),
[`gui/services/config_split.py:371-406`](https://github.com/Bella-Addormentata/XOPTrader/blob/93d06a39172fb635d20362c8f607343589d0b0f9/gui/services/config_split.py#L371-L406),
[`gui/widgets/settings.py:3522`](https://github.com/Bella-Addormentata/XOPTrader/blob/93d06a39172fb635d20362c8f607343589d0b0f9/gui/widgets/settings.py#L3522)

`PermutoIdentity.create()` stores `bls_private_key_dpapi` in `secrets.yaml`, but
the `permuto` section is absent from both `SECRET_KEYS` and
`WALLET_MANAGED_KEYS`. Settings loads a merged snapshot and writes every unknown
section as public configuration. `config.yaml` is tracked.

The selected VS Code interpreter reproduced the leak:

```text
SECRET_IN_PUBLIC= True
permuto:
  bls_private_key_dpapi: SECRET_BLOB
  bls_public_key: PUB
```

The ciphertext is machine/user-bound, but it is still persistent key material,
can be committed, and can resurrect stale identity state if the secret overlay
is later missing. This violates the project's established key isolation.

**Suggested change:** add every identity-owned `permuto` field to
`SECRET_KEYS`, and add the complete section to `WALLET_MANAGED_KEYS` so a stale
Settings snapshot can neither publish nor overwrite it. Add a regression that
loads merged config containing a Permuto identity, calls `split_and_save()`, and
asserts no `permuto` key material appears in `config.yaml` while the current
on-disk secret remains unchanged.

### 119-2 - High: closing the recovery dialog can create an unrecoverable account

**Locations:**
[`gui/widgets/permuto.py:149-167`](https://github.com/Bella-Addormentata/XOPTrader/blob/93d06a39172fb635d20362c8f607343589d0b0f9/gui/widgets/permuto.py#L149-L167),
[`gui/widgets/permuto.py:480-492`](https://github.com/Bella-Addormentata/XOPTrader/blob/93d06a39172fb635d20362c8f607343589d0b0f9/gui/widgets/permuto.py#L480-L492)

The identity is persisted before the modal opens. Disabling OK does not disable
Escape or the window close button; either calls `done(Rejected)`. The phrase is
then lost, Create is disabled because the key exists, and the page-level backup
checkbox can later mark the unrecoverable key as backed up.

**Suggested change:** either refuse rejection/close until confirmation, or roll
back a newly created, unregistered identity on cancellation so Create can safely
start over. Test Escape, title-bar close, application shutdown, and confirmed OK.

### 119-3 - High: successful linking is lost when read-back fails

**Location:**
[`gui/widgets/permuto.py:227-250`](https://github.com/Bella-Addormentata/XOPTrader/blob/93d06a39172fb635d20362c8f607343589d0b0f9/gui/widgets/permuto.py#L227-L250)

`auth.register()` performs the permanent link, then the worker calls the public
leaderboard before emitting success. If that second request fails, the single
outer `except` emits only `{ok: false}`. `_on_finished()` persists nothing and
reenables Register even though the venue account already exists.

The selected interpreter reproduced exactly this result after a successful
mock link and failed lookup:

```text
[{'error': 'leaderboard unavailable', 'ok': False}]
```

**Suggested change:** persist `registered`, `user_id`, and `trading_address`
immediately after `auth.register()` returns, before optional verification. Catch
leaderboard failure separately and report a durable amber linked/unverified
state. Add a test proving a post-link timeout never re-enables registration.

### 119-4 - Medium: the hidden dialog retains the mnemonic

**Locations:**
[`gui/widgets/permuto.py:74-147`](https://github.com/Bella-Addormentata/XOPTrader/blob/93d06a39172fb635d20362c8f607343589d0b0f9/gui/widgets/permuto.py#L74-L147),
[`gui/widgets/permuto.py:486-492`](https://github.com/Bella-Addormentata/XOPTrader/blob/93d06a39172fb635d20362c8f607343589d0b0f9/gui/widgets/permuto.py#L486-L492)

`exec()` hides this parented dialog; it does not destroy it. The QTextEdit and
copy-button lambda continue retaining the full phrase as children of the page,
contradicting the claim that it goes out of scope after creation.

**Suggested change:** capture only the confirmation result, clear the text and
disconnect phrase-capturing callbacks, then `deleteLater()` the dialog. Test that
no four-word phrase fragment remains in child widget text after completion.

### 119-5 - Low: a temporary leaderboard miss visibly retracts green status

`mark_registered()` correctly refuses to downgrade persisted
`listing_verified=True`, but `_on_finished()` overwrites the refreshed green
status with amber whenever the current lookup returns no row. Preserve the
durable verified status while reporting the transient lookup separately.

**Validation:** all 43 Permuto tests pass. Pylance reports no type errors in
`identity.py` or `config_split.py`; it reports only unused-symbol hints in
`auth.py` and `permuto.py`. Those tests do not cover findings 119-1 through
119-4.

## Recommended order

1. Block #119 until secret splitting and recovery cancellation are fixed; then
   fix post-link persistence before any operator registers a real identity.
2. Fix #115's upgrade path before merging it into `main`.
3. Fix #117's direct XCH valuation and merge it only after the corrected #115.
4. Clarify and enforce #118's route policy and route-level risk before wiring
   the planner to execution.
5. Repair #116's session selection and stale documentation before relying on
   its `CONFIRMED` measurement.

## Validation performed

- Fetched all remotes and resolved open PR heads through GitHub and
  `refs/pull/*/head`.
- Confirmed all five PRs are open, non-draft, clean/mergeable, and green in the
  reported GitHub checks.
- Reviewed each diff against its declared base, including #117 as a stacked PR.
- Ran #118 tests in a detached temporary worktree: **37 passed**; Python syntax
  compilation passed.
- Ran #119 focused tests: **43 passed**.
- Ran Pylance diagnostics on #119 core files: no type errors; three unused-name
  hints only.
- Reproduced #116 multi-session false confirmation, #118 route-selection and
  compounded-slippage behavior, and #119 secret leakage/post-link failure.
- Ruff is configured but not installed in the active interpreter, so a local
  Ruff run was not available. GitHub's reported checks do not include Ruff.

No PR branch was checked out for editing. Temporary detached worktrees used for
tests were removed, and unrelated untracked workspace files were not modified.