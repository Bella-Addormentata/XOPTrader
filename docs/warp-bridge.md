# Warp Bridge runbook — USDC (Base) → wUSDC.b (Chia)

Automatic, background bridging of USDC on **Base** into **wUSDC.b** (the Chia CAT
`fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d`, the bot's
primary quote asset) via [warp.green](https://warp.green). You fund an
app-controlled Base hot wallet; XOPTrader detects the deposit and performs the
whole bridge on its own — approve + `bridgeToChia` on Base, wait for the
validator attestation, collect the validator BLS signatures from Nostr, then
build and push the Chia claim spend. wUSDC.b lands in the bot's Chia wallet.

This is a **GUI-only** feature. The C++ trading engine never reads any of it and
is completely untouched.

> **Read this whole document before enabling anything.** The
> [dry-run rehearsal](#4-dry-run-rehearsal-hard-gate) is a **hard gate** — do it
> first. This bridge is **mainnet-only**; there is no testnet mode (see
> [Why there is no testnet](#41-why-there-is-no-testnet)).

---

## Contents

1. [Safety model](#1-safety-model)
2. [Prerequisites](#2-prerequisites)
3. [One-time setup](#3-one-time-setup)
4. [Dry-run rehearsal (hard gate)](#4-dry-run-rehearsal-hard-gate)
5. [First live bridge (small amount)](#5-first-live-bridge-small-amount)
6. [Day-to-day operation](#6-day-to-day-operation)
7. [Recovery & sweep](#7-recovery--sweep)
8. [Config & secrets reference](#8-config--secrets-reference)
9. [Troubleshooting](#9-troubleshooting)
10. [Known limitations](#10-known-limitations)

---

## 1. Safety model

- **Off by default.** `warp.enabled: false` and `warp.auto_bridge: false` ship as
  the defaults. Merging this PR changes nothing for the live bot until you
  explicitly turn it on and restart the GUI.
- **Dry run by default.** `warp.dry_run` defaults to **true**. In dry run the
  bridge does everything except broadcast: it resolves the receiver, reads the
  live message toll and bridge tip, estimates gas, and encodes *and signs* both
  Base transactions — then ends the job at `DRY_RUN_OK` without sending. Turning
  `enabled: true` on by itself cannot move funds.
- **Disabled = dark.** When disabled (or when deps are missing, or the key is not
  configured) the background worker is a no-op and imports nothing heavy. The
  Warp tab simply shows *disabled* / *blocked* with the reason.
- **One active job at a time.** The bridge processes a single job end-to-end
  before starting another. A partial unique index in the job DB enforces this.
- **Stuck-not-lost.** Once the Base bridge transaction is attested by warp.green's
  validators, the message is **claimable forever**. Every failure mode leaves
  funds either still in the hot wallet or claimable — never destroyed. See
  [Recovery & sweep](#7-recovery--sweep).
- **Refuse-to-start anchor.** At **engine construction**, before a single client
  is built or a single wei moves, the service re-derives the wrapped-asset TAIL
  offline and checks it against both the deployment constant and your configured
  `expected_asset_id`. A mismatch **blocks the engine** with a banner. During the
  claim it re-checks the same anchor against the *attested* token contract, plus
  the attested message destination and contents. A mismatch at any point refuses
  rather than moving funds.
- **Blast-radius caps.** `max_auto_bridge_usdc` caps every bridge, automatic or
  manual — a mis-click on a large balance bridges at most the cap, and the
  remainder stays in the hot wallet. `min_auto_bridge_usdc` is an *auto-bridge
  floor only*: "Bridge now" deliberately ignores it, so you can test with a
  small deposit without lowering the threshold that arms auto-bridge.
- **One hot wallet per job.** A job records the network and hot-wallet address it
  was frozen against. If the configured key changes, the engine refuses to
  process that job and says so in the banner, rather than signing wallet B's
  funds for wallet A's frozen amounts.
- **Key at rest.** The EVM hot-wallet private key is stored **DPAPI-encrypted**
  (Windows `CryptProtectData`, current-user scope + app entropy) in
  `secrets.yaml` (gitignored), never in `config.yaml` and never in the job DB. A
  stolen `secrets.yaml` row is useless on another machine or under another
  Windows account.

---

## 2. Prerequisites

- **Windows.** At-rest key encryption uses DPAPI. The service refuses to run on
  non-Windows platforms rather than store a key in plaintext.
- **The XOPTrader GUI venv** (`.venv\Scripts\python.exe`, Python 3.13) with the
  warp dependencies installed (they ship in `pyproject.toml [gui]` and
  `gui/requirements.txt`): `web3`, `clvm`, `chia_rs`, `websocket-client`. If you
  built the GUI env before this PR, reinstall: `.venv\Scripts\pip install -r gui\requirements.txt`.
- **A running, unlocked Chia wallet.** The bridge funds the Chia claim from the
  bot's own wallet via the wallet RPC, reusing your existing `chia:` settings
  (`wallet_host`, `wallet_port`, `wallet_fingerprint`, `wallet_cert_path`,
  `wallet_key_path`). No new Chia config is needed. wUSDC.b lands in this wallet.
- **A little XCH** in that wallet for the claim fee (default 0.0001 XCH per claim,
  `claim_fee_mojos`) plus the attested amount, which is sent to the ephemeral
  claim coin and comes back to you as wUSDC.b.
- **A Base RPC endpoint.** The public `https://mainnet.base.org` works but rate
  limits; a free Alchemy/Infura Base key in `base_rpc_url` is more reliable.
- **A little ETH on Base** in the hot wallet for gas: each bridge spends the
  Portal message toll (currently 0.00001 ETH, read live) plus gas for the ERC-20
  `approve` and `bridgeToChia`. Keep ≥ 0.005 ETH; the tab warns below 0.001 ETH.

### 2a. Run the tests first

**Do this before the [dry run](#4-dry-run-rehearsal-hard-gate), on a machine with
the GUI dependencies installed.** CI runs `python -m compileall` only, so the warp
test suite is not executed anywhere automatically:

```bash
.venv/Scripts/python.exe -m pytest gui/services/warp/tests tests/test_warp_widget.py -q
```

Every module `importorskip`s `clvm` and `chia_rs`, and the widget tests need
`PySide6` — if those are missing the suite reports skips or collection errors
rather than passing, so check the summary line says what you expect.

Three defects have already been found that no amount of static review would have
caught, each of which alone prevented any bridge from completing: a `0x`-prefixed
watcher nonce (HTTP 500), a `bytes32`-vs-20-byte token comparison that could never
be equal, and coinset "not found" responses being read as successes. All three
were invisible to the tests because the fixtures encoded the same wrong
assumptions as the code. Treat a green suite as necessary, not sufficient — and
note the dry run does not exercise any of them, since all three sit past the
broadcast point it stops at.

---

## 3. One-time setup

### 3a. Generate (or import) the hot-wallet key

This version has **no in-GUI key generator** (see
[Known limitations](#10-known-limitations)). You create the DPAPI blob once, by
hand, with the shipped keystore helpers.

> **Run this on the same Windows user account that runs the GUI.** DPAPI binds
> the ciphertext to your Windows account — a blob generated under a different
> account (or on another machine) will fail to decrypt at runtime.

From the repository root (the folder containing `gui\`), run the venv Python:

```bash
.venv/Scripts/python.exe -c "from gui.services.warp import keystore; k = keystore.new_evm_key(); print('ADDRESS  :', k.address); print('BACKUP   : 0x' + k.private_key.hex()); print('DPAPI    :', keystore.protect_evm_key(k))"
```

To **import** an existing key instead of generating a fresh one, replace
`keystore.new_evm_key()` with `keystore.evm_key_from_hex("0x...")`.

It prints three things:

- **ADDRESS** — the Base address you will fund with ETH (gas) and USDC.
- **BACKUP** — the raw private key. **Write it down offline now**, then never
  again. This is your only recovery if the DPAPI blob is ever lost (e.g. Windows
  profile rebuild). Anyone with it controls the hot wallet — treat it like cash.
- **DPAPI** — the encrypted blob to paste into `secrets.yaml` (next step).

Clear your terminal scrollback afterward so the BACKUP line isn't left on screen.

### 3b. Store the blob in `secrets.yaml`

Open `secrets.yaml` (the sibling of your `config.yaml`; it is gitignored) and add:

```yaml
warp:
  evm_private_key_dpapi: "<paste the DPAPI value here>"
```

The config loader deep-merges `secrets.yaml` over `config.yaml`, and
`warp.evm_private_key_dpapi` is registered in `SECRET_KEYS`, so it is always
routed to `secrets.yaml` on save and never written to `config.yaml`. Do **not**
put the key in `config.yaml`.

### 3c. Configure the `warp:` section

In `config.yaml`, set the [warp keys](#8-config--secrets-reference). Start with
the rehearsal settings — `dry_run: true` and a small cap:

```yaml
warp:
  enabled: true
  dry_run: true                              # sign, never broadcast
  base_rpc_url: "https://mainnet.base.org"   # a private RPC is more reliable
  auto_bridge: false                         # manual only; nothing fires on its own
  min_auto_bridge_usdc: 100                  # auto-bridge floor; Bridge now ignores it
  max_auto_bridge_usdc: 5                    # HARD cap, manual included — raise later
  claim_fee_mojos: 100000000                 # 0.0001 XCH reserved per claim
  chia_receiver_address: ""                  # blank = the bot's own wallet
  expected_asset_id: "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
```

Set `expected_asset_id` deliberately. It is now cross-checked against the offline
derivation at startup, so a typo blocks the bridge with a banner instead of being
silently ignored.

### 3d. Fund the hot wallet

- Send **ETH on Base** to the ADDRESS from 3a for gas (≥ 0.005 ETH recommended).
- Send **$5 of USDC on Base**. The dry run needs a real balance to freeze real
  amounts against, and with `dry_run: true` it cannot be spent.

### 3e. Restart the GUI

The GUI loads warp code and config at startup (it hard-kills the previous
instance on launch). **Restart it** after any code update or after enabling warp.

---

## 4. Dry-run rehearsal (hard gate)

**Do not set `dry_run: false` until a dry run has reached `DRY_RUN_OK`.** The dry
run exercises every part of the deposit leg for real — the DPAPI key, the Base
RPC, the Chia wallet daemon, the receiver address decode, the live message toll
and bridge tip, gas estimation, nonce selection, ABI encoding, and transaction
signing. The only thing it does not do is broadcast.

1. With the [3c](#3c-configure-the-warp-section) config in place, restart the GUI
   and open the **Warp** tab. The banner should read *dry run* with a **DRY RUN**
   badge. Confirm the hot-wallet address, the USDC/ETH balances, the destination
   Chia address, and the wUSDC.b asset id all populate.
2. Click **Bridge now**. Watch the job advance:
   `AWAITING_DEPOSIT → DEPOSIT_SEEN → APPROVING → BRIDGING → DRY_RUN_OK`.
3. Check the frozen numbers on the row: USDC in should be exactly your test
   amount (clamped to `max_auto_bridge_usdc` if lower), and the wUSDC.b out
   figure should be that amount less the 0.3% bridge tip.
4. Confirm the destination address matches the Chia wallet you expect.

Nothing was broadcast, so there is no abort path to know — if anything looks
wrong, set `warp.enabled: false` and walk away. That is the whole point.

If the job sticks or the banner reads *blocked*, see
[Troubleshooting](#9-troubleshooting).

### 4.1 Why there is no testnet

Earlier versions documented a Base Sepolia / testnet11 drill. It was removed
because it could not do what it claimed:

- Base Sepolia has **no wired USDC**, and the EVM client only builds ERC-20
  `approve`/`bridgeToChia` calls — so a testnet job could never leave
  `AWAITING_DEPOSIT`, while still occupying the single job slot.
- The testnet deployment shipped with an **empty `expected_asset_id`**, which
  disabled the one anchor that refuses to move funds. The drill therefore
  exercised *less* safety than production while claiming to be identical.
- Sharing one job database across two networks made it possible to resume a
  mainnet job under testnet contracts, or vice versa.

The dry run replaces it and is a strictly better rehearsal for the deposit leg:
same network, same contracts, same key, same anchors, zero broadcast. Setting
`warp.testnet: true` now raises a clear configuration error rather than silently
running against mainnet.

---

## 5. First live bridge (small amount)

Only after a clean `DRY_RUN_OK`.

1. Set `warp.dry_run: false` in `config.yaml`. Leave `auto_bridge: false` and
   `max_auto_bridge_usdc: 5`. Restart the GUI.
2. Confirm the banner reads *active on mainnet* with no DRY RUN badge.
3. Click **Bridge now** on the same $5. Watch the full state walk:
   `AWAITING_DEPOSIT → DEPOSIT_SEEN → APPROVING → BRIDGING → BRIDGE_CONFIRMED →
   MESSAGE_SENT → FUNDING_CLAIM → CLAIM_FUNDED → COLLECTING_SIGS → CLAIMING →
   COMPLETED`.
4. Check at each stage: the approve and bridge transactions confirm on BaseScan;
   `MESSAGE_SENT` attests the exact post-tip amount; `FUNDING_CLAIM` sends
   **exactly one** funding transaction (verify in your Chia wallet); signatures
   reach 6 of 10; the wUSDC.b lands at the receiver on SpaceScan.
5. Only after that succeeds, raise `max_auto_bridge_usdc` incrementally. Leave
   `auto_bridge: false` for at least one more manual round-trip before enabling
   it. Restart to apply any change.

**Abort paths.** Before the bridge transaction is broadcast (`AWAITING_DEPOSIT`,
`DEPOSIT_SEEN`, `APPROVING`) right-click the job and **Cancel** — clean, nothing
on chain. Once `bridgeToChia` broadcasts you **cannot** abort: the message exists
and must be claimed. Let it run; if it fails, **Retry**, and if Retry cannot
progress, **Sweep** recovers the XCH funding coin and closes the job. The attested
message stays claimable forever — through warp.green's own portal if necessary
(the **Open warp.green portal** button on this tab).

---

## 6. Day-to-day operation

- **Auto-bridge** (`auto_bridge: true`): whenever the hot wallet's USDC balance is
  at least `min_auto_bridge_usdc`, the service opens a job automatically and
  bridges up to `max_auto_bridge_usdc`. Deposit USDC from anywhere (Coinbase, a
  Circle payout, any wallet) and walk away.
- **Manual** ("Bridge now"): opens a single job for the current balance,
  **ignoring `min_auto_bridge_usdc`** but still honouring `max_auto_bridge_usdc`.
  That is what lets you test with $5 without lowering the threshold that arms
  auto-bridge. Disabled while a job is already active — including a **failed**
  one, which holds the slot until you Retry, Sweep or Abandon it. The button
  tooltip says
  which case applies.
- **Where funds land:** `chia_receiver_address` if set, otherwise the bot's own
  Chia wallet address.
- **The jobs table** shows status, USDC in / wUSDC.b out, last-updated, and the
  Base tx. Right-click a row for actions and explorer links. Hover a failed
  row's status for the error.
- **Cadence:** the background worker ticks roughly every 30 s and advances the
  active job one bounded step per tick. Bridges are not instant — the Base
  confirmations, validator attestation, and Nostr signature collection each take
  their own time. This is normal; the job simply progresses across ticks.

---

## 7. Recovery & sweep

Every job carries an **ephemeral BLS key** (also DPAPI-wrapped, in the job row)
that controls the coin used to fund its Chia claim. That key is retained until
the claim's security coin is provably spent, which is what makes every failure
recoverable.

Right-click a job in the table:

- **Retry** — only for a **FAILED** job. Resumes from the step it failed at.
  Transient problems (RPC hiccups, "not attested yet", incomplete signatures,
  portal advanced, low gas) are handled automatically with backoff and don't need
  this; Retry is for a job that gave up.
- **Cancel** — only before the Base bridge is broadcast (`AWAITING_DEPOSIT`,
  `DEPOSIT_SEEN`, `APPROVING`). Once bridged, the message exists on-chain and must
  be claimed, not cancelled.
- **Sweep security coin** — for a terminal job (**COMPLETED** or **FAILED**).
  Recovers the ephemeral funding coin back to your Chia wallet. On a COMPLETED job
  this reclaims any leftover; on a FAILED job where funding already happened, this
  is how you get the XCH back. Sweep is re-runnable.

A **FAILED** job deliberately holds the single active-job slot until you resolve
it, so a failure can't be silently buried under a new job. **Sweep is the usual escape:**
a sweep that *resolves* — the coin was recovered, was already spent, or never
existed — moves the job to **Cancelled** and frees the slot. A sweep that could
not reach the chain leaves the job FAILED so you can try again with the funds
still known recoverable.

Some failures cannot be swept at all. The three attested-terms anchors run
*after* `bridgeToChia` confirms and *before* the ephemeral key is minted, so a
job that trips one has a bridge nonce and no security coin: Sweep raises,
Cancel is refused, and Retry re-fails against the same immutable attestation.
For those, use **Abandon job**. It records the unclaimed nonce and the portal
recovery details in the job's audit log, then closes the job and frees the
slot. No funds are lost: the message stays claimable at the portal, which is
exactly why the details are written down before the row is closed.

**The core guarantee:** an attested warp.green message is claimable forever. If a
job is stuck after the bridge confirmed, the USDC is not lost — it is waiting to
be claimed. Retry drives the claim; if all else fails, the message can still be
claimed through warp.green's own UI (see [Known limitations](#10-known-limitations)).

---

## 8. Config & secrets reference

### `config.yaml` → `warp:`

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | Master switch. Off ⇒ the bridge is completely dark. |
| `dry_run` | **`true`** | Sign both Base transactions but never broadcast; the job ends at `DRY_RUN_OK`. Set `false` only after a clean rehearsal. |
| `base_rpc_url` | `https://mainnet.base.org` | Base JSON-RPC endpoint. A private Alchemy/Infura key is more reliable. Blank ⇒ network default. |
| `auto_bridge` | `false` | Automatically bridge deposits at/above `min_auto_bridge_usdc`. |
| `min_auto_bridge_usdc` | `100` | **Auto-bridge floor only.** "Bridge now" ignores it and bridges any positive balance. |
| `max_auto_bridge_usdc` | *required* | Blast-radius cap. Applies to **manual bridges too**; the excess stays in the hot wallet. There is no default: with `warp.enabled: true` the bridge refuses to start unless this is set, because an absent value previously meant *no cap*, not `10000`. Set `unlimited` to allow any balance. |
| `claim_fee_mojos` | `100000000` | XCH fee reserved for the Chia claim spend (0.0001 XCH). |
| `chia_funding_fee_mojos` | `0` | Extra fee on the wallet→claim funding send. |
| `poll_interval_s` | `15` | How long a healthy "still waiting" step sleeps before it is re-checked. The GUI's own tick cadence (~30 s) is separate and not configurable here. |
| `coinset_url` | `""` | Override the coinset API base. Blank ⇒ network default. |
| `chia_receiver_address` | `""` | Where wUSDC.b lands. Blank ⇒ the bot's own wallet address, resolved once at startup and shown in the tab. |
| `expected_asset_id` | `fa4a180a…a6b7a99d` | Correctness anchor, checked **at startup** before any client is built. A mismatch blocks the engine with a banner. |

`testnet` is **no longer supported**. A truthy `warp.testnet` raises a
configuration error rather than silently running against mainnet — remove the key.

Reused from your existing `chia:` section (no new keys): `wallet_host`,
`wallet_port`, `wallet_fingerprint`, `wallet_cert_path`, `wallet_key_path`.

### `secrets.yaml` → `warp:`

| Key | Meaning |
|---|---|
| `evm_private_key_dpapi` | Base64 of the DPAPI-encrypted EVM hot-wallet private key. Generated in [3a](#3a-generate-or-import-the-hot-wallet-key). Never commit; never place in `config.yaml`. |

### Fixed protocol facts (mainnet, for reference)

| | |
|---|---|
| Base Portal | `0x382bd36d1dE6Fe0a3D9943004D3ca5Ee389627EE` |
| Base ERC20Bridge | `0x8412f06e811b858Ea9edcf81a5E5882dbf70aC96` |
| USDC (Base, 6 dec) | `0x833589fcd6edb6e08f4c7c32d4f71b54bda02913` |
| Message toll | 0.00001 ETH (owner-mutable; read live each bridge) |
| Validator threshold | 6 of 10 |
| wUSDC.b (Chia CAT, 3 dec) | `fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d` |
| Bridge tip | 0.3% deducted in-message (min 1 mojo) |

---

## 9. Troubleshooting

- **Banner: "disabled"** — `warp.enabled` is not `true`, or you didn't restart the
  GUI after changing it.
- **Banner: "blocked: no EVM hot-wallet key configured"** — `secrets.yaml` is
  missing `warp.evm_private_key_dpapi`, or the blob doesn't decrypt (generated
  under a different Windows account/machine). Regenerate on the correct account
  ([3a](#3a-generate-or-import-the-hot-wallet-key)).
- **Banner: "blocked: …DPAPI…" / KeystoreUnavailable** — you're not on Windows, or
  DPAPI failed. The bridge requires Windows.
- **Banner: "blocked: wrapped-asset anchor failed"** — the offline derivation
  doesn't match `expected_asset_id` (or the deployment constant). This is the
  anchor doing its job at startup; nothing was built and no funds can move. Fix
  `expected_asset_id` in `config.yaml` — the Base-bridged wUSDC.b id is
  `fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d`.
- **Banner: "blocked: warp.testnet is no longer supported"** — remove the
  `testnet` key from `config.yaml`; see [4.1](#41-why-there-is-no-testnet).
- **Banner: a hot-wallet binding error** — the job database holds a job frozen
  against a different Base hot wallet (you rotated the key, or copied a job DB).
  Restore the original key and resolve or sweep that job before rotating. A job
  that never reached the chain can still be cancelled.
- **"Bridge now" is greyed out with a failed job in the table** — that failed job
  holds the single active-job slot by design. Retry it, or Sweep it to recover
  the funding coin and close it. If the job has no security coin to sweep
  (an attested-terms anchor failed), use **Abandon job** — Sweep and Retry
  cannot resolve that one, and it would otherwise hold the slot forever.
- **Balances show "unavailable"** — the Base RPC is unreachable or rate-limiting.
  Set a private `base_rpc_url`.
- **Low-gas warning** — top up the hot wallet's ETH on Base.
- **Job stuck at COLLECTING_SIGS** — validators haven't all published yet, or the
  portal singleton advanced (the service re-collects automatically). If it stays
  stuck long-term, a wrong signed-digest would make every signature fail local BLS
  verification; check `status.warp.green` for validator health.
- **Job FAILED** — hover the status for the reason, then Retry or Sweep
  ([Recovery & sweep](#7-recovery--sweep)).

Blocked reasons come from startup anchors, so warp.green operational drift
(validator rotation, portal redeploy) turns into a **visible** blocked state, not
a mid-job surprise.

---

## 9b. Unwrap (Chia -> Base)

The return leg burns wUSDC.b and releases native USDC on Base. Operator-only:
no auto-unwrap exists, and `warp.max_unwrap_usdc` must be set (or `unlimited`,
stated explicitly) before the Unwrap button does anything.

* **The commit point is the `cat_spend`.** Every gate -- burn anchors (offline
  and live), the EIP-712 domain, wallet resolution, spendable balance, live
  signature threshold, bridge liquidity, relay gas, toll funds -- runs before
  it. After it the unwrap is **forward-only**: the receiver is curried into
  the burn puzzle, so nothing can redirect the funds, including us. The
  failure mode is "stuck pending a relay", never "stolen".
* **Costs**: the 0.3% warp tip (scale-then-tip: 0.001 USDC in pays 997
  micro-USDC out) + the 0.001 XCH toll (burned as the bundle's fee) + Base
  relay gas (~145k measured).
* **Dry run**: with `warp.dry_run: true` an unwrap exercises every gate and
  closes DRY_RUN_OK without sending the `cat_spend`. **The first live unwrap
  should be 0.001 USDC** -- the smallest possible -- and treated as the
  rehearsal for the one step no test has ever executed.
* **Spendable vs confirmed**: the daemon's default coin selection honours
  offer locks, so an unwrap waits while the balance is offer collateral
  rather than double-spending it. The gate pends with the difference shown.
* **Recovery**: a FAILED unwrap offers Retry (safe: the relay is idempotent,
  '!nonce' means someone already delivered and completes the job), Sweep (the
  toll coin only), and Abandon (records the nonce and recovery details; the
  attested message stays deliverable forever). Whether the portal explorer's
  "Complete Relay" works for xch->bse is **unverified** -- until it is, keep
  ETH on Base for our own relay.
* **P&L**: the engine has no transfer-out event yet, so an unwrap appears as
  a wUSDC.b balance drop: expect one LedgerDivergence alert and one negative
  `adjust` per unwrap. Cross-check any divergence against `warp_jobs.db`
  (both directions) before dismissing it; `SUM(adjust)` now overstates
  "unexplained" by the sum of deliberate unwraps. A proper `transfer` ledger
  event is a follow-up PR.

## 10. Known limitations

This PR wires the fully-automatic bridge and its live monitor. Two operator
affordances from the original design are **deferred** to this runbook because the
committed engine exposes only `tick` / `request_bridge` / `job_action` /
`update_config` — there is no GUI method backing them yet:

- **In-GUI key generation / import.** There is no one-click "Generate key" dialog
  with an on-screen backup reveal. Instead, generate the key once with the venv
  snippet in [3a](#3a-generate-or-import-the-hot-wallet-key) and back up the raw
  key yourself.
- **Claim-by-Base-tx-hash recovery.** There is no in-app "claim this tx hash"
  box. The app only claims bridges it initiated (it tracks the job from deposit
  onward). If USDC is bridged out-of-band, or a job row is lost before the claim,
  the attested message is still claimable forever — recover it through
  **warp.green's own web UI**, which pays the attested Chia receiver. The **Open
  warp.green portal** button on the Warp tab is that path; it stays enabled even
  when the engine is blocked, which is exactly when you need it.

### Third-party-claim detection is conservative

If someone else claims your attested message first, they pay the *same* attested
receiver, so your deposit still lands and only your XCH funding needs sweeping
back. The service detects this by checking whether the message coin for your
specific nonce has been **spent** — spending it is what forces the mint to the
attested receiver, so mere existence is not proof and is not accepted.

The check is deliberately conservative in both directions. The message coin has
`amount = 0`, so if the coinset index omits zero-amount coins it reads as "not
claimed". Either way the job stays in `CLAIMING` and re-syncs rather than
auto-completing. There is no Sweep action while a job is in `CLAIMING`, by
design — sweeping the security coin under a claim that might still land would
break it. After 10 conflicting rounds with no evidence the mint ran, the job
fails terminally, which is what makes **Sweep** available to recover the ~0.1 XCH
funding coin. Check SpaceScan: if the wUSDC.b did arrive, a third party claimed
it and only the funding coin needs recovering.

This replaced a heuristic that compared a count of *all* the receiver's wUSDC.b
coins against a baseline, which the bot's own trading could trip, wrongly marking
an unclaimed bridge as completed.

### A job that fails after the bridge confirms cannot be closed in-app

Three attested-terms anchors fire *after* `bridgeToChia` has confirmed but before
the claim key exists — for example if warp.green's tip changes between the quote
and execution, so the attested amount no longer matches what was frozen. Such a
job holds a real, unclaimed deposit and has no funding coin to sweep, so **Sweep
refuses to close it** and says so, naming the nonce. That is deliberate: closing
it would discard the only in-app record of live funds. Recover the message
through the warp.green portal, which pays the attested Chia receiver.

### The bridge is one-way

There is no unwrap (Chia → Base) in this version. To move wUSDC.b back to native
USDC on Base, use warp.green's own portal — the **Open warp.green portal** button
on the Warp tab. What building an in-app unwrap would require is scoped in
[warp-unwrap-design.md](warp-unwrap-design.md); note that the return leg's commit
point is irreversible in a way the deposit leg's is not, so it is not a mirror
image of this one.

Also out of scope for this version: the Ethereum-mainnet path (that mints a
*different* CAT, wUSDC — this bridge is the **Base**-bridged wUSDC.b), non-USDC
tokens, and any Circle Mint API integration.
