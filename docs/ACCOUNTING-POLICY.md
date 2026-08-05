# XOPTrader accounting policy

The written policy for how the bot accounts for money. Change this document
in the same commit as any code that changes the treatment — silent divergence
between policy and implementation is how the July 2026 P&L failures went
unnoticed for three months.

Status: adopted 2026-07-30 (v0.8.0).

---

## 1. Chart of accounts

Every value the bot holds is one **asset account**, keyed by canonical asset
id — `xch`, or the 64-character lower-hex CAT id. Never by display symbol:
`"XCH"` and `"xch"` are different strings, and keying by the display symbol is
exactly what pinned unrealized P&L at zero before v0.8.0.

Currently tracked: `xch`, wUSDC.b, BYC, DBX.

## 2. Double-entry (`ledger_entries`)

Every event that changes what the bot believes it holds posts a **balanced set
of legs**. Summing `delta_mojos` per asset gives the ledger's implied balance.

| Event | Legs |
|---|---|
| `opening` | one leg per asset = wallet confirmed balance at genesis |
| `fill` (bid) | base `+size`, quote `−quote_mojos`, `xch −fee` |
| `fill` (ask) | base `−size`, quote `+quote_mojos`, `xch −fee` |
| `adjust` | single leg, explicitly labelled, for movements with no internal event |

**Sign convention**: positive is an inflow to the bot, negative an outflow.

**Idempotency key**: `(event_id, leg, asset_id)` is UNIQUE. A fill re-detected
after a crash re-posts identical legs, which are ignored rather than doubled.

**Append-only.** Never `UPDATE` or `DELETE` a ledger row. Corrections are new
`adjust` legs that reference the original event. Both historical backfill
scripts violated this on `trade_log` and left a column that is a palimpsest of
three incompatible formula generations; they are now retired and refuse to run.

**Genesis does not replay `trade_log`.** That table was shown on 2026-07-30 to
disagree with the wallet by ~665 XCH; replaying it would import precisely the
corruption the ledger exists to detect. The ledger starts from observed wallet
balances and is authoritative only from that point forward.

## 3. The control account: wallet confirmed balance

The wallet is the control account; the ledger is the subsidiary ledger. They
must tie.

**Tie to `confirmed_wallet_balance`. Never `spendable_balance`.** Posting an
offer reserves *whole UTXOs* — a 0.005 XCH fee can lock a 16 XCH coin — so
spendable swings by up to 100% of a wallet purely from the bot's own quoting,
while confirmed is unchanged because the coins are still unspent on-chain.
Tying to spendable would book a phantom outflow on every quote and a phantom
inflow on every cancel.

**Do not use `OnChainReconciler`'s `on_chain` figure for this.** It sums coins
at puzzle hashes harvested from `get_spendable_coins`, which excludes
offer-reserved coins, so it is structurally biased low: 1,599 of 1,599 samples
over 2.9 days were negative, median −11.5 XCH, correlating +0.88 with open
offer count. That existing check also triple-counts the XCH wallet (keyed by
pair label, not wallet id), producing ~550 warnings/day that nothing acts on.
It is a monitoring artifact, not an accounting control.

## 4. Tolerance model

The invariant is checked per asset:

```
divergence = ledger_balance(a) − wallet_confirmed(a)

tolerance  = live_offer_exposure(a)          # only live offers can settle
           + fee_slack (XCH only)            # observed dust is 5,000-mojo steps
           + max(floor_a, pct × confirmed_a)
```

`live_offer_exposure` is the honest bound on what can move between two
observations, is exactly computable from the live book, and **collapses to
zero when the book is empty** — which is when the control becomes tight enough
to catch a single missed fill.

Percentage-only tolerances were rejected: the CAT wallets are tiny (wUSDC.b
runs 9,000–38,000 mojos, and one heartbeat of fills moves 20–75% of that),
while 1% of the XCH wallet is 1.2 XCH — enough to swallow an entire missed
1-XCH fill.

| Parameter | Default | Basis |
|---|---|---|
| `alert_pct` | 0.5% | 0.50–0.72 XCH on current balances: above all observed dust, below one 1-XCH offer |
| `pause_pct` | 2% | ~3 offers' worth |
| `floor_xch_mojos` | 1e9 (0.001 XCH) | covers fee dust |
| `floor_cat_mojos` | 100 (0.1 unit) | 100× the 1-mojo per-leg `llround` error |
| `fee_slack_mojos` | 200,000 | ~40 offer events at 5,000 mojos each |

**Persistence requirement**: escalation needs N consecutive **same-signed**
breaches (alert at 2, pause at 3). The ledger can only ever *lag* the wallet,
never lead it, so a latency-induced divergence self-heals on the next
observation while a real one does not.

**Gates** — the check is skipped, not alarmed, when any hold: `pending_change
!= 0` for that wallet (coins in flight), the balance snapshot is older than
`max_balance_age_blocks`, or the confirmed balance is non-positive.

Backtested over 205 heartbeat intervals of live data, this model produces zero
alerts across the 134 dust-and-fill intervals and fires on all 22 intervals
with genuinely unexplained whole-XCH movement.

## 5. Escalation

Alert-only by default (`pause_enabled: false`). Auto-pausing is deliberately
opt-in because several **real** balance movements still have no ledger event:

- Taker fills in the arbitrage and drift-correction steps call `take_offer()`
  and record nothing to State, inventory, `trade_log` or `offer_log`.
- External deposits and withdrawals are invisible — nothing polls
  `get_transactions` for accounting.
- DBX liquidity rewards arrive directly in the wallet.

Until those post legs, an auto-pause would halt trading on legitimate activity.
Turn `pause_enabled` on once the ledger runs clean for a sustained period.

## 6. Cost basis

**Weighted average**, per asset, stored USD-normalized (USD per base unit ×
1e12). Permitted by IAS 2 and ASC 330. LIFO is not used and is prohibited
under IFRS.

USD normalization exists because one asset trades against several quote
currencies: XCH quotes in wUSDC.b (~$1.35), BYC (~1.15 BYC) and DBX (~100 DBX)
simultaneously, and a single shared basis in raw pair units would blend
incompatible currencies. Conversion happens only at the boundary, via
`Engine::to_usd_pseudo` / `from_usd_pseudo`.

**Unknown cost is never fabricated.** When no USD valuation is available, the
fill posts quantity only and the basis is left untouched and flagged for
repair. Substituting a placeholder price destroys the position's basis
irrecoverably.

Peg assumptions: wUSDC/wUSDC.b/USDS are treated as exactly $1. BYC is a
CDP-backed stablecoin that trades off peg and uses its live cross rate. DBX is
not pegged and is cross-derived from the XCH rate.

### Why the fiat wrappers are pinned rather than floated

wUSDC.b is the **numeraire** — the unit everything else is measured in. BYC can
float because there is a `BYC/wUSDC.b` market to price it *in*; floating the
numeraire itself would require an external anchor and would make every
historical figure move whenever that feed twitched.

More decisively: a live rate inside a **persisted** cost basis is the exact
failure removed in v0.8.0, where a hardcoded 2.70 XCH rate was baked into
stored basis and ran ~2x wrong for months. A feed glitch would write a
permanently wrong basis to `inventory_state`.

The exposure is nonetheless real, so it is **monitored** (`accounting.peg_*`):

| signal | catches | threshold |
|---|---|---|
| CoinGecko `usd-coin` vs par | native USDC depeg | 1% |
| implied wUSDC.b = `usdc × cex_mid / dex_mid` | **bridge** depeg | 3% |

The second exists because native USDC can hold $1.00 while the Chia bridge
breaks — the feed alone cannot see that. Its 3% threshold clears this venue's
structural DEX-vs-CEX basis (217 logged samples: p50 78 bps, p90 118 bps, max
218 bps), so a 2% threshold would fire on ordinary basis. Both require 4
consecutive breaches and stay silent on missing data.

> ⚠️  The separate `depeg:` detector does **not** cover this. It compares a
> pair's own mid against a config constant and is registered only for
> BYC/wUSDC.b, so it cannot see wUSDC.b move — wUSDC.b is that pair's quote
> unit. Its `auto_disable_pair`, `alert_on_warn` and `alert_on_bail` settings
> are parsed and printed but **never read by any code path**; a bail only sets
> `quote_valid = false` for one cycle. It has bailed 172 times and sent zero
> alerts.

## 7. Revenue recognition

Realized P&L is recognized **on disposal**, not on holding — the realization
principle. Unrealized mark-to-market is reported separately and never mixed
into realized figures.

Mark source: the DEX mid price for the pair. Note for ASC 820 purposes that a
thin book (DBX especially) is not a Level 1 input; marks on those assets should
be treated as estimates.

## 8. Fees

On-chain fees are XCH-denominated and expensed when **paid**, which for an
offer's creation fee is at settlement — the fee rides inside the offer's spend
bundle and is never paid for the ~94% of offers that are cancelled. Booking it
at post time would manufacture a systematic phantom XCH outflow.

Known gap: cancelled-offer fees (roughly 32× the filled-offer fees in
aggregate) are recorded only in `offer_log` and are not yet part of the P&L
fee leg.

## 9. Tax reporting — known divergence

Weighted-average cost is correct for internal risk control but is **not** a
method US tax rules recognize for property. The IRS treats crypto as property;
FIFO is the default and Specific Identification is permitted with adequate
records. `PnLTracker::export_trades_csv` currently emits Form 8949 rows using
weighted-average cost.

**This gap is unresolved and should be settled with a qualified accountant
before the export is used for filing.** Closing it means tracking FIFO lots
alongside the weighted-average basis used for risk.

## 10. Materiality and consistency

Thresholds live in `config.yaml` under `accounting:` so they are explicit and
reviewable rather than scattered as literals.

Consistency is a stated requirement: a change of method (cost basis, mark
source, fee treatment) is a change to this document first, with the prior
treatment and the switch date recorded, so historical figures remain
interpretable.
