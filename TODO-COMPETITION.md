# Permuto Capital perps competition — scoping

Working document for whether and how XOPTrader competes at
<https://perps.permuto.capital>. Separate from `TODO.md`, which tracks the
live Chia bot.

**Status: scoping only. Nothing committed, nothing built.**

---

## What the venue actually is

Not stocks, and not spot. **Volatility perpetual futures** — you trade the
implied volatility of an underlying, not its price. The `MARK` of 9.13%
observed on 2026-08-26 is a vol number, not a dollar price.

Three markets live (`GET /info/meta`), all identical in shape:

| market | oracle ticker | max leverage | lot size | impact notional |
|---|---|---|---|---|
| `QQQ-VOL-PERP` | QQQ-VOL | 10x | 1 | 6000 |
| `TSLA-VOL-PERP` | TSLA-VOL | 10x | 1 | 6000 |
| `NVDA-VOL-PERP` | NVDA-VOL | 10x | 1 | 6000 |

Funding: sampled every 5s, premium window 60s, settled every 60s.

**It settles on Chia.** Cloud Wallet auth, identities are canonical 32-byte
puzzle hashes, on-chain deposits/withdrawals plus an escape path for
equity exit. Collateral is **wUSDC.b**, with `POST /exchange/faucet` for
testnet coin. So the wallet and settlement world is one we are already in —
though note the live wUSDC.b bridge compromise (see
`TODO.md` / warp.green) makes the mainnet asset and the testnet faucet
asset very different propositions.

API surface: 60 HTTP routes + 1 WebSocket. There is an MCP endpoint
(`GET`/`POST /mcp`), an `llms.txt`, and published agent skills under
`/.well-known/agent-skills/` — the venue explicitly expects bots.

## How the contest is scored

From `llms.txt`, verbatim:

> Competition scoring: MM prizes use a `depth_seconds` **eligibility gate**
> (≥ 300,000,000), then **net PnL** rank (not summed)

Two stages, and the order matters:

1. **Eligibility** — accumulate ≥ 300,000,000 depth-seconds. Depth accrues
   only while quoting within ±`MM_UPTIME_PROXIMITY_PCT` of oracle. This is
   an uptime-and-tightness gate: it rewards being continuously present near
   the reference, which is exactly what a tiered ladder does.
2. **Rank** — among the eligible, by net PnL.

`GET /exchange/leaderboard` exposes `total_pnl`, `depth_seconds_5d`,
`depth_seconds_24h`, `prize_eligible`, and page-level
`mm_prize_min_depth_seconds`. A contest window applies when `CONTEST_START`
is set, otherwise it is a rolling 5-day bank.

**Open question — the first thing to settle.** 300,000,000 depth-seconds is
a large number and its units are not documented on the API page. If it is
notional-dollars × seconds, then e.g. $6,000 of resting depth held
continuously for 5 days is 6000 × 432,000 ≈ 2.6e9, comfortably over. If the
unit is something else the gate could be unreachable at our size. **Read the
market-maker auth skill's "How to win" section before any build work** —
this single number decides whether the whole exercise is viable.

## What transfers from XOPTrader

| layer | verdict |
|---|---|
| Quoting brain — Avellaneda-Stoikov, tier ladders, spread floors, inventory skew, PID, adverse-selection sizing | **transfers**, venue-agnostic, and the expensive part to have built |
| Risk — drawdown/rolling breakers, pause, valuation authority gate | mostly transfers; needs perp concepts added |
| Peg handling | **just generalised** — see `cpp/include/xop/peg_registry.hpp` |
| Market data | **rewrite** — REST `/info/l2`, `/info/mids`, `/info/oracle`, `/info/candles` + WebSocket, instead of dexie polling |
| Execution | **rewrite** — `POST /exchange/order` (GTC/IOC/FOK/ALO), `batch_upsert`, `cancel_all`. No offer files, no `take_offer` |
| Position / margin | **new** — positions, leverage, funding accrual, liquidation distance do not exist in XOPTrader today |
| Auth | **new** — OAuth once in a browser, then `agent_session` renewed every ~40 min or on any 401 |

The honest summary: this is **a venue adapter plus a perps position model**,
not a new trading pair. XOPTrader's entire data model is spot — a position
*is* a wallet balance, and a fill is an atomic swap of two CATs. Perps have
positions distinct from balances, margin, funding, and liquidation.

One encouraging detail: `POST /exchange/batch_upsert` ("modify-or-place per
market+side, up to 12 legs for one rate-limit token") maps almost directly
onto the existing cancel-and-repost ladder in Step 8, and is friendlier than
dexie's per-offer model.

## Sequenced work, if we go

Nothing here is started.

- [ ] **C-01 Read the agent skills.** `perps-auth`, `perps-market-maker-auth`
      (incl. "How to win"), `perps-trader-bot-auth`, and the OpenAPI 3.1
      schemas. Settle the depth-seconds unit question. **Blocks everything.**
- [ ] **C-02 Decide the auth path.** OAuth trader bot (API key →
      `agent_session`) vs market-maker Sage/WalletConnect. MM prizes appear
      to require the MM path; confirm.
- [ ] **C-03 Read-only client + paper mode.** `/info/*` ingest and a
      WebSocket subscriber that logs what we *would* quote, scored against
      the real book. No orders. Establishes whether the strategy is
      competitive before any capital or auth complexity.
- [ ] **C-04 Perps position model.** Positions, margin, leverage, funding
      accrual, liquidation distance. The genuinely new domain work.
- [ ] **C-05 Execution adapter.** `order` / `modify` / `cancel` /
      `batch_upsert` / `cancel_all`, with the rate-limit token model
      respected.
- [ ] **C-06 Risk layer for perps.** Reuse the breakers; add liquidation
      proximity and a funding-cost budget. Note the equity-blindness bug
      class from 2026-08-25: never let a position price at 0 in silence.
- [ ] **C-07 Testnet dry run.** Faucet wUSDC.b, quote all three markets,
      measure actual depth-seconds accrual against the gate.

## Decisions the operator owns

- Whether to compete at all, once C-01 answers the depth-seconds question.
- Real capital or testnet only. `POST /exchange/faucet` suggests testnet is
  supported; whether prizes require mainnet is unconfirmed.
- Whether competition work is allowed to touch the live bot's shared code,
  or must live behind an adapter boundary. **Recommend the latter** — the
  live bot is mid-incident and should not absorb churn from an experiment.

## Sources

- <https://perps.permuto.capital> — API documentation page (in-app)
- <https://perps.permuto.capital/llms.txt> — agent discovery index
- `GET /info/meta` — market metadata, read 2026-08-26
