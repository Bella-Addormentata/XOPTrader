# Base wallet key backup — current state and future options

Status: **decision deferred.** Written 2026-08-21 so the options are not
re-derived from scratch later. Nothing here is scheduled work.

The immediate question ("will the next update lose the Base wallet?") is
answered in [What we have today](#what-we-have-today): **no**. Everything
below that is about improving *recoverability*, not about an open risk in the
current release.

---

## What we have today

Shipped in **v0.9.8** (PR #78, `3371e93`).

The Base hot wallet is a single random secp256k1 key created by
`Account.create()` (`gui/services/warp/keystore.py:new_evm_key`). It is not
derived from a seed. At rest it lives as `warp.evm_private_key_dpapi` in
`secrets.yaml`, wrapped with Windows DPAPI (`CryptProtectData`, `CURRENT_USER`
scope) plus a versioned application entropy blob, stored base64.

Backup surface (`gui/widgets/base_wallet.py`):

- **Back up key** reveals the active key as `0x` + 64 hex characters in a
  masked, short-lived modal.
- Clipboard copy self-clears if untouched.
- An explicit "I saved this recovery key securely" sets
  `warp.evm_key_backup_confirmed`.
- The key buffer is zeroed when the dialog closes.
- It can be re-revealed at any time, so a saved copy can be re-verified.

State verified on the live box 2026-08-21: key present,
`evm_key_backup_confirmed = True`, **zero** retired keys. The flag resets to
`False` on both creation and rotation, so with no rotations the confirmed
backup corresponds to the key in use.

### Why an installer update cannot lose it

1. `secrets.yaml` resolves as a sibling of `config.yaml`
   (`gui/services/config_split.py:359`), i.e. `C:\GitHub\XOPTrader\`, outside
   `{app}`. The installer uninstalls the previous version first but removes
   only files recorded in its own uninstall log.
2. `create_wallet()` refuses to overwrite an existing key
   (`gui/services/basewallet.py:126`) — "overwriting a key is destroying
   money". A fresh run cannot silently regenerate over a funded wallet.

### What the current backup does NOT protect against

DPAPI is bound to the Windows **user account on this machine**. Therefore:

- Copying `secrets.yaml` to another PC or another Windows account is useless;
  it will not decrypt.
- Windows reinstall, recreated user profile, disk failure, or hardware
  replacement makes the stored blob unrecoverable.

**A file-level backup of `secrets.yaml` is not a backup of the wallet.** The
revealed hex is the only machine-independent recovery path that exists today.

### Invariant that must not be broken casually

`_APP_ENTROPY` is versioned (`XOPTrader/warp/keystore/v1`). Changing it
invalidates every previously stored blob on every machine. Any change needs a
migration that re-wraps existing keys, not a bare constant edit.

---

## The industry standard: BIP-39

The word-list scheme is **BIP-39**: 12 or 24 words from a fixed 2048-word
list with a checksum, normally combined with BIP-32/BIP-44 hierarchical
derivation (`m/44'/60'/0'/0/0` for Ethereum). Wallet UIs present a word grid
for entry; steel plates with letter grids are the fire-proof storage form.

Why it beats raw hex: the checksum catches transcription errors, words are far
easier to copy by hand and read back than 64 hex characters, and the format is
portable across MetaMask, Ledger and Trezor.

---

## Options

### A. Status quo plus hygiene (no code)

Keep raw-hex backup. Store the hex in a password manager and/or offline paper
or metal. Record the wallet address separately so a restore can be verified
before funds are moved.

- Cost: none. Risk: unchanged. Transcription remains error-prone and the
  backup is not portable to a standard wallet.

### B. Encode the existing 32 bytes as 24 BIP-39 words

Any 256-bit secret can be rendered as a 24-word BIP-39 phrase, so the current
key could be displayed as words without changing the key or the address.

**Foot-gun, and the reason this is not simply "better".** A standard wallet
importing those words treats them as a *seed* and derives
`m/44'/60'/0'/0/0` from them — yielding a **different address** than ours. The
phrase would be a transcription aid for our own importer only. Someone
recovering under stress, years later, would very reasonably type it into
MetaMask, see an empty unrelated account, and conclude the funds were gone.

- Cost: small. Benefit: safer transcription. Risk: **actively misleading**
  unless labelled unmistakably. Not recommended on its own.

### C. Re-key the wallet from a BIP-39 mnemonic (the real interop option)

Generate a new wallet from a BIP-39 mnemonic with standard derivation, then
rotate: fund the new address, migrate, retire the old key through the existing
rotation path (which archives rather than deletes).

- Benefit: genuine recoverability in any standard wallet, independent of this
  machine, this OS and this application. This is what "industry standard"
  actually buys.
- Cost: a new address plus a funds migration, and `evm_key_from_hex` grows a
  mnemonic sibling. Must be done deliberately, never bundled into an update.
- Note: the wallet address is referenced by in-flight warp jobs; rotation must
  happen with no job mid-flight.

### D. Verify-backup flow (small, independent of the above)

A dialog that accepts a typed key or phrase and reports only match/no-match
against the stored key, revealing nothing. Turns "I think I saved it" into a
tested fact, and is the cheapest real risk reduction available.

- Cost: small. Benefit: catches a bad backup while it can still be fixed.
  Compatible with A, B and C.

### E. Split backup (SLIP-39 / Shamir)

Split the secret into shares, k-of-n. Relevant only if the threat model
includes a single backup location being compromised or destroyed.

- Cost: meaningful. Probably disproportionate at current balances.

### F. External signer / hardware wallet

Move signing off the box entirely. The largest change, and it conflicts with
unattended operation — the bridge signs without a human present.

---

## Recommendation

1. Do **not** rotate reactively. The current backup is valid and the update
   path is safe.
2. **D** is the best value for the effort and does not commit to a direction.
3. If BIP-39 is wanted for real, do **C**, scheduled as a rotation with no
   warp job in flight — not **B**, whose address mismatch is a trap precisely
   when it would be relied on.
4. Revisit if the Base balance grows enough to change the threat model.

## Open questions for later

- Should the CAT/XCH side (Chia master key, fingerprint 481655774) be brought
  under the same backup story? It is currently managed by the Chia client, not
  by us, and has a separate 24-word backup already.
- Does a mnemonic path change how `warp` ephemeral BLS keys are handled? They
  are deliberately short-lived and out of scope here.
