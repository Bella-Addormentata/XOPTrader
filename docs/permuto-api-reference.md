# Permuto Capital API — endpoint reference and intended use

Working reference for the Permuto Capital perps API, `https://perps.permuto.capital`.
Every fact here was read from the live venue, its OpenAPI 3.1 spec, its
published agent skills, or the product page, on **2026-08-26**. Where
something is inferred rather than documented it is marked.

Companion documents: `TODO-COMPETITION.md` (whether to compete and what it
would cost) and `advanced-trading-methods.md` §4 (the SVPerp literature and
what we measured).

---

## 0  What an SVPerp actually is — and why it matters to every endpoint below

From the product page (permuto.capital/product-svperps):

> perpetual contracts on the **realized volatility** of U.S. equities
> … **a single number built from price movement over the preceding 60
> seconds** … **refreshed every five seconds** during U.S. market hours

That one sentence explains most of what we measured, and it changes how the
market-data endpoints should be read.

The oracle is a **60-second trailing realized-volatility estimate, resampled
every 5s**. A realized-vol estimate built from sixty seconds of data has a
very large standard error — so a substantial part of the 12–26% dispersion
we measured is **estimator noise, not market information**. That is a
statistically different thing from a price series moving on news.

Two consequences worth carrying into every design decision:

1. **Successive oracle prints overlap.** At a 5s refresh over a 60s window,
   consecutive observations share ~55/60 of their input. They are
   autocorrelated *by construction*, which is why levels look like a random
   walk at short horizons (our VR(2) ≈ 1.0) while the underlying quantity
   is not actually moving that much.
2. **Estimator noise reverts; information does not.** To the extent the
   jitter is sampling error around a slower true volatility, quoting around
   a *smoothed* oracle is sound and the noise is a source of edge rather
   than of toxicity. **This is the central untested hypothesis of the whole
   exercise** and is what the analysis-mode observer (C-03) exists to
   settle.

Other product facts: settlement in wUSDC.b, collateral in a public on-chain
account wallet, up to 10× leverage, no expiry, on-chain activity record
published every five minutes, and **non-U.S. participants only**.

---

## 1  Public market data — `/info/*`

No authentication. Verified working unauthenticated.

| endpoint | what it returns | how we would use it |
| -------- | --------------- | ------------------- |
| `GET /info/meta` | markets, feature flags, band percentages, funding cadence | **Read first, every startup.** Source of truth for which markets are `active`, and for `signup_closed` / `untraded_purge_at` |
| `GET /info/oracle` | current oracle price per market | The reference everything is measured against. ~1s ingest cadence |
| `GET /info/mids` | mid / mark per market | Mark drives PnL and margin; the mark–oracle gap is the funding premium |
| `GET /info/l2/{market}` | L2 book snapshot | `?levels=N`, default 20, **max 500**. On VOL markets only levels within ±`vol_oracle_band_pct` are shown |
| `GET /info/trades/{market}` | recent trades | Realised flow. Our only view of what actually fills |
| `GET /info/candles/{market}` | OHLCV from TimescaleDB | ⚠ **`tf` is accepted and IGNORED** — `tf=60` and `tf=300` both return 3600s bars. Hourly is all there is |
| `GET /info/funding/{market}` | historical funding | Backtest the funding leg |
| `GET /info/funding/predicted` | predicted rates + impact bid/ask + instant premium | Forward-looking funding. Also the only place the other **42 configured markets** are visible |
| `GET /info/stats` | 24h stats | Currently `[]` |
| `GET /info/clearinghouse` | state root, epoch | On-chain settlement progress |
| `GET /info/price_certificate` | BLS-signed oracle snapshot | ⚠ returns `No price certificate available yet` — **no signed provenance is available** |
| `GET /info/tx_log` | tx log state + pending count | Settlement backlog depth |
| `GET /info/wallet_bls_trading_address` | resolve trading address for a BLS pubkey | **No link side effects** — safe to call before committing to an identity |
| `POST /info/wallet_bls_trading_addresses` | batch version of the above | Same, batched |

**Band parameters, from `/info/meta`.** There are two and they are not the
same:

- `vol_oracle_band_pct` — default **5** — the *legal placement* band. Orders
  outside it are rejected HTTP 400.
- `vol_aggressive_ring_pct` — default **2** — the inner *aggressive* ring.
  Outside the ring only **passive** rests are allowed (bids ≤ oracle, asks ≥
  oracle). **This ±2% is the same band that governs depth-seconds credit.**

**Funding cadence.** `markets[].funding_timing` is authoritative and VOL
markets settle every **60s**. The top-level `funding_timing` (3600s) is only
a platform fallback — reading the wrong one understates funding frequency
by 60×.

**Order prices are decimal annualized vol** — `0.176` means 17.6%. A missing
or stale oracle returns HTTP **503**.

⚠ The API skill calls this "IV", which conventionally means *implied*
volatility from option prices. §0 establishes the ORACLE is a 60-second
trailing **realized**-vol estimate. Both can be true — the traded mark is a
forward-looking number the book agrees on, while the oracle it settles
against prints realized — but do not read "IV" here as an options-derived
signal. There is no options chain on this venue.

---

## 2  Trading — `/exchange/*`

All require a session token: `Authorization: Bearer <token>` or
`Cookie: perps_session=<token>` — **except `GET /exchange/leaderboard`**,
which is public.

⚠ **Send a browser `User-Agent`.** Verified 2026-08-27: the leaderboard
returns full data with no credentials, but a bare `urllib`/default agent
gets **HTTP 403**. The 403 is agent filtering, not an auth requirement, and
reads as "we need a token" if you do not test it. This is what lets the
depth-accrual probe (`scripts/permuto_depth_probe.py`) sample the field
without an account.

### Order entry

| endpoint | use |
| -------- | --- |
| `POST /exchange/order` | single order. `market`, `side` (`buy`/`sell`), `size` required; `order_type` (`market`/`limit`), `price`, `tif` (`GTC`/`ALO`/`IOC`), `reduce_only` |
| `POST /exchange/batch_place` | up to 12 limit legs, one **place** token. Insert-only semantics |
| `POST /exchange/batch_upsert` | up to 12 GTC/ALO quotes, one **mutate** token. Modify-or-place per `(market, side)`, at most one leg each |
| `POST /exchange/batch_modify` | up to 12 resting orders by `order_id`, one mutate token |
| `POST /exchange/modify` | single reprice / partial-fill top-up |
| `POST /exchange/cancel` | cancel one resting order |
| `POST /exchange/cancel_all` | `user_id` required, optional `market` |
| `POST /exchange/close` | close a position with a reduce-only market order |

**`batch_upsert` is the one that matters for us.** It maps almost exactly
onto XOPTrader's existing Step 8 cancel-and-repost ladder, and it is the
documented way to restore a side after a fill. Since depth credit is
`min(bid_notional, ask_notional)`, **a lifted side earns zero for that
market until restored** — so the restore path must be one call, not a
cancel/place pair.

Rate limiting distinguishes **place** tokens from **mutate** tokens; a
12-leg batch costs one token either way. Any quoting loop should be built on
batches from the start rather than retrofitted.

### Account and state

| endpoint | use |
| -------- | --- |
| `POST /exchange/account` | `user_id` required. Balance, positions, PnL. **This is where the $500k seed appears** — it is a clearinghouse balance, not a wallet asset |
| `POST /exchange/open_orders` | resting orders — reconcile against our own view |
| `POST /exchange/trade_history` | fills. Cursor paginate with `after_fill_id` + `limit` (default 200, **max 1000**); `limit` without a cursor → HTTP 400 |
| `POST /exchange/funding_history` | funding paid/received |
| `POST /exchange/leverage` | per-market leverage |
| `GET /exchange/leaderboard` | ⚠ **paginated, default 20.** Check `market_makers_total` and page — reading page one only is how we misread the field by 35× |

### Safety

| endpoint | use |
| -------- | --- |
| `POST /exchange/schedule_cancel` | **dead man's switch.** `{user_id, time: <utc_ms>}` arms a future cancel-all; omit `time` to clear. `min_delay_ms` 5000, `max_triggers_per_day` 10. Extend on each healthy loop |

`schedule_cancel` is the single most valuable endpoint for us given this
week's incident. A four-hour engine wedge with live quotes is exactly what
it prevents — the venue cancels on our behalf if we stop extending the
deadline. **Note the budget:** 429 when a *fresh* arm would exceed 10/day;
re-scheduling while already armed is always allowed. So extend, never
disarm-and-rearm.

---

## 3  Authentication

Three paths. **Market-maker prizes require the WalletConnect/BLS identity,
not OAuth.**

### Unattended bot (what we would use)

| step | call |
| ---- | ---- |
| 0 | `GET /info/meta` → check `flags.signup_closed` **before** anything else |
| 1 | `POST /exchange/wallet_link_challenge` → `{challenge_token, nonce}` |
| 2 | *(local)* AugSchemeMPL sign over the 32 decoded nonce bytes |
| 3 | `POST /exchange/wallet_auth` → session token |

A new identity gets HTTP **403** at step 1 when signup is closed — fail
there, do not proceed to signing. `GET /exchange/session` exposes
`linked_wallet_*` (recover the signing pubkey after expiry) and
`trading_*` (current authorized identity).

`POST /exchange/link_wallet` + `POST /exchange/wallet_challenge` are the
**deprecated** two-step flow and can race across sessions. Use
`wallet_link_challenge`.

### OAuth trader bot

Browser OAuth once → `POST /exchange/api_keys` → bot calls
`POST /exchange/agent_session` with `Authorization: Bearer perps_agent_…`
to mint sessions, **renewed every ~40 minutes or on any 401**. Trade with
the returned session token, never the API key.

Supporting: `/oauth/authorize`, `/oauth/token`, `/oauth/jwks`,
`/exchange/cloud_wallet_auth`, `/exchange/cloud_wallet_callback`,
`/exchange/cloud_wallet_oauth_state`.

**Security note.** The unattended path needs a **local BLS private key** —
a class of secret this project has never held; Chia keys have always stayed
in the wallet. Placement and protection are an operator decision, unresolved.

---

## 4  Settlement and on-chain

| endpoint | use |
| -------- | --- |
| `POST /exchange/register` | register wallet, compute deposit puzzle hash |
| `POST /exchange/deposits_v3` | pool-model deposit status |
| `POST /exchange/deposits` | legacy escrow history |
| `POST /exchange/withdraw` | queue a withdrawal |
| `POST /exchange/withdrawals` | withdrawal history |
| `POST /exchange/max_withdrawal` | maximum withdrawable |
| `POST /exchange/escape` | **escape-request puzzle hash for on-chain equity exit** |
| `POST /exchange/faucet` | testnet wUSDC.b |

`flags.withdrawals_enabled` is currently **false** ("Competition Mode:
withdrawals disabled"), so the withdrawal family is inert during the
contest. `escape` is the trust-minimising exit and worth understanding
before depositing anything real.

---

## 5  Streaming and ops

| endpoint | use |
| -------- | --- |
| `WS /ws` | channels incl. `l2Book` (fixed 20 levels), `bbo`, `trades`, `oracle` |
| `GET /health` | detailed; **503 when live mutations are blocked**. Pause alone still returns 200 |
| `GET /ready` | k8s-style liveness |
| `GET /metrics` | Prometheus text |
| `GET,POST /mcp` | MCP transport + JSON-RPC tool catalog |

WS notes: `oracle` broadcasts the full price map on each ingest (~1s), and
on VOL markets `l2Book`/`bbo` use the same oracle band as marks — so
out-of-band resting liquidity is **omitted from snapshots**. Our own
out-of-band orders will not appear in the book we read back. `markPrice` on
subscribe may equal oracle until the first computed mark publish.

---

## 6  Constraints that bind a market maker

Collected from the API skill; each has bitten someone already.

- **Risk-increasing orders capped at $500k/market, $1M portfolio notional**
  (position mark + risk-increasing resting). Reduce-only is exempt. Not to
  be confused with the $500k seed — same number, different thing.
- **Carried session (equity market closed): risk-increasing places need 8×
  stressed initial margin** (`VOL_CARRIED_STRESS_MULTIPLE`). Reduce-only
  exempt. Overnight depth is low-risk but **not low-capital**.
- **At carried→live, every resting order and pending trigger on that vol
  market is cancelled.** Quotes cannot be carried through the open; re-quote
  after. The venue is protecting makers from the 488%-range hour.
- **Untraded purge.** Quote-only accounts lose session, API keys **and
  seed**, and re-signup is blocked while `signup_closed` is true. ≥1 real
  fill (maker or taker) required; cancels and IOC misses do not count.
- **Rate limits.** 429 on `trade_history` → exponential backoff and retry
  **the same** `after_fill_id`; do not advance the cursor. 503 → retry, also
  without advancing.
- **`status: "partial"` is success**, not failure. Only `"rejected"` is a
  failure.

---

## 7  Endpoints we would exercise, in order

1. **C-03 observer, read-only, no key, no capital.** `/info/meta`,
   `/info/oracle`, `/info/l2/{market}`, `/info/trades/{market}`,
   `/info/funding/predicted`, plus `WS /ws`. Records the oracle at 1–2s and
   computes what depth-seconds and PnL a candidate rule *would* have earned.
   This is the only way to get sub-hourly history, since `tf` is ignored.
2. **Auth.** `wallet_link_challenge` → sign → `wallet_auth`, gated on
   `signup_closed`.
3. **State.** `/exchange/account`, `/exchange/open_orders`.
4. **Safety first, before any quote.** `schedule_cancel` armed and extended
   on every healthy loop.
5. **Quote.** `batch_upsert` for two-sided ladders; `cancel_all` as the
   kill switch.
6. **Reconcile.** `trade_history` with cursor paging; `funding_history`.

## 8  Open questions

- **Do carried (frozen-oracle) ticks accrue depth-seconds?** Carried is not
  the same as stale — stale placement returns 503 while carried placement is
  allowed at 8× IM — but whether the ~10s sampler *credits* carried ticks is
  undocumented, and it decides whether the overnight window is worth the
  capital.
- **Is the oracle's short-horizon jitter estimator noise or information?**
  §0 argues it is substantially the former. Decides whether this is a
  quotable market at all.
- **What is `vol_aggressive_ring_pct` for, exactly?** Documented as the
  passive-only boundary and it coincides with the depth-credit band, but
  whether aggressive quoting inside the ring earns differently is unstated.
- **Why is `price_certificate` empty?** The documented oracle attestation is
  not being produced, so there is no signed provenance for the number the
  entire product settles on.
