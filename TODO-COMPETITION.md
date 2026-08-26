# Permuto Capital perps competition — scoping

Working document for whether and how XOPTrader competes at
<https://perps.permuto.capital>. Separate from `TODO.md`, which tracks the
live Chia bot.

**Status: scoped, nothing built.** All figures below were read from the live
venue on 2026-08-26 unless marked otherwise. Anything I could not verify is
flagged as such rather than smoothed over.

---

## What the venue is

**Volatility perpetual futures** — you trade implied volatility of an
underlying, not its price. A `MARK` of 9.13% is a vol number, not a dollar
price.

Three markets are **active** (`GET /info/meta`), all identical in shape:

| market | oracle ticker | max leverage | lot size | impact notional |
|---|---|---|---|---|
| `QQQ-VOL-PERP` | QQQ-VOL | 10x | 1 | 6000 |
| `NVDA-VOL-PERP` | NVDA-VOL | 10x | 1 | 6000 |
| `TSLA-VOL-PERP` | TSLA-VOL | 10x | 1 | 6000 |

**But 42 markets exist.** `GET /info/funding/predicted` lists plain stock
perps (`AAPL-PERP`, `TSLA-PERP`, `MSFT-PERP`, …) plus `f`- and `g`-prefixed
variants (`fAAPL-PERP`, `gAAPL-PERP`) — very likely the dividend and
appreciation legs of Permuto's registered equity product. None are
tradeable today: `/info/l2/AAPL-PERP` returns `market not found` and
`/info/stats` is empty. **Watch for activation** — it would change the
contest materially.

Funding on the vol markets: sampled every 5s, premium window 3600s, settled
hourly. (Note `/info/meta` top-level `funding_timing` says 3600s while the
per-market entries say 60s — unresolved, see gaps.)

Settlement is on Chia: Cloud Wallet / Sage auth, identities are 32-byte
puzzle hashes, deposits and withdrawals on-chain with an escape path.
Collateral is wUSDC.b with `POST /exchange/faucet` for testnet.

## The seed: $500,000, and it is not a CAT

Derived from `GET /exchange/leaderboard` — every MM row satisfies
`equity − total_pnl = 500000.00` exactly; traders get `100000.00`.

```
net_pnl = equity − finalized_competition_seed
```

The seed is **subtracted**, so rank is profit *above* the grant and everyone
starts level. The MM auth skill calls it "the finalized **paper-money**
grant for the WalletConnect MM account" — it is a clearinghouse balance, not
an on-chain token. `flags.withdrawals_enabled` is `false` and the banner
reads *"Competition Mode: withdrawals disabled"*, so it cannot leave the
venue. XOPTrader would read it from `/exchange/account`, never hold it in a
wallet.

Do not confuse it with the **$500k risk-increasing order cap per market**
($1M portfolio notional) in the API skill — same number, unrelated.

## How the contest is scored — C-01 ANSWERED

Two independent metrics, not summed:

1. **Eligibility gate** — banked balanced depth ≥ **300,000,000**
2. **Rank among eligible** — by net PnL, descending; ties on `user_id` ascending

```
# per market, per ~10s tick, only within ±2% of a FRESH oracle:
balanced_depth_usdc = min(bid_notional_in_band, ask_notional_in_band)
depth_usdc          = Σ_markets balanced_depth_usdc
depth_seconds      += depth_usdc × 10          # monotonic
```

The skill states the gate plainly: **≈$3,000 average balanced depth for ~28
hours** (~80% of cash-session hours). On a $500k seed that is 0.6% of
capital. **The binding constraint is uptime and two-sidedness, not size.**

Contest window guidance: Monday 09:30 ET → Friday 16:00 ET.
`CONTEST_START` is currently unset, so eligibility integrates the rolling 5d
bank.

Rules that shape the design:

- **Two-sided or nothing.** `min(bid, ask)` means one lifted side earns
  **zero** for that market until restored. Use `batch_upsert` to restore.
- **±2% of oracle.** `MM_UPTIME_PROXIMITY_PCT` = 2, while
  `vol_oracle_band_pct` = 5 — you may quote wider than you earn on.
- **Monotonic.** Banked score cannot be taken away by an adversary.
- **Zero-depth ticks.** Pauses, restarts and stale oracle advance the window
  and add nothing. Downtime is silent, permanent loss of accrual.

### The opening: the entire field is failing the gate

Live book, 2026-08-26 15:11 UTC:

| market | bids | asks | balanced |
|---|---|---|---|
| QQQ-VOL-PERP | 3 ($1,576) | **0** | **$0** |
| NVDA-VOL-PERP | 2 ($650) | **0** | **$0** |
| TSLA-VOL-PERP | 2 ($550) | 1 ($150) | **$150** |

Venue-wide balanced depth: **$150** against a ~$3,000 requirement. All 20 MM
rows show `prize_eligible: false`; the best has 7.2M depth-seconds, still
**40× short** of the floor. Top MM is +$20,033 on 200 trades; the top
*trader* is +$1.33M on a $100k seed.

A bot that simply quotes both sides and stays up would clear a bar nobody
has cleared. That is precisely what XOPTrader's tier ladder does.

## THE TRAP: quote-only accounts get wiped

> Bots that only quoted lose session, API keys, and **seed**. Re-signup is
> impossible while `signup_closed` stays true.

`flags.untraded_purge_at` is the public clock. **At least one real fill**
(maker or taker) is required before it — cancels and IOC misses do not
count. Currently `untraded_purge_at: null` and `signup_closed: false`, so
nothing is armed. This must be polled, not assumed.

## Connecting: API, not an embedded browser

Unattended bots do **not** need Sage or a browser:

> Unattended bots skip this path — they use an explicit BLS pubkey from
> config/key file and sign the raw nonce bytes directly.

| step | call | note |
|---|---|---|
| 1 | `POST /exchange/wallet_link_challenge` | binds signing pubkey, returns `{challenge_token, nonce}` |
| 2 | *(local)* | AugSchemeMPL over 32 decoded nonce bytes |
| 3 | `POST /exchange/wallet_auth` | returns session token |

Then `Authorization: Bearer <token>` on trading calls. Poll
`flags.signup_closed` **before** step 1 — a new identity gets HTTP 403 and
must not proceed to signing.

Verified read-only access with no auth: `/health`, `/info/mids`,
`/info/oracle`, `/info/l2/{market}`, `/exchange/session`, `/info/meta`,
`/exchange/leaderboard` all return 200. `auth_enforced: false`.

**This requires a local BLS private key** — a new class of secret for this
project. Placement, protection, and whether it is reused from an existing
Chia key are operator decisions and are not assumed here.

### GUI: a Permuto tab, connecting to the API

Recommended shape — **not** an embedded web view, which would be a second UI
we do not control and cannot drive a bot from:

- connection state (linked pubkey, session validity, time to re-auth)
- the three markets: oracle, mark, our bid/ask, in-band or not
- **depth-seconds progress against 300,000,000**, per market and total —
  the single number that decides eligibility
- net PnL vs the $500k seed, positions, leverage, liquidation distance
- `untraded_purge_at` countdown and whether we have a qualifying fill
- a kill switch that flattens and cancels

## What transfers from XOPTrader

| layer | verdict |
|---|---|
| Quoting brain — Avellaneda-Stoikov, tier ladders, spread floors, inventory skew, PID, adverse-selection sizing | **transfers**; venue-agnostic and the expensive part to have built |
| Risk — drawdown/rolling breakers, pause, valuation authority | mostly transfers; needs perp concepts |
| Peg handling | **done** — `cpp/include/xop/peg_registry.hpp` |
| Market data | **rewrite** — REST `/info/*` + WebSocket, not dexie polling |
| Execution | **rewrite** — `POST /exchange/order` (GTC/IOC/FOK/ALO), `batch_upsert`, `cancel_all` |
| Position / margin | **new** — positions, leverage, funding accrual, liquidation |
| Auth | **new** — BLS nonce signing, session renewal |

A venue adapter plus a perps position model, not a new trading pair.
XOPTrader is spot throughout: a position *is* a wallet balance and a fill is
an atomic CAT swap. `batch_upsert` (12 legs, one mutate token,
modify-or-place per market+side) maps closely onto the Step 8 ladder repost.

## Sequenced work

- [x] **C-01 Depth-seconds gate quantified.** ≈$3,000 balanced depth × ~28h.
      Reachable. No longer blocking.
- [ ] **C-02 Decide auth path and key handling.** MM prizes require the
      WalletConnect/BLS identity, not OAuth. Where does the BLS secret live?
      **Operator decision.**
- [ ] **C-03 Read-only client + paper mode.** `/info/*` + WS ingest, logging
      what we *would* quote and the depth-seconds we *would* bank, scored
      against the real book. No orders, no auth, no key. Proves the strategy
      and the gate maths before any risk.
- [ ] **C-04 Perps position model.** Positions, margin, funding accrual,
      liquidation distance.
- [ ] **C-05 Execution adapter.** `order` / `modify` / `cancel` /
      `batch_upsert` / `cancel_all`, respecting place-vs-mutate rate tokens.
- [ ] **C-06 Risk layer for perps.** Reuse the breakers; add liquidation
      proximity and a funding budget. Never let a position price at 0 in
      silence (see the 2026-08-25 equity blindness in `TODO.md`).
- [ ] **C-07 Two-sided uptime discipline.** Restore the lifted side before
      the next ~10s tick; arm `schedule_cancel` as a dead-man's switch
      (min 5000ms, max 10 fresh arms/day).
- [ ] **C-08 Testnet dry run.** Faucet, quote all three, measure actual
      accrual against the gate, and secure the qualifying fill early.

## Operator decisions

- Compete at all? The gate is reachable and the field is not clearing it.
- Where the BLS signing key lives, and whether it is a fresh identity.
- Real capital or testnet only — prizes may require mainnet; unconfirmed.
- **Must competition code stay behind an adapter boundary?** Strongly
  recommended: the live bot is paused mid-incident and should not absorb
  churn from an experiment.

## Sources

- <https://perps.permuto.capital> — in-app API documentation
- <https://perps.permuto.capital/llms.txt> — agent discovery index
- `/.well-known/agent-skills/perps-market-maker-auth/SKILL.md` — auth + "How to win"
- `/.well-known/agent-skills/perps-api/SKILL.md` — trading endpoints, limits
- `GET /info/meta`, `/exchange/leaderboard`, `/info/l2/*` — read 2026-08-26
