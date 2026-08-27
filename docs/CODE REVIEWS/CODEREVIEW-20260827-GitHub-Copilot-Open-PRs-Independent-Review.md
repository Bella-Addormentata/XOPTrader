# Independent Review: Open Pull Requests #115, #116, and #117

**Review date:** 2026-08-27
**Repository:** `Bella-Addormentata/XOPTrader`
**Reviewer:** GitHub Copilot
**Source state:** `git fetch --all --prune`, GitHub REST API, GitHub check API,
and exact local Git diffs at the head SHAs below.

## Current GitHub state

| PR | Head SHA | Base | Diff | GitHub state | Recommendation |
| --- | --- | --- | --- | --- | --- |
| [#115](https://github.com/Bella-Addormentata/XOPTrader/pull/115) Peg registry | `12d045d` | `main` | 10 files, +989/-26 | Open, non-draft, mergeable/rebaseable, clean; checks green | Fix the two config-identity defects before merge |
| [#116](https://github.com/Bella-Addormentata/XOPTrader/pull/116) Permuto research | `6e255cd` | `main` | 3 files, +1045/-0 | Open, non-draft, mergeable/rebaseable, clean; checks green | Documentation-only; correct unsupported conclusions and API contradictions |
| [#117](https://github.com/Bella-Addormentata/XOPTrader/pull/117) S27 equity blindness | `3d8e186` | `feat/peg-registry` (#115) | 2 files, +112/-10 | Open, non-draft, mergeable/rebaseable, clean; checks green | **Do not merge yet:** two high-severity fail-open paths remain |

The reported successful checks cover the C++ compile/test job, Python syntax
and tests, and GUI lock installation on Ubuntu, Windows, and macOS. Green CI
does not exercise either #117 production failure below; the PR explicitly says
its new tests do not reach the engine path.

## PR #117: S27 equity blindness

### 117-1 - High: a stale CoinGecko value suppresses the DEX fallback forever

**Locations:**
[`cpp/src/engine.cpp:11326-11329`](https://github.com/Bella-Addormentata/XOPTrader/blob/3d8e186b591e0d33f31a67a7968839d031a8bd36/cpp/src/engine.cpp#L11326-L11329),
[`cpp/src/engine.cpp:1995-2009`](https://github.com/Bella-Addormentata/XOPTrader/blob/3d8e186b591e0d33f31a67a7968839d031a8bd36/cpp/src/engine.cpp#L1995-L2009)

`usd_per_xch()` accepts any finite positive cached `coingecko_prices_["chia"]`.
On a failed fetch, Step 1 deliberately retains the old map and does not advance
`coingecko_last_fetch_`. The new code ignores that timestamp, so an hours- or
days-old price continues to win and the advertised DEX fallback is unreachable.
Because the result remains positive, portfolio valuation also refreshes
`last_asset_live_block_` every heartbeat and never declares the frozen source
degraded.

**Suggested change:** gate the cache on the age of the last successful fetch.
The repository already has `coingecko_feed_fresh_for_revival()` with the right
timestamp arithmetic. Either generalize/rename that helper or add an equivalent
valuation helper. Define `cex_freshness_threshold_sec <= 0` explicitly: for this
risk path it should either reject startup configuration or make CoinGecko
unavailable and continue to the DEX fallback, never mean "fresh forever."

```cpp
const bool coingecko_fresh = coingecko_feed_fresh_for_revival(
    !coingecko_prices_.empty(),
    coingecko_last_fetch_,
    std::chrono::steady_clock::now(),
    config_.market_data.cex_freshness_threshold_sec);

if (coingecko_fresh) {
    const auto it = coingecko_prices_.find("chia");
    if (it != coingecko_prices_.end() && std::isfinite(it->second)
        && it->second > 0.0) {
        return it->second;
    }
}
// Existing XCH/<par-wrapper> fallback follows.
```

Add a test for: successful value -> clock passes freshness threshold -> next
fetch fails -> DEX value is selected and the stale external value does not
refresh valuation carry.

### 117-2 - High: a process that starts degraded still has inert breakers

**Locations:**
[`cpp/src/engine.cpp:11777-11779`](https://github.com/Bella-Addormentata/XOPTrader/blob/3d8e186b591e0d33f31a67a7968839d031a8bd36/cpp/src/engine.cpp#L11777-L11779),
[`cpp/src/engine.cpp:13663-13718`](https://github.com/Bella-Addormentata/XOPTrader/blob/3d8e186b591e0d33f31a67a7968839d031a8bd36/cpp/src/engine.cpp#L13663-L13718)

The new never-valued branch correctly sets `valuation_degraded_`, but the
authority gate then forbids seeding `peak_equity_hwm_usd_`. On a fresh process
the peak remains zero. After startup grace expires,
`equity_drawdown_frac(0, equity)` still returns zero, exactly as the new test
documents. This can persist indefinitely while one held asset is unpriced, so
the PR does not close its stated fail-open condition.

**Suggested change:** keep the existing frozen-peak behavior when a valid peak
already exists, but fail closed after grace if held assets are unvalued and no
peak has ever been established. Do not seed the peak from partial equity.

```cpp
if (drawdown_grace_remaining_ == 0
    && valuation_degraded_
    && peak_equity_hwm_usd_ <= 0.0) {
    state_->set_status(BotStatus::Paused);
    breaker_pause_active_ = true;
    // Emit one latched CircuitBreaker alert naming the unvalued assets.
}
```

Factor the decision into pure logic if an Engine fixture is still unavailable,
then test at least: degraded/no peak fails closed after grace; degraded/valid
peak keeps comparing against the frozen peak; clean startup seeds normally;
recovery requires the existing clean streak.

## PR #115: asset-level peg registry

### 115-1 - Medium: peg asset IDs are not canonicalized or validated

**Locations:**
[`cpp/src/config.cpp:434`](https://github.com/Bella-Addormentata/XOPTrader/blob/12d045ddf9a61145019a88df778b4b2c17fd7183/cpp/src/config.cpp#L434),
[`cpp/src/config.cpp:461-484`](https://github.com/Bella-Addormentata/XOPTrader/blob/12d045ddf9a61145019a88df778b4b2c17fd7183/cpp/src/config.cpp#L461-L484)

`parse_pairs()` lowercases each asset ID and requires `xch` or 64 hexadecimal
characters. `parse_pegged_assets()` accepts any non-empty, case-sensitive
string. An uppercase CAT tail is therefore accepted, but it can never match the
lowercased pair ID. Former stable-quote paths then return no value, silently
removing the XCH/USD anchor the registry is meant to provide. Lowercase
`peg_currency: usd` has a similar silent miss against the exact `"USD"` check.

**Suggested change:** extract the pair parser's asset-ID normalization and
validation into one shared helper and call it from both parsers. Normalize
ISO-style currency codes to uppercase at the same boundary. Add parser tests for
uppercase CAT IDs, malformed IDs, and lowercase `usd`.

### 115-2 - Medium: duplicate asset IDs silently overwrite safety policy

**Location:**
[`cpp/include/xop/peg_registry.hpp:170-177`](https://github.com/Bella-Addormentata/XOPTrader/blob/12d045ddf9a61145019a88df778b4b2c17fd7183/cpp/include/xop/peg_registry.hpp#L170-L177)

`by_asset_id_[key] = ...` makes the last duplicate declaration win. A copied
entry can silently flip `enforce` or `prefer_market_cross`, contradicting the
parser's fail-loud policy and making the effective emergency policy depend on
YAML order.

**Suggested change:** reject duplicates before insertion, preferably with a
`ConfigError` that names the asset ID and both symbols. A minimal parser-side
fix is:

```cpp
if (reg.find(a.asset_id) != nullptr) {
    throw ConfigError("pegged_assets: duplicate asset_id '" + a.asset_id + "'");
}
if (!reg.add(std::move(a))) {
    // Existing incoherent-entry error.
}
```

Add a test where two entries share an ID but disagree on `enforce`.

### 115-3 - Low: registry helper validation is internally inconsistent

**Location:**
[`cpp/include/xop/peg_registry.hpp:280-290`](https://github.com/Bella-Addormentata/XOPTrader/blob/12d045ddf9a61145019a88df778b4b2c17fd7183/cpp/include/xop/peg_registry.hpp#L280-L290)

`classify()` and `usd_par_value()` ignore unenforced declarations and reject
non-finite inputs. `deviation_pct()` does neither, so it returns a deviation for
an unenforced asset and returns infinity for `observed = +inf`. The helper has no
production caller yet, making this low severity, but it should be corrected
before S30 monitoring starts.

```cpp
if (a == nullptr || !a->enforce || !observed.has_value()
    || !std::isfinite(*observed) || !(*observed > 0.0)) {
    return std::nullopt;
}
```

Also parse `sustained_observations` through a signed/wide temporary and reject
negative or overflowing values rather than relying on unsigned YAML conversion.

### 115-4 - Low: remove obsolete pair-name parsing

**Locations:**
[`cpp/src/engine.cpp:11426-11429`](https://github.com/Bella-Addormentata/XOPTrader/blob/12d045ddf9a61145019a88df778b4b2c17fd7183/cpp/src/engine.cpp#L11426-L11429),
[`cpp/src/engine.cpp:11445-11448`](https://github.com/Bella-Addormentata/XOPTrader/blob/12d045ddf9a61145019a88df778b4b2c17fd7183/cpp/src/engine.cpp#L11445-L11448)

Both functions still parse `slash` and `quote`, then use only asset-ID-backed
registry helpers. CI is green, so this is not a current build failure; it is
dead code that obscures the intended removal of symbol-based decisions. Delete
both local blocks.

**Known scope gap:** the PR description now accurately states that the registry
does not make depeg monitoring asset-level. `PegRegistry::classify()` still has
no production caller and disabling the observation pair can still remove BYC
monitoring. Keep S30 open and do not describe #115 as completing that work.

## PR #116: Permuto research documentation

### 116-1 - Medium: the sample cannot support permanent market classifications

**Locations:**
[`docs/advanced-trading-methods.md:280-305`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/docs/advanced-trading-methods.md#L280-L305),
[`TODO-COMPETITION.md:132-141`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/TODO-COMPETITION.md#L132-L141)

The strategy chapter concludes that NVDA mean-reverts, TSLA trends, and a maker
will be systematically run over. The evidence is 200 samples over about 7.5
minutes from a 60-second trailing estimator resampled every five seconds. The
same PR correctly says those overlapping windows confound the variance-ratio
result and calls the sample indicative only. The table also does not define
whether `sd` is based on levels or increments or how it is normalized, so its
12-26% values cannot directly calibrate Avellaneda-Stoikov sigma.

**Suggested replacement:**

> This short sample suggests market-specific serial dependence, but overlapping
> 60-second windows, repeated observations, one regime, and no confidence
> intervals prevent classifying any market as mean-reverting or trending. Treat
> NVDA reversion and TSLA persistence as hypotheses for C-03. Before using sigma
> in quoting, define it on price increments at the model's time unit and report
> the normalization and uncertainty.

Apply the same qualification to the PR description and later strategy claims.

### 116-2 - Medium: jump, hedge, and leaderboard conclusions exceed the evidence

**Locations:**
[`docs/advanced-trading-methods.md:374-413`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/docs/advanced-trading-methods.md#L374-L413),
[`docs/advanced-trading-methods.md:426-436`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/docs/advanced-trading-methods.md#L426-L436)

The continuity assumption in standard variance-swap replication concerns the
underlying equity path. A large move in a derived realized-volatility estimator
does not by itself prove jumps in QQQ/NVDA/TSLA or invalidate that replication.
Permuto has no on-venue option hedge, but QQQ, NVDA, and TSLA have external
listed options; therefore "unhedged by construction" and "can only be flattened
on the same venue" are too broad. Finally, leaderboard correlation cannot
separate hedge absence from leverage, quote selection, adverse selection, or
execution quality.

**Suggested change:** call the jump model a hypothesis pending a jump test on
the underlying source returns. Replace the hedge conclusion with:

> Permuto provides no direct on-venue hedge. Under current XOPTrader scope the
> position would be unhedged unless an external options/variance adapter is
> added; such a hedge would still carry tenor, oracle-basis, collateral,
> latency, and access risk. Leaderboard losses are consistent with inventory
> risk but do not identify its cause.

### 116-3 - Low: API contract statements contradict one another

1. [`docs/permuto-api-reference.md:94`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/docs/permuto-api-reference.md#L94)
   says all `/exchange/*` routes require a session, while
   [`TODO-COMPETITION.md:276-278`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/TODO-COMPETITION.md#L276-L278)
   says `/exchange/leaderboard` and `/exchange/session` were verified public.
   State the public exceptions next to the general authentication rule.
2. [`docs/permuto-api-reference.md:100`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/docs/permuto-api-reference.md#L100)
   lists `GTC/ALO/IOC`, while
   [`docs/advanced-trading-methods.md:476`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/docs/advanced-trading-methods.md#L476)
   also lists `FOK`. Verify the OpenAPI enum and make both lists identical.
3. [`docs/advanced-trading-methods.md:465`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/docs/advanced-trading-methods.md#L465)
   omits `-PERP` from three market IDs even though the verified names in
   `TODO-COMPETITION.md` are `QQQ-VOL-PERP`, `NVDA-VOL-PERP`, and
   `TSLA-VOL-PERP`. Use exact API identifiers where the text claims an
   `/info/meta` result.
4. [`docs/permuto-api-reference.md:212`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/docs/permuto-api-reference.md#L212)
   calls `/ready` a Kubernetes liveness endpoint. It is readiness terminology;
   liveness controls restart behavior.
5. [`docs/permuto-api-reference.md:87`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/docs/permuto-api-reference.md#L87)
   calls the documented trailing realized-volatility oracle "IV." Use
   "decimal annualized volatility" unless the venue explicitly defines the
   value as options-implied volatility.

### 116-4 - Low: two operational references are misleading

[`TODO-COMPETITION.md:340`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/TODO-COMPETITION.md#L340)
says to "spend real money" on a qualifying fill after repeatedly establishing
that the contest uses simulated capital. Replace it with "use contest capital."

[`TODO-COMPETITION.md:350`](https://github.com/Bella-Addormentata/XOPTrader/blob/6e255cd301fe5ddacf20807d7e5e9b0d0a18a1f7/TODO-COMPETITION.md#L350)
says the dead man's switch is filed as S31, but PR #116 targets `main` at
`cfb36a0`, whose `TODO.md` has no S31 entry. Either add the cross-reference only
after the S31 PR merges or link directly to the relevant PR/branch.

## Recommended merge sequence

1. Fix #115's asset-ID normalization and duplicate handling, run its registry
   and config tests, then merge #115.
2. Fix both #117 high-severity findings and add focused regression tests. Retarget
   #117 to `main` after #115 merges, rerun the full C++ suite, then merge it.
3. #116 is independent and can merge in either order after its factual and
   contract inconsistencies are corrected.

## Review limitations

The GitHub CLI installed locally is not authenticated, so live metadata was
cross-checked through the authenticated VS Code GitHub integration and the
public GitHub REST API. Exact PR refs were fetched locally. No PR branch was
checked out and no source branch was modified. The existing local
`config.yaml` change was not touched.