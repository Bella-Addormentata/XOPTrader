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

> **Read this whole document before enabling anything on mainnet.** The
> [testnet drill](#4-testnet-drill-hard-gate) is a **hard gate** — do it first.

---

## Contents

1. [Safety model](#1-safety-model)
2. [Prerequisites](#2-prerequisites)
3. [One-time setup](#3-one-time-setup)
4. [Testnet drill (hard gate)](#4-testnet-drill-hard-gate)
5. [Mainnet smoke test](#5-mainnet-smoke-test)
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
- **Disabled = dark.** When disabled (or when deps are missing, or the key is not
  configured) the background worker is a no-op and imports nothing heavy. The
  Warp tab simply shows *disabled* / *blocked* with the reason.
- **One active job at a time.** The bridge processes a single job end-to-end
  before starting another. A partial unique index in the job DB enforces this.
- **Stuck-not-lost.** Once the Base bridge transaction is attested by warp.green's
  validators, the message is **claimable forever**. Every failure mode leaves
  funds either still in the hot wallet or claimable — never destroyed. See
  [Recovery & sweep](#7-recovery--sweep).
- **Refuse-to-move anchors.** Before any funds move, the service re-derives the
  wrapped-asset TAIL and checks it equals the configured
  `expected_asset_id`; it checks the attested message destination and contents
  match what it precomputed. A mismatch **blocks** the service (visible in the
  banner) rather than moving funds.
- **Blast-radius caps.** `min_auto_bridge_usdc` / `max_auto_bridge_usdc` bound
  each automatic bridge. Manual "Bridge now" is always available regardless.
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

In `config.yaml`, set the [warp keys](#8-config--secrets-reference). To start,
leave `enabled: false` — you will turn it on for the testnet drill first. At
minimum review:

```yaml
warp:
  enabled: false
  testnet: false
  base_rpc_url: "https://mainnet.base.org"   # a private RPC is more reliable
  auto_bridge: false
  min_auto_bridge_usdc: 100
  max_auto_bridge_usdc: 10000
  claim_fee_mojos: 100000000                 # 0.0001 XCH reserved per claim
  chia_receiver_address: ""                  # blank = the bot's own wallet
  expected_asset_id: "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
```

### 3d. Fund the hot wallet

- Send **ETH on Base** to the ADDRESS from 3a for gas (≥ 0.005 ETH recommended).
- Do **not** send mainnet USDC yet — the [testnet drill](#4-testnet-drill-hard-gate)
  comes first.

### 3e. Restart the GUI

The GUI loads warp code and config at startup (it hard-kills the previous
instance on launch). **Restart it** after any code update or after enabling warp.

---

## 4. Testnet drill (hard gate)

**Do not enable mainnet until an end-to-end testnet bridge has completed.** The
testnet path exercises the exact same state machine and claim-bundle construction
as mainnet; a shape error shows up here for free instead of with real money.

Base Sepolia has no wired USDC, so the drill bridges **milliETH** instead — which
also exercises the 3-decimal / mojo-factor-1 edge case and the lower validator
threshold (3 of 10, vs 6 on mainnet).

1. Set `warp.testnet: true` and `warp.enabled: true` in `config.yaml`. Point
   `base_rpc_url` at a Base **Sepolia** RPC (e.g. `https://sepolia.base.org`), or
   leave it blank to use the network default.
2. Generate a **separate** testnet hot-wallet key (repeat 3a/3b). Fund it with
   Base Sepolia ETH from a faucet.
3. Make sure your Chia wallet is pointed at **testnet11** and has a little TXCH.
4. Restart the GUI, open the **Warp** tab. The banner should read *active on
   testnet11* with a **TESTNET** badge. Confirm the hot-wallet address and
   balances populate.
5. Click **Bridge now** (or send a small amount and let auto-bridge pick it up).
   Watch the job advance through the states:
   `AWAITING_DEPOSIT → DEPOSIT_SEEN → APPROVING → BRIDGING → BRIDGE_CONFIRMED →
   MESSAGE_SENT → FUNDING_CLAIM → CLAIM_FUNDED → COLLECTING_SIGS → CLAIMING →
   COMPLETED`.
6. Confirm the wrapped testnet asset arrives in the Chia wallet, and the job row
   ends **Completed**. Use the row's BaseScan / SpaceScan links to cross-check.

If the job sticks, see [Recovery & sweep](#7-recovery--sweep) and
[Troubleshooting](#9-troubleshooting). **Only proceed to mainnet once a testnet
bridge has reached COMPLETED.**

---

## 5. Mainnet smoke test

1. Set `warp.testnet: false`, keep `warp.enabled: true`, restore the mainnet
   `base_rpc_url`, and make sure the **mainnet** hot-wallet blob is in
   `secrets.yaml`. Restart the GUI.
2. Confirm the banner reads *active on mainnet* (no TESTNET badge) and balances
   populate. Leave `auto_bridge: false` for the smoke test.
3. Send a **small** amount of USDC on Base (~$5) to the hot-wallet address.
4. Click **Bridge now** and watch it run to **COMPLETED**, confirming wUSDC.b
   arrives in the Chia wallet.
5. Only after that succeeds, decide whether to enable `auto_bridge: true` and set
   your `min_auto_bridge_usdc` / `max_auto_bridge_usdc` caps. Restart to apply.

---

## 6. Day-to-day operation

- **Auto-bridge** (`auto_bridge: true`): whenever the hot wallet's USDC balance is
  at least `min_auto_bridge_usdc`, the service opens a job automatically and
  bridges up to `max_auto_bridge_usdc`. Deposit USDC from anywhere (Coinbase, a
  Circle payout, any wallet) and walk away.
- **Manual** ("Bridge now"): opens a single job for the current balance. Disabled
  while a job is already active (the button tooltip explains why).
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
  is how you get the XCH back. Sweep is best-effort and re-runnable.

A **FAILED** job deliberately holds the single active-job slot until you resolve
it (Retry or Sweep, then it clears), so a failure can't be silently buried under
a new job.

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
| `testnet` | `false` | `true` ⇒ Base Sepolia + testnet11 deployment. |
| `base_rpc_url` | `https://mainnet.base.org` | Base JSON-RPC endpoint. A private Alchemy/Infura key is more reliable. Blank ⇒ network default. |
| `auto_bridge` | `false` | Automatically bridge deposits at/above `min_auto_bridge_usdc`. |
| `min_auto_bridge_usdc` | `100` | Ignore auto-bridge below this (manual still works). |
| `max_auto_bridge_usdc` | `10000` | Cap per auto-bridge; bounds blast radius. |
| `claim_fee_mojos` | `100000000` | XCH fee reserved for the Chia claim spend (0.0001 XCH). |
| `chia_funding_fee_mojos` | `0` | Extra fee on the wallet→claim funding send. |
| `poll_interval_s` | `15` | Background tick cadence (seconds). |
| `coinset_url` | `""` | Override the coinset API base. Blank ⇒ network default. |
| `chia_receiver_address` | `""` | Where wUSDC.b lands. Blank ⇒ the bot's own wallet address. |
| `expected_asset_id` | `fa4a180a…a6b7a99d` | Correctness anchor: the derived wrapped-asset id must equal this or the service refuses to move funds. |

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
| Validator threshold | 6 of 10 (testnet: 3) |
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
- **Banner: "blocked: expected asset id mismatch"** — the derived wrapped-asset id
  doesn't equal `expected_asset_id`. This is the anchor doing its job; funds do
  **not** move. Verify `expected_asset_id` and `testnet` are correct for the
  network you intend.
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
  **warp.green's own web UI**, which pays the attested Chia receiver.

Also out of scope for this version: unwrap (Chia → Base), the Ethereum-mainnet
path (that mints a *different* CAT, wUSDC — this bridge is the **Base**-bridged
wUSDC.b), non-USDC tokens, and any Circle Mint API integration.
