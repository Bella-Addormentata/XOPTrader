# Warp unwrap (Chia → Base) — requirements and design notes

**Status: IMPLEMENTED (2026-08-09), with the corrections below.** The design
survived contact with reality well; where execution disagreed with this
document, execution won:

* **The minimum viable unwrap is 0.001 USDC (1 mojo), not 0.002** (SS8): the
  ERC20Bridge scales mojos to base units FIRST and then takes the 30 bps tip
  -- proven against two real unwraps. 1 mojo pays the receiver 997 base units.
* **Relay gas measured 145,195** on a real delivery (SS4 guessed ~160k).
* **The domain separator getters all revert** on the deployed Portal; the live
  read is EIP-5267 `eip712Domain()`, and the reconstructed separator is
  anchored in tests and verified by recovering a real relay's six signatures.
* **Five states, not seven** (SS6.2): UNWRAP_CHECKS -> BURN_SENT -> BURNING ->
  COLLECTING_EVM_SIGS -> RELAYING, sharing the closed states. No `direction`
  column (SS5): the status vocabulary is the discriminator and direction rides
  in JSON state, per the store's own evolving-payload rule. The single
  active-slot index stays table-wide (Q4 settled).
* **Two transactions, not one five-spend bundle** (SS2.1): the daemon's
  `cat_spend` first (THE commit point), then a four-spend bundle (security,
  burn CAT, cat_burner, bridging). The burn<->message atomicity lives inside
  the bundle, so the structural guarantee holds unchanged.
* Q1 settled: daemon 2.7.3 exposes `cat_spend`; the wUSDC.b wallet resolves
  by TAIL at runtime. Q2 settled in-tree: the burn hash derives offline and
  matches the live contract. Q3 (portal-explorer relay for xch->bse) remains
  UNVERIFIED and the manual-fallback story still rests on it. Q6 (ledger):
  an unwrap books as engine drift -- see the runbook; a proper transfer event
  is a follow-up.
* **What remains unexecuted anywhere: the `cat_spend` itself.** Every gate,
  bundle construction, digest, signature and relay encoding is pinned against
  real mainnet data, but no one has ever run the commit point. The first live
  action must be the 0.001-USDC micro-unwrap rehearsal.


The shipped bridge (see [warp-bridge.md](warp-bridge.md)) is one-way: USDC on Base →
wUSDC.b on Chia. This document describes what the return leg — wUSDC.b → native USDC
on Base — would require, so the decision to build it can be made with real numbers
rather than a guess.

Until it is built, the return trip is done manually through
[warp.green's own portal](https://www.warp.green), which the Warp tab links to.

> **Evidence standard.** Facts here were read from warp.green's published Chialisp and
> Solidity, or read live from Base mainnet, and are marked **[V]**. Inferences are
> marked **[?]** with a note on how to settle them. Do not treat a **[?]** as
> settled during implementation — several of them change the design shape.

---

## Contents

1. [Fix this first](#1-fix-this-first)
2. [How the unwrap works](#2-how-the-unwrap-works)
3. [The commit point](#3-the-commit-point)
4. [Costs and who pays](#4-costs-and-who-pays)
5. [What we can reuse](#5-what-we-can-reuse)
6. [What has to be built](#6-what-has-to-be-built)
7. [Interaction with the trading engine](#7-interaction-with-the-trading-engine)
8. [Failure and recovery](#8-failure-and-recovery)
9. [Open questions that change the design](#9-open-questions-that-change-the-design)
10. [Effort and sequencing](#10-effort-and-sequencing)

---

## 1. Fix this first

**A bug in the *inbound* path surfaced during this research and is fixed in the same
change as this document.** `watcher.py` sent the message nonce `0x`-prefixed. The
watcher API returns HTTP 500 for that, on both source chains — verified live:

```bash
curl -s -o /dev/null -w "%{http_code}\n" "https://watcher-api.warp.green/messages?source_chain=bse&nonce=00000000000000000000000000000000000000000000000000000000000001d7"
```

Bare hex returns 200; the same request with `0x` returns 500. Since
`_default_getter` calls `raise_for_status()`, **every attestation poll raised**, so
no inbound bridge could ever leave `MESSAGE_SENT`. The fix is one line
(`_0x(nonce)` → `_hx(nonce)`), and it means the inbound leg has evidently never been
run against the live watcher. Weigh that when planning the first live test.

---

## 2. How the unwrap works

### 2.1 Chia side — burn, don't send **[V]**

There is no "send wUSDC.b to the bridge" transaction. The CAT is **destroyed**, and
its destruction emits a message the validators observe. Five coin spends, one atomic
bundle:

| # | Coin | Puzzle | Role |
|---|---|---|---|
| 1 | security coin | `p2_delegated_puzzle_or_hidden_puzzle` curried with an ephemeral BLS pubkey | creates the toll coin; `ASSERT_CONCURRENT_SPEND` on it |
| 2 | source wUSDC.b CAT | `cat_v2(…, bot inner puzzle)` | ordinary CAT spend; sole output is a coin at the **burn inner puzzle hash** |
| 3 | burn CAT (child of #2) | `cat_v2(…, burn_inner_puzzle)` | emits CAT-v2 magic `(51 () -113 tail_reveal tail_solution)` → burns itself |
| 4 | `cat_burner` coin | `cat_burner` (static, keyless) | emits the message: `CREATE_COIN BRIDGING_PUZZLE_HASH toll (memos…)` |
| 5 | bridging coin (child of #4) | `bridging_puzzle` | `ASSERT_MY_AMOUNT` + `RESERVE_FEE my_amount` |

Three things worth internalising:

**The Chia portal singleton is not spent.** It is a receiver only. Validators
discover outbound messages by polling
`get_coin_records_by_puzzle_hash(BRIDGING_PUZZLE_HASH)`. The **nonce is the bridging
coin's id**, which means it is computable offline *before* you push — you never wait
to be told what to poll for.

**Burn and message are mutually atomic.** `burn_inner_puzzle` and `cat_burner`
cross-assert each other's coin announcements, and `cat_burner` re-derives the burn
CAT's full coin id from scratch using its own puzzle hash and amount as the burn
inner puzzle's curry arguments. **"CAT destroyed, no message emitted" is structurally
impossible**, and message contents cannot disagree with the burned coin.

**The destination EVM address is committed three times** — curried into the burn inner
puzzle as `RECEIVER`, committed by the wrapped TAIL, and supplied in the `cat_burner`
solution. Any disagreement fails the spend.

### 2.2 Base side — anyone can relay **[V]**

Two calls, verified against deployed bytecode and live `eth_call`s:

- `Portal.receiveMessage(bytes32,bytes3,bytes32,address,bytes32[],bytes)` — selector
  `0xb2e7bebb`, **not payable**
- `ERC20Bridge.receiveMessage(bytes32,bytes3,bytes32,bytes32[])` — selector
  `0x574632fc`, called by the Portal

The Portal verifies EIP-712 signatures:

- `MESSAGE_TYPE_HASH = keccak256("Message(bytes32 nonce,bytes3 source_chain,bytes32 source,address destination,bytes32[] contents)")`
  = `0x9972dc9e80132460f6459b361feb003781068b85cac2d95d54bc2150f439b824`
- `require(_sigs.length == signatureThreshold * 65)` — **exact, not a minimum**
- Signature layout is **v‖r‖s** (v at +0, r at +1, s at +33), not the usual r‖s‖v
- `require(isSigner[signer])` then `require(signer > lastSigner, "!order")` — the
  recovered addresses must be in **ascending** order

Live reads from Base mainnet: `signatureThreshold()` = **6**;
`ERC20Bridge.burnPuzzleHash()` = `0x6d64cf902916f73b90fa0a6412c7d1b43996c04fb3f245fcc2d767aa556c93a1`;
`otherChain()` = `"xch"`; `tip()` = **30 bps**, `immutable`.

**The digest binds nothing mutable.** Unlike the inbound leg — where a portal
singleton advance invalidates collected signatures and forces re-collection —
**outbound signatures never expire**. That removes an entire class of retry logic.

One important asymmetry: `usedNonces[key] = true` is written *before* the external
call, with no try/catch. So a bridge-side revert (insufficient bridge liquidity, or
`!amnt` on a dust amount) **rolls back the whole transaction including the nonce
write**. Failed relays are retryable; only a `"!nonce"` revert means already-delivered.

### 2.3 Attestation

Validators sign with **ECDSA secp256k1** over the EIP-712 digest and publish to the
same Nostr relays the inbound leg already uses, as a kind-1 note whose content is the
bech32m-encoded signature. The routing tag is `["r", bech32m(src‖dst‖nonce)]`; a `["c", ""]`
tag is present but empty for EVM destinations, so **filter on `#r` only**.

**The validator, not us, builds the EVM message contents.** `xch_follower` left-pads
each Chia memo atom to 32 bytes and **truncates anything longer**. To verify the six
signatures locally before spending gas, the bot must reproduce that padding exactly,
offline, from its own memo.

---

## 3. The commit point

**The irreversible moment is the `cat_spend` that moves wUSDC.b to the burn inner
puzzle hash — one transaction *before* the burn bundle is pushed.** **[V]**

`burn_inner_puzzle.clsp` has exactly one code path. No alternative `CREATE_COIN`, no
recovery branch, no timelock. Once a coin sits at that puzzle hash, its only possible
future is destruction into a message paying the curried `RECEIVER`.

This is the single most important design constraint, and it is easy to get wrong by
analogy with the inbound leg:

- **Every safety gate must run before the `cat_spend`** — Base ETH balance for the
  relay, receiver address validation, tip bound, live `signatureThreshold()` read,
  ERC20Bridge USDC liquidity. Gating on the bundle push is too late.
- **There is no outbound equivalent of Sweep.** The inbound leg's ephemeral funding
  coin is always recoverable because the bot alone holds its key. A coin at a
  burn-inner PH is recoverable by nobody.
- **It is forward-only, not loss-prone.** `RECEIVER` is curried in, so even a third
  party who burns the coin for you pays *you*. The failure mode is "stuck pending a
  relay", not "stolen".

---

## 4. Costs and who pays **[V]**

| Cost | Amount | Paid to | Paid by |
|---|---|---|---|
| Chia toll | 0.001 XCH | **Chia farmers** — it becomes the bundle's `RESERVE_FEE` | the bot |
| Base tip | 0.3% of the USDC | the Portal contract | deducted from the transfer |
| Base gas | ~160k gas **[?]** | network | **whoever calls `receiveMessage`** |
| Chia network fee | none beyond the toll | — | — |

Two things to note. The Chia toll is not a warp fee — it is burned as a farmer fee and
doubles as the bundle's mempool fee, so it is not additional. And **there is no
relayer**: `receiveMessage` is non-payable and warp.green's docs say users pay both
legs' network fees. Someone must hold ETH on Base and submit the transaction — that is
us, or a manual step through the portal explorer.

Round-tripping USDC therefore costs ~0.6% in warp tips alone, plus two lots of gas.
That is the number to weigh against automating this at all.

---

## 5. What we can reuse

Substantial. The inbound leg's scaffolding is mostly direction-agnostic.

**Reusable essentially unchanged:**
- `jobs.py` — the durable store, event log, optimistic `expected_status` guard,
  crash-resume discipline. Needs a `direction` column and a decision about the
  single-slot index (§9).
- `service.py`'s `WarpEngine` skeleton — the dispatcher, the four-outcome persistence
  (`advance`/`stay`/`retry`/`terminal`), the error taxonomy, the backoff gate, the
  binding guard, the Qt worker. Only the handler chain is inbound-specific.
- `keystore.py` — the ephemeral BLS key machinery is exactly what the toll-coin
  security coin needs.
- `claim.py`'s `build_security_coin_spend` / `find_security_coin` / `_sign_p2_delegated`.
- `coinset.py`, `clvm_utils.py`, all vendored puzzles (including `wrapped_tail`, whose
  full reveal is carried in the burn solution — load-bearing at runtime, not just for
  the anchor).
- `evm.py`'s RPC plumbing, receipt polling, gas estimation, raw-tx signing.
- `nostr.py`'s relay loop, bech32m tag encoding, and collector structure.

**Needs extension:**
- `nostr.py`'s *verification* — hardcodes `G2_SIG_LEN = 96` and `AugSchemeMPL`. The
  outbound path needs ecrecover against `evm_validator_addresses`.
- `watcher.py` — same API, different `source_chain`; already parameterised.
- `constants.py` — add `burnPuzzleHash`, the EIP-712 domain, Chia toll. Neither
  `burnPuzzleHash` nor `mintPuzzleHash` currently appears anywhere in the repo.
- `wallet.py` — no `get_wallets`, no `cat_get_asset_id`, no `cat_spend`.

**Already there and unused:** `evm_validator_addresses` — ten checksummed addresses in
`constants.py` that nothing reads. That is the outbound signature set, transcribed and
waiting.

---

## 6. What has to be built

### 6.1 Chia wallet RPC — the design's foundation **[V]**

The bot has never spent a CAT it received. It has no master key, no synthetic-key
derivation, no lineage-proof helper. Building those would be a large and risky
addition.

It does not have to. The Chia daemon's **`cat_spend` RPC accepts an arbitrary
`inner_address`** — it runs it through `decode_puzzle_hash` and uses the result
directly as the CAT inner puzzle hash, with no ownership check. So the bot can move
wUSDC.b to the burn inner puzzle hash without ever holding a signing key, and the
daemon handles coin selection, lineage proofs, signing, and change.

Two rules follow, and both are load-bearing:

- **Let the daemon select coins. Never pass `coins=[…]`.** That parameter bypasses
  `select_coins` entirely, and default selection is what honours offer reservations
  (§7). Pinning a coin explicitly would happily double-spend one committed to an open
  offer. This is the entire coin-conflict mitigation.
- The daemon **unconditionally prepends the target puzzle hash as a hint memo**. Fine,
  but do not design around `memos=[]` meaning "no memos".

### 6.2 New code, roughly

| Area | Work |
|---|---|
| `drivers.py` | `get_cat_burn_inner_puzzle` (2nd curry: `RECEIVER`, `BRIDGE_FEE`), `get_burn_inner_puzzle_solution`, `get_cat_burner_puzzle_solution`, `build_burn_bundle`. All exist upstream in warp's `drivers/wrapped_assets.py`; this is a port, not a design. |
| `evm.py` | `receiveMessage` calldata encoding (v‖r‖s packing, ascending-signer sort), `MessageReceived` log parsing, a live `signatureThreshold()` read. |
| `nostr.py` | ECDSA collector: ecrecover each signature, match against `evm_validator_addresses`, sort ascending. |
| `wallet.py` | `get_wallets`, `cat_get_asset_id` (to resolve the wUSDC.b wallet id), `cat_spend`. |
| `service.py` | An outbound handler chain — roughly `AWAITING_UNWRAP → PRE_BURN_CHECKS → BURN_SENT → BURN_CONFIRMED → COLLECTING_EVM_SIGS → RELAYING → UNWRAP_COMPLETED`. |
| `constants.py` | Outbound deployment constants + a `derive_burn_puzzle_hash` anchor. |
| widget + runbook | A destination-address field with strong validation, an unwrap control, docs. |

### 6.3 Anchors, which must all run pre-`cat_spend`

The inbound analogue of `expected_asset_id` is `burnPuzzleHash`. Derive
`sha256tree(get_cat_burner_puzzle("bse", erc20_bridge_address))` offline and assert it
equals the live `ERC20Bridge.burnPuzzleHash()`. **[?]** — the derivation is expected to
match `0x6d64cf90…93a1` but has not been run against this repo's `drivers.py`. There is
a latent divergence to document while doing it: our `get_cat_burner_puzzle` applies
`_strip_source(destination)` and upstream does not — a no-op only because
`0x8412f0…` has no leading zero byte.

Also assert, before spending: exactly three memo content atoms (`ERC20Bridge.receiveMessage`
indexes `[0]`,`[1]`,`[2]` without a length check, so a malformed memo produces a
permanently unrelayable message); amount above the tip floor; ERC20Bridge USDC balance
sufficient; and the six signatures verify locally against the offline-reconstructed
digest.

---

## 7. Interaction with the trading engine

This is the risk that most deserves attention: the C++ engine quotes wUSDC.b and posts
offers collateralised by specific coins. An unwrap that spends a committed coin would
be a real incident.

**The good news, verified:** offer-locked coins are excluded from daemon coin selection
— `get_spendable_coins_for_wallet` subtracts `trade_manager.get_locked_coins()`. So
default `cat_spend` selection already avoids offer collateral. The exposure is
*liveness* (an unwrap may find less spendable than the displayed balance), not
correctness.

**The trap:** that guarantee lives entirely in the daemon and is bypassed by
`coins=[…]`. It is also not backed by anything in our own code — `CoinManager::lock_coin()`
has **zero call sites**; in-process locking is dead code.

**Not yet costed:** the ledger. The C++ event vocabulary is
`opening | fill | fee | take | adjust`, with no transfer-out. An unwrap booked as
`adjust` would silently distort realized P&L. Given the recent accounting overhaul,
this belongs on the critical path, not in a footnote.

---

## 8. Failure and recovery

The inbound guarantee — "an attested message is claimable forever, and the funding coin
is always sweepable" — **does not carry over symmetrically**.

| Stage | Reversible? | Recovery |
|---|---|---|
| before `cat_spend` | yes | abandon; nothing moved |
| after `cat_spend`, before burn | **no** | the coin can only ever be burned to `RECEIVER`; push the burn again |
| after burn, before relay | **no** | message exists; anyone can relay it, signatures never expire |
| relay reverted | yes | nonce write rolled back; retry |

So an outbound `FAILED` state means something different from inbound: there is nothing
to sweep and nothing to cancel. The only escapes are "retry the next step" or "hand off
to the portal explorer". The widget affordances and runbook wording need designing
against that, not copied from the inbound leg.

A dry-run rehearsal is still possible and is worth building: everything up to and
including building the burn bundle can be exercised without pushing, and the relay
transaction can be signed without broadcasting — the same cut point the inbound
`dry_run` uses. The bright line is the `cat_spend`, which is the one step a rehearsal
must never take.

Because the tip floors at 1 unit and wUSDC.b has 3 decimals, the minimum viable
transfer is **0.002 USDC** — so a live end-to-end rehearsal costs fractions of a cent
plus gas. That is the right first test.

---

## 9. Open questions that change the design

Settle these before committing to an estimate:

1. **Does this deployment's daemon expose `cat_spend`, and what is the wUSDC.b
   `wallet_id`?** This gates everything. `wallet.py` has no way to ask.
2. **Does the burn-inner puzzle hash derive correctly from this repo's `drivers.py`?**
   One assertion in `test_warp_clvm_anchors.py` settles it.
3. **Does the portal explorer's "Complete Relay" work for xch→bse?** If yes, the manual
   fallback is solid and automating the relay is optional. If no, we must always hold
   ETH on Base.
4. **Should outbound share the single active-job slot?** They contend for different
   assets but share the Base ETH gas balance. The current partial unique index allows
   exactly one open job table-wide.
5. **What is the right per-unwrap cap?** Ideally relative to `max_drawdown_frac ×
   peak_equity`, but peak equity is computed inside the C++ engine and is not exposed
   to the GUI.
6. **How does an unwrap book in the ledger?** See §7.

---

## 10. Effort and sequencing

The inbound leg is ~11k lines including tests. Outbound is **meaningfully smaller** —
call it 40–60% — because the job store, state-machine scaffolding, keystore, coinset
client, puzzles, and relay plumbing all transfer, and because outbound signatures never
expire (no portal-sync, no re-collection). The genuinely new work is the burn bundle
port, the ECDSA collector, `receiveMessage` encoding, and three wallet RPCs.

Suggested order, each step independently useful:

1. **Settle Q1 and Q2** — an afternoon. If `cat_spend` is unavailable, stop; the design
   changes shape entirely.
2. **Offline anchors** — derive `burnPuzzleHash`, reproduce the validator's memo→bytes32
   padding, verify a real historical xch→bse message's signatures locally. All offline,
   all testable, and it de-risks the expensive parts.
3. **Ledger design** — decide how an unwrap books, in the C++ event vocabulary.
4. **Burn bundle + dry run** — build and sign everything, push nothing.
5. **Live micro-unwrap** — 0.002 USDC, manual relay through the portal.
6. **Automated relay** — only after 5 works end to end.

Steps 1–3 are worth doing regardless of whether the rest is ever built: they are cheap,
they produce durable knowledge, and step 3 is arguably needed anyway to account for the
manual portal unwraps being done today.
