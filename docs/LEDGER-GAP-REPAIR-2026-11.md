# Ledger gap repair, 2026-11

Status as of **2026-11-24**: **PREPARED, NOT APPLIED.** The dry run on the live
data closes every asset to within dust (table below). Apply it in the stop
before the first start of the release that books takes on settlement, so that
no engine runs between the repair and the fix.

Script: `scripts/oneoff/ledger_gap_repair_2026_11/repair_ledger_gap.py`.
The dry run and `--check` only read: the engine database through a read-only
URI, and the local wallet's RPC. `--apply` writes, and only with the engine
and the GUI stopped. Policy: `docs/ACCOUNTING-POLICY.md` §2 (reversals and
corrections of auto-adjusts, append-only) and §8 (fees).

## What happened

On 2026-11-24 the ledger stood 17.388 XCH above the wallet. It also stood
833.855 DBX and 10.013 BYC below it, and 5.761 wUSDC.b above it. The wallet
here is every coin except those at the options address. A read-only trace
compared every wallet trade record and every coin record of the four
wallets with `ledger_entries`. It started from the XCH re-tie of 2026-09-03
(`adjust:xch:9241973`), where the gap was exactly 0. Every mojo of the gap
is one of the items below.

| | XCH | DBX | BYC | wUSDC.b |
|---|---|---|---|---|
| A. takes booked that never settled (14) | −44.543716 | +2,933.000 | +15.912 | −1.377 |
| B. corrections of the BYC auto-adjusts after the re-tie (10) | | | −9.575 | |
| C. settled trades never booked (6 takes, 1 fill) | +26.000000 | −2,147.000 | +3.663 | −4.384 |
| D. options address round trips | +1.495742 | | | |
| E. Dexie reward coins never booked (144) | | +47.855 | | |
| F. network fees paid outside fills and takes | −0.339999 | | | |
| **effect on the ledger** | **−17.387974** | **+833.855** | **+10.000** | **−5.761** |
| ledger − wallet before (dry run) | +17.387974 | −833.855 | −10.013 | −0.239 † |
| ledger − wallet after | 0.000000 | 0 | −0.013 | −6.000 † |

† CAT figures in the script count the options address's coins too. Its 6.000
wUSDC.b (2 + 3.991 booked top-ups, and 0.009 of its own) are the whole
difference, so wUSDC.b closes exactly.

Not booked: a 13-mojo BYC coin of unknown origin, and the 0.009 wUSDC.b at
the options address. The script reports both.

## The items

- **A. Reversals.** These 14 takes were booked the moment `take_offer`
  returned: 1bcf6dcc, bcd44195, 6d43bfe5, 083be5b2, da2edc43, e2d47d4e, the
  seven of 2026-11-20 (da716925, f8a59d4b, 8acbdbfe, 7b95898e, 1f3a9fd8,
  a757b5f6, 43cbe65b), and peg-arb #700, 35dcf500. The wallet holds each
  FAILED or CANCELLED, and no coin of any was paid to us. Each gets one
  `reversal` leg per leg, `event_id` `reversal:take:<trade_id>`. Each
  `taker_fills` row is archived as JSON in `offer_closure_events`
  (`phantom_take_correction`) and deleted, so pacing and history no longer
  count it.
- **B. Corrections.** The ten BYC auto-adjusts posted after the re-tie
  (2026-09-04 to 10-15) net to +9.575 BYC. That is exactly the BYC legs of
  the five takes in A that were booked before them (+10.912), less the BYC
  paid by the 2026-09-11 fill in C (−1.337). The rest of each adjust was
  transient and the next one reversed it. Each adjust gets one
  `correction:<event_id>` leg that negates it, so `SUM(adjust)` no longer
  counts it as unexplained. The script refuses if the two sums differ by a
  mojo.
- **C. Bookings.**
  - Five takes of 2026-11-20 (5ed85168, 0b5bba65, 415e3027, 72a48007,
    c6915c78) and one of 2026-11-02 (3ea8fe38). Each settled and paid us in
    its block, but the engine never recorded it. Each is booked as `take`
    legs at the wallet's amounts, at the confirmation height, with fee 0,
    because the wallet's record does not state our fee.
  - The XCH/BYC bid 0xe5c70eac.. of 2026-09-11 was taken while `offer_log`
    calls it cancelled. It is booked as `fill` legs only. `trade_log` and
    `offer_log` are left as they are, because §6 never fabricates an unknown
    cost.
- **D. Options transfers.** 5.2 XCH crossed from the trading addresses to
  the options address before 2026-11-24, and the options ingest booked the
  later 3.0 of it. The first send, 2.199985 XCH at block 9,577,479, came
  before the fence, and the fence's first event only adopted it. It becomes
  an `options_withdrawal`. The 3.695638 XCH that came back in six coins
  become `options_deposit` rows. Two of those coins were clawed-back
  collateral that a cancel spent as a fee coin. `flow_usd` is valued at the
  engine's own XCH mark from the nearest snapshot, so Net Deposits picks the
  flows up.
- **E. Rewards.** These coins were dropped by the 2,000-mojo ceiling or the
  200-row window, or arrived while the engine was stopped for over two days
  (2026-11-16 to 18). They are booked as `reward` rows keyed on their wallet
  transaction, valued at the `px_usd_per_unit` of the nearest reward the
  engine did book. A DBX coin above the engine's reward ceiling (20,000
  mojos) is not a reward by the engine's own rule. It is reported, not
  booked.
- **F. Fees.** The XCH that left trading coins since the re-tie, less the
  XCH legs of the trades that settled and the fees already booked (less the
  fee legs of events already reversed), is 0.339999 XCH. It is the network
  fees of cancels and coin splits (§8 known gap), posted as one `fee` row. It
  keeps growing while the engine cancels, so it is measured when the script
  runs. Each block's outflow is checked first, after the XCH paid by the
  trades that settled in that block (at the block their payment landed in).
  More than 0.001 XCH left over in any one block is a payment, not a fee, and
  the script refuses until it is explained. The dry run also checks what is
  left of the XCH gap after A to F: 0.000000 XCH. `--apply` refuses if that
  is more than 0.01 XCH.

## Runbook

All commands run from `C:\GitHub\XOPTrader`, with the Chia wallet running and synced.

1. Merge and publish the release with the take fix. Do not install it yet.
2. Stop the GUI. That stops the engine and the options book. Stop the
   options runner too if it runs in a terminal. `--apply` refuses while
   `xop_trader.exe` or `xop_trader_gui.exe` runs, or a Python process runs
   `gui.main` or `gui.services.options`. It also refuses if it cannot read
   the process list.
3. Dry run, and read the summary. "XCH left after every item" must be
   within ±0.01. A fill taken while the engine was stopped shows up there;
   if so, wait for it to be booked.
   `.venv\Scripts\python.exe scripts\oneoff\ledger_gap_repair_2026_11\repair_ledger_gap.py --db data\xop_trader.db --config config.yaml`
4. Apply. The output names the backup, `data\backups\xop_trader_pre_ledger_gap_repair_2026_11_<UTC>.db`.
   `... repair_ledger_gap.py --db data\xop_trader.db --config config.yaml --apply`
5. Install the new release and start it.
6. Check that the gap is now within dust: `... --check`

**Rollback:** before the first start, copy the backup over
`data\xop_trader.db`. After a start, reverse the repair's rows instead
(`reversal:<event_id>`, policy §2).

## Residuals

- Takes and fills booked before the re-tie are left as they are. The
  auto-adjusts of July to September absorbed them, and their attribution is
  not reworked.
- The fill of 2026-09-11 has no `trade_log` row, so its realized P&L is not
  in the P&L.
- The six booked takes carry fee 0. Their fees are inside F.
- `inventory_state` is not rewritten. Step 11 re-ties quantities to the
  wallet at boot.
