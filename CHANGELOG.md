# Changelog

All notable changes to XOPTrader are documented in this file.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.10.26] — 2026-09-24 — positions that follow the wallet, a wallet left to sync, and fills the chain has proven

### The risk limits read the wallet's positions, not a guess that fills made

On 2026-09-22 every startup balance read timed out (four 30-second attempts
each, 16:58:39 to 17:04:49) and the seed skipped each asset with a DEBUG line,
so State (the positions the risk limits read) started empty and the log said
nothing. Fills then gave State 1.103 XCH and 102.976 DBX, and no BYC. In that
heartbeat the single-CAT cap read DBX as 52.8% of the portfolio (full block at
50%) and sized the XCH/DBX ask to zero; after the next fill State held 0.103 XCH
and 203.188 DBX, and DBX read as about 96%. The wallet held about 34.7 XCH, 99
BYC and 2,167 DBX. Step 8 had a recovery for an unseeded State, but it fired
only while a position was exactly zero, and the fills had made them non-zero
before Step 8 first read a balance. (That evening Step 8 posted nothing anyway:
it stopped at its wallet sync gate on every heartbeat. But the moment the
wallet synced, the wrong State would have sized every ask, and nothing could
have corrected it.)

- **Every validated balance Step 8 reads now sets State's position**, whatever
  State held before: the main loop's balance gate and the empty-ladder
  liveness refresh, which is the only read a suspended pair's CAT gets (BYC's,
  that evening). Fees, taker fills and deposits, which never pass through
  `record_buy`/`record_sell`, stop accumulating in State as drift too. A reply
  missing `confirmed_wallet_balance` or `pending_change` changes nothing, and
  the bridge asset keeps its single writer while its scan is operational.
- **An unread startup balance is never zero.** A failed read falls back to the
  quantity last persisted for the asset (`inventory_state`), logs an ERROR
  naming it, and stays unverified until Step 8 reads the wallet, which logs a
  WARN with the correction. A built wallet map with no wallet for an asset is a
  verified zero; an unbuilt one proves nothing. The empty State this replaces
  was the least cautious default available: with no positions, concentration
  reads "balanced" and every CAT 0%, so no limit can trip. A startup read, or
  a map with no wallet for the asset, counts as the wallet's word only if the
  startup sync wait saw the wallet fully synced (review round 4). When the
  wait runs out first, every asset takes the unverified path, the log says so,
  and Step 8 verifies each one once the wallet is synced. The wallet-ID map
  boot built is dropped too (review round 5). It is built only once, and one
  built mid-sync can lack a CAT wallet not yet created, which Step 8 would
  then verify as a zero. Step 8 rebuilds it below its sync gate.
- The per-asset startup read failure is logged as a WARN instead of DEBUG.
- **Nothing is quoted from a guessed position** (review round 1). Step 6 sizes
  a heartbeat's ladders before Step 8 reads the wallet, so Step 8 now verifies
  every unverified position right below its sync gate. A heartbeat that
  verifies anything posts nothing, and the next one is sized from the wallet.
  A pair that trades a position the pass could not read is not quoted until
  it can be: that covers a first boot with no persisted row, where the guess
  is nothing at all. A built wallet map with no wallet for the asset still
  counts as a verified zero.
- A pace-managed pair with an empty ladder has both its assets read by the
  empty-ladder refresh, below Step 8's sync gate, like any other pair (review
  round 4). Rounds 1 and 2 took them from pace's own read instead. That read
  runs before the sync check, so it could be taken mid-sync, and it covers
  only the assets pace lists, so it skips XCH in a CAT-only pace config. The
  bridge scan clears the unverified mark on its own asset, but only in a
  heartbeat whose Step 8 passed its wallet-sync gate (review round 6). The
  scan runs every heartbeat, and its own balance fetch checks only that the
  wallet answers. The reconciled cost basis goes through `to_mojo_checked()`,
  and a ratio that is not a representable Mojo falls back to the unit basis
  instead of being converted.
- **An unverified pair's resting offers come down** (review round 2). Its gate
  used to skip every cancel path in the pair loop, so offers restored at boot
  rested unmanaged for as long as the read kept failing. Step 8 now cancels
  them (reason `unverified_position`), each heartbeat, until the pair is flat.
  Round 3 moved that drain ahead of the pair loop and made it scan the whole
  book, like the peg-suspension drain. It runs right after the heartbeat's
  fees are set, which every cancel in Step 8 pays, and ahead of every exit
  that follows. Inside the pair loop it sat behind the skip for an empty
  ladder or an invalid quote, which an unverified pair is likely to have.
  The heartbeat that verifies a position no longer skips it (review round 7).
  That heartbeat used to return before the drain, and on the next one the
  asset no longer counted as unverified, so the offers restored at boot on
  its pairs were never taken down. Now both places that verify, Step 8's
  pass and the bridge scan, record the block they verified at. The drain
  takes down, on the asset's pairs, every offer created before that block,
  never one the pair loop posts afterwards. It forgets the asset once a pass
  has taken them all. The verifying heartbeat ends right after the drain,
  before anything is posted. The drain takes every offer created at or
  before that block (review round 8): after a restart within one peak, a
  restored offer can carry the very height the first heartbeat verifies at.
- A pair whose position a fill moved posts nothing new until the next
  heartbeat (review round 9, from #172's review). Step 8 sets each State
  position to the wallet's balance, and Step 2 books each fill into State
  too. So a take the wallet already showed at the last Step 8 counts twice
  until this Step 8 reads the balance again. That happens when the wallet
  sees the block between Step 2 and Step 8, or when the fill proof waits for
  the node, and Step 6 sized that heartbeat's ladders from the doubled
  position. Its cancels still run; only posting waits a heartbeat.
- **The drift corrector does nothing while any position is unverified** (review
  rounds 2 and 3). Round 2 stood Step 9f down only when no balance had been
  read at all. With some read and some not, the unread asset was simply
  missing from its shares, every other asset looked overweight, and 9f, which
  trades both ways toward its targets, would have sold them.
- **The empty-ladder liveness refresh reads XCH too** (review round 3). Step 7's
  XCH read updates the cap and never State, so for a pair that is not
  pace-managed, nothing corrected XCH's State position while every XCH ladder
  stayed empty. The refresh runs below Step 8's sync gate, so no half-synced
  wallet's reading reaches State. It costs at most one extra XCH balance call
  per heartbeat, and only while some pair's ladder is empty.
- The startup fallback is the quantity `inventory_state` restored, captured
  before the read loop. `seed_position()` fills an empty record from this
  boot's reply, and a reply without `confirmed_wallet_balance` would otherwise
  have passed its spendable balance off as the persisted quantity (review
  round 2).

Not in this change: the InventoryTracker (the strategy's `q`) and its one-shot
Step 11 reconcile, and the XCH/DBX bid, which was zero for a different reason
that evening (`q` above `q_max`).

### The engine stops restarting a wallet that is still syncing

On 2026-09-22 the wallet reported `synced=false, syncing=true` on every Step 8
heartbeat from 17:08 on, so Step 8 managed no offers all evening. Every 20
unsynced heartbeats (6-10 minutes in practice; the code said "~3 min") the
engine ran `chia stop wallet & chia start wallet`, 9 times between 17:18 and
18:24, and that restart is what kept the wallet from syncing. In Chia 2.7.4 a
long sync records its progress only when it completes; with
`use_delta_sync: false` it re-reads every puzzle hash and coin from height 0;
and a freshly started wallet begins its first long sync by rolling back 256
blocks. So every restart threw away the sync in progress and moved the wallet
backwards. At 18:29 its finished-sync height was 9,297,547 against a node peak
of 9,329,985.

- The restart now follows `execution/wallet_sync_watch.hpp`. A wallet that
  reports a sync in progress is not restarted until it has been unsynced for
  2 hours in total. Idle time before the sync counts too, at most 15 minutes,
  so that a wallet flipping between syncing and idle still reaches a restart
  (review round 4). One that is neither synced nor syncing is restarted after
  15 minutes. Each successful restart doubles both budgets for the next
  attempt (capped at 24 hours), and reporting synced resets them.
- Time is measured on a monotonic clock instead of counted in heartbeats. A gap
  of more than 10 minutes between readings (Step 8 not reached) starts a new
  streak rather than counting as unsynced time.
- A reply without `syncing` is treated as syncing: never as idle, and never as
  synced either (review round 2). Step 8 used to read `{"synced": true}` alone
  as synced and manage offers, while the startup gate kept waiting on the same
  reply.
- The Step 8 line says how long the wallet has been unsynced and what a restart
  waits for; the restart line says which restart it is. It prints the syncing
  state the verdict used, so a reply without `syncing` reads
  `syncing=missing, read as true`, not `syncing=false` (review round 3).
- The re-synced line reports the whole outage, restarts included (review
  round 5). It printed the unsynced streak, which each restart starts afresh.
  So after a restart it gave only the time since that restart, and "0s"
  when the first reading after a restart or a pause was already synced. When
  Step 8 was not reached for part of the outage, its length is unknown, and
  the line says so instead of giving a number.
- A restart whose start failed no longer leaves the wallet down (review
  round 6). The restart ran as one command, `chia stop wallet & chia start
  wallet` on Windows, which returned only the start's code. A start that
  failed after a stop that worked left no wallet to answer Step 8's sync
  check. The watch then never decided again, and the wallet circuit breaker
  skipped Step 8 altogether, so the retry never came. Stop and start are now
  two commands. A start that fails is owed, and the poll loop retries it on
  every poll, before any wallet or height call (review round 7): after 60
  seconds, then at doubling intervals up to 15 minutes, until a start
  succeeds or the wallet answers. It runs there rather than in the
  heartbeat because in wallet-only mode a heartbeat needs a height from the
  very wallet the failed start left down. Once the owed start works, or the
  wallet answers, a restart whose stop worked counts again, so the next
  budgets double as a successful restart's do.

Not in this change: the wallet's own configuration (`use_delta_sync`,
`connect_to_unknown_peers`), and the restart itself, which is still a blocking
`std::system` call.

### A fill is booked only when the chain shows the offer was taken

On 2026-09-22 the engine booked three fills for offers that were never taken:
the XCH/DBX asks `0xd6a8325c15` and `0x83eef9df80` and the XCH/BYC bid
`0xdb63709cb9` (trade_log rows 1900-1902). Each had lost exactly one XCH input
to another of the bot's own transactions, which spent it paying a
15,000,000-mojo fee (blocks 9,324,680, 9,325,004 and 9,325,694). Every other
maker coin of all three is still unspent, and Dexie shows all three cancelled.
The wallet nevertheless reported them CONFIRMED at exactly those heights, and
detect_fills booked every CONFIRMED offer: they entered trade_log, the ledger,
the inventory tracker and State (about -0.9 XCH, -1.864 BYC and +203.188 DBX,
and 44,876 DBX mojos of realized P&L that never happened).

The wallet's CONFIRMED is its own bookkeeping, not evidence. The likely
mechanism, read from Chia 2.7.4's code: it marks a trade CONFIRMED when every
coin its inputs would create exists on-chain, but it only checks the inputs it
finds in its coin store at that moment. After the 2026-09-22 resyncs the
untouched inputs could be missing from the store, and a check over what is left
can pass for an offer nobody took.

- Before booking a CONFIRMED offer, detect_fills looks up the offer's maker
  coins (the trade record's `coins_of_interest`) on-chain
  (`execution/fill_proof.hpp`). It asks the full node while the engine trusts
  it (a node, and not `wallet_only_mode_`, the rule the S14 cancel escalation
  uses), and otherwise the wallet, which does not answer until it is synced.
- A take spends every maker coin in one block, but so does a cancel, or a
  stray spend of an offer funded by one coin, so that alone books nothing. A
  fill is booked only when that block also shows the take's own mark:
  - From the node: a settlement coin, created from a maker coin at the offered
    asset's settlement puzzle, for exactly the amount offered, and spent in
    the same block. The puzzle is OFFER_MOD for XCH, and for a CAT it is CAT
    v2 curried with the CAT's TAIL around OFFER_MOD
    (`CoinManager::settlement_puzzle_hash`). An amount alone could belong to
    any child.
  - From the wallet, which cannot see settlement coins: a coin of ours
    confirmed in that block for exactly a requested amount, whose parent is
    not one of this offer's maker coins. That is all the check can prove: it
    cannot tell a payment from an unrelated coin of ours (see "Not detected"
    below).
    The wallet is asked for every such coin, with no row limit. A limit of
    50 would have hidden a payment past the 50th row on every retry.

  Four real takes (one ask, three bids) each show exactly one such
  settlement coin, at exactly the puzzle computed for its asset. The
  phantom's consumed coin and four confirmed cancels show none. Once the
  mark is found the fill books as before. The fill's height is
  now the height of those spends, which the confirmation-depth buffer counts
  from, not the wallet's `confirmed_at_index`. For a take the wallet saw
  itself they are the same number.
- A maker coin still unspent while another is spent, maker coins spent at
  different heights, or every coin spent in one block that the node shows
  without a settlement coin, means the offer died without being taken. That
  includes a block where the maker coins created no children at all. A reply
  without its list of coin records, at either stage and from the node or the
  wallet, is a failed lookup, not an empty list. So the first malformed reply
  ends the lookups for that heartbeat, as a timeout does, instead of every
  other CONFIRMED offer asking again (review round 7). The S14 cancel
  escalation, which shares the node's lookup, now stops its sweep on one too.
  Nothing is booked. Once the first spend is
  `strategy.confirmation_depth_blocks` deep (default 6), the offer stops being
  tracked, the engine logs an ERROR, and it records the offer `cancelled` at
  the height of that spend with the reason `dead_on_chain`. The closure event
  keeps that reason. The offer_log row follows the rule every closure does
  (S14): a row still open closes `cancelled` at that height with that reason,
  a row whose cancel was already submitted closes the same way but keeps that
  cancel's cause, and a row already closed keeps its status. So an audit of
  dead offers reads the closure events (review round 9). A write that
  fails is retried every heartbeat, up to the 10 failures S25 allows. A fee
  ticket for a cancel on it closes without an observation, the same way a
  FAILED offer's does: the chain cannot say whose spend killed it.
- Every maker coin unspent means the offer can still be taken. Nothing is
  booked and it stays tracked. If it is taken later, it is booked then.
- A lookup that fails, or an answer that does not cover every maker coin or
  cannot be read, books nothing. So does a wallet that shows no payment: its
  store is what was incomplete on 2026-09-22, so its silence proves nothing
  either way. The offer stays tracked and is asked about again next heartbeat.
  The first deferral and every 20th are logged. After one lookup fails,
  nothing more is asked that heartbeat, as the S14 escalation does: each
  failure spends its transport retries. A lookup the node or wallet refuses
  is different (review round 15). The wallet refuses a request that names a
  coin it does not hold. The refusal costs one round trip and is that
  offer's alone, so the offers after it are still asked. The fill poll visits
  offers in hash-map order, so the same offer could otherwise have come
  first every heartbeat and kept every later fill unproven. Only that
  refusal is one offer's (review round 16). A wallet that is not synced, or
  not connected to a synced peer or any full node, refuses every lookup the
  same way, so that refusal still ends the heartbeat's lookups.
- While the wallet reports an offer CONFIRMED and the proof has not settled
  it, the engine will not cancel it. Chia's secure cancel sets PENDING_CANCEL
  over any status, and an insecure one sets CANCELLED. So a cancel sent during
  a one-heartbeat lookup failure would erase the CONFIRMED the proof is waiting
  on, and a real take with it. The one exception: an offer the full node shows
  live again can be cancelled. That means every maker coin unspent in the
  latest fill check, with the node at least `confirmation_depth_blocks` past
  the height the wallet claims. A take undone by a reorganisation therefore
  does not leave a quote nobody can withdraw. The latest check is counted by
  call, not by block: two checks can run at one height, and a live proof from
  the earlier one no longer counts. An offer is held from the poll that reads
  CONFIRMED, before the engine waits on anything else. So a Cancel All or a
  shutdown that runs while the proof is being asked cannot slip in before the
  hold. An offer stops being held by the first poll that reads any other
  status, again before the engine waits on anything else. Once the engine
  has read that status, no cancel of the offer is refused on its account.
  The status must be one of Chia's six trade statuses (review round 8). An
  unrecognised one is no evidence the offer left CONFIRMED, so the hold
  stays until a poll reads one that is.
  Nor does PENDING_CANCEL or CANCELLED release it (review round 11): they are
  what a cancel writes, and a cancel can write them over a real take. In
  chia 2.7.4 cancelling an offer marks every trade not yet CANCELLED that
  shares one of its coins, a CONFIRMED one included. And a take leaves a
  pending offer that shared one of its coins PENDING_ACCEPT, so cancelling
  that offer later -- any sweep or per-offer cancel, the watchdog's included --
  reaches the taken trade. Its take was then never proved or booked. Now the
  next heartbeat proves such an offer on-chain once more. If the chain shows
  the take, it is booked, from that proof. If it shows the offer live or dead,
  the hold is released and the offer is handled as its status says, as
  before. If the chain cannot say, it stays held and is asked again.
  Nor only an offer this process held (review round 12). The overwrite can
  come before the first poll that reads CONFIRMED, and a restart forgets
  every hold. So every tracked offer is proven the first time it shows each
  cancel status, and a held one every heartbeat. The answer is remembered,
  so a cancel costs about one lookup per status. For an offer never held, an
  answer the chain cannot settle is asked again only after a failed lookup.
  Otherwise its status stands, so no offer waits on a question no retry
  will answer.
  Nor at boot (review round 13). Startup reconciliation counted a DB row the
  wallet reported CANCELLED as terminal, and the engine stamped it cancelled
  before restoring the book. So a take that a cancel overwrote before a
  restart never reached the proof. Such a row is no longer stamped: it
  restores into State flagged cancel_pending, and detect_fills proves it the
  first time it polls it. FAILED, which no cancel writes, is still stamped.
  Two refinements (review round 14). An offer the wallet reports
  PENDING_CANCEL is marked cancel_pending in State before any proof, as
  recheck_terminal's revival does. The cancel in flight may be anyone's,
  and once a proof released the hold, the TTL and reprice paths would
  otherwise have sent a second secure cancel for the same coins. And only a
  Dead answer is remembered: a Live offer can still be taken, so it is asked
  again next heartbeat, one lookup, until its cancel lands.
  And an offer the wallet reports CANCELLED while every maker coin is unspent
  stays tracked (review round 15). That is a local cancel: the last resort of
  the emergency cancel ladder, or one made in the wallet's own UI. The offer
  can still be taken, and the wallet watches no CANCELLED trade's coins
  (chia 2.7.4), so it would never report the take. detect_fills used to
  close such an offer after one Live proof. It now stays tracked, flagged
  cancel_pending, and is proven every heartbeat until the chain shows it
  taken (booked then) or dead (closed then). The one exception is an offer
  the expiry retire (`ttl_cancel_mode: expire`) cancelled locally after
  proving it expired: nothing can take it, so it still closes on that
  verdict. The engine remembers those only until it restarts, so after a
  restart such an offer stays tracked until one of its coins is spent.
  Periodic reconciliation now leaves a CANCELLED offer to detect_fills the
  same way, instead of removing it unproven.
  A Dead answer under a cancel's status is final only once the spend that
  killed the offer is `strategy.confirmation_depth_blocks` deep, as it is
  for a CONFIRMED offer (review round 16). A shallower spend can be
  reorganised out, and in chia 2.7.4 a reorganisation leaves the wallet's
  trade records alone, so the cancel's status would stay over an offer that
  can be taken again. Until the spend is deep enough, the offer stays
  tracked and is proven every heartbeat.
  After an accepted Cancel All sweep, an offer already flagged
  cancel_pending is read again, as a held one is (review round 16). The
  sweep skips a trade the wallet calls CANCELLED, and such an offer was
  reported as one this call had cancelled.
  An answer under a cancel's status that settles nothing is asked again
  (#172's review). One that does not cover every maker coin, a record that
  cannot be read, or the wallet's silence can still be completed by a node
  catching up, and the status used to stand at once, so a take hidden under
  it was never asked about again. Such an offer now stays tracked, flagged
  cancel_pending, and is proven every heartbeat for
  `strategy.confirmation_depth_blocks` from the first such answer. Only
  then does its status stand. A record that gives the proof nothing to ask,
  such as no readable maker coin, still lets it stand at once.
  After an accepted Cancel All sweep, an offer the wallet reports CANCELLED
  is proven on-chain before it counts as closed (review round 18). A local
  cancel leaves every maker coin unspent and the offer takeable, and the
  sweep skips it, so a shutdown could end "all cancelled" with it still on
  offer. One the chain shows dead or taken is closed. One it shows live, or
  cannot prove, is reported outstanding, and stays so on every retry: the
  per-offer path no longer reports such an offer as a cancel already in
  flight. Nothing is sent for it; it takes a secure cancel.
  Cancel All's wallet-wide sweep skips every trade the wallet calls
  completed, so it never reports a held offer as cancelled. Such an offer goes
  through the guarded per-offer path instead: it is cancelled there if the
  node has proven it live again, and otherwise it stays outstanding. It is
  never marked cancel_pending over a quote that may still be takeable.
  A hold, though, is the status of the last poll, and the sweep acts on the
  status each trade has when it runs. In chia 2.7.4 it marks PENDING_CANCEL
  every trade it cancels, and every trade not yet CANCELLED that shares a
  cancellation coin with one, a held CONFIRMED trade included. So after an
  accepted sweep, each held offer's status is read again (review round 10).
  Only one the wallet still reports CONFIRMED goes to the guarded path, and
  nothing is sent a second time. One PENDING_ACCEPT or PENDING_CONFIRM is
  live and was not swept, so it is cancelled. One whose status cannot be
  read is sent nothing and reported outstanding. The rest are not reported
  as cancels this call submitted, which the callers would persist with their
  own cause (review round 11). One PENDING_CANCEL is reported as a cancel
  already in flight. One CANCELLED or FAILED is reported closed, for
  detect_fills to read. PENDING_CANCEL and CANCELLED keep the hold, for the
  proof above; FAILED, PENDING_ACCEPT and PENDING_CONFIRM release it.
- A coin record whose height does not fit a BlockHeight is unreadable, so it
  proves nothing. The engine narrows every proven height to 32 bits, and such
  a height would have wrapped to an old block.
- `recheck_terminal` no longer re-adopts an offer proven dead. The wallet goes
  on reporting it CONFIRMED, and re-adopting it would send it through
  detect_fills again.

The gtest replays the wallet and node records of `0xdb63709cb9` (one of three
maker coins spent, at 9,325,694: dead), and of two real takes: an ask on
2026-09-16, `0x202ff7d2d8`, and a bid, `0x18672b6b0f`. Each has its settlement
coin and its payment.

A genuine fill can now wait a heartbeat or more if the node has not yet seen
the take. Each CONFIRMED offer costs one or two extra coin-record calls per
heartbeat until it is resolved.

Not detected: another of our offers, built on the same coins and offering
exactly the same amount, taken while this one is reported CONFIRMED. Its
settlement coin looks the same, and only its requested payment differs. The
node cannot search for a payment. From the wallet, which is asked only while
the node is not trusted: an unrelated coin of ours, confirmed in the same
block for exactly a requested amount. The wallet shows a payment's parent
only as a coin id, and it holds no record of a coin that is not ours, so it
cannot tell a settlement coin from any other sender.

Not changed: a fill still books once, when the take is found, and then waits
out the confirmation depth without being checked again. A take reorganised out
of the chain inside that window is still booked. That gap predates this change
and is listed in `docs/PNL-FIX-DEPLOYMENT.md`.

Not in this change: repairing what the three rows already booked (trade_log
1900-1902, their ledger legs, their offer_log rows and the tracker). That needs
the engine stopped and a separate decision. Also not in this change: the
trigger, which is a new offer built on an XCH coin that a pending transaction
spends as its fee.

## [0.10.25] — 2026-09-21 — less dust, fewer cancels, a fee controller shipped off, and stops that keep the book

### Less reward dust in new offers, except on the no-floor retry

Dexie pays liquidity rewards as one tiny coin per rewarded offer. On 2026-09-19
the DBX wallet held 4,575 unspent coins, 4,389 of them under 0.1 DBX and worth
84.6 DBX together. The wallet's coin selection minimises overshoot, which
favours dust, so an XCH/DBX bid paying 80.334 DBX became a 62,228-character
offer that Dexie refused with HTTP 400 "Too many input coins". The offer still
existed in the wallet and locked its coins, listed nowhere. `engine.log` holds
13 such refusals between 2026-09-10 and 2026-09-19.

What the floor below does, stated plainly: the **first** create for each offer
asks the wallet for coins of at least 1% of the amount, which bounds its CAT
leg at 100 inputs — about 46,000 characters at the 459 per input measured
here, against refusals that began at 60,612. (That bounds the CAT leg only:
the XCH fee coin is a separate selection this CAT-scaled value does not
constrain — one coin while every XCH coin covers the fee, as today's do by
133x the **cap** on that fee, which is a measurement and not a guarantee.)
Those 13 HTTP 400s
become a create the wallet either satisfies or refuses up front, before any
offer exists. What the floor does **not** do is guarantee the outcome: a
refused create is re-sent once without the floor, and the offer that retry
builds is as exposed to dust as it was before this change.

- **A floor on the coins an offer is funded from.** The **first**
  `create_offer_for_ids` attempt for a CAT-funded offer now carries
  `min_coin_amount` =
  ceil(mojos the offer spends x `strategy.offer_min_input_coin_frac`) — **the
  first attempt only: the no-floor fallback below sends a second, distinct
  create with the key omitted**, and an offer built by that retry is as
  exposed to reward dust as it was before this change. The new
  key defaults to 0.01, accepts [0, 1), is read at startup, and 0 disables it,
  restoring the previous request byte for byte — **the request only**, see the
  operator note below. With every CAT input at least 1% of
  the amount, 100 of them always suffice — the **CAT leg** only; the XCH fee
  coin is selected separately and this CAT-scaled floor does not bound that
  count. Dexie does not
  publish its limit; from this bot's own submissions it accepted 125 inputs
  (57,382 characters) and refused everything from 60,612 characters up.
- **XCH-funded offers are unchanged.** Their coins are shaped by the coin pool
  and budgeted by the XCH lock ledger under the wallet's default selection.
- **One value covers the fee coin too, and the XCH lock ledger models it.** In
  chia 2.7.4 the coin-selection keys are read from the top level of the request
  and one config governs every selection it makes, including the XCH fee coin
  of a CAT-funded offer. The floor is CAT-scaled (804 mojos for the offer
  above), and it changes which XCH coin pays the fee **exactly when the XCH
  wallet holds a coin below the floor** — chia filters the candidate set
  before it chooses a selection branch, so excluding one coin can flip the
  branch whatever that coin's size relative to the fee. The fee decides only
  whether the exclusion changes the answer. (An earlier revision of this entry
  said the effect "needs floor > fee" and derived thresholds of 500 and
  1,500,000 CAT units from `fees.min_fee_mojos`. That was wrong in both
  directions and is withdrawn: a floor under the fee can still change the
  selection, and a floor no coin falls under changes nothing however large.)
  When the wallet does skip a small XCH coin it locks a larger one, so
  `CoinLockLedger::try_lock`
  and `try_lock_floor_only` take the same floor, in the preflight probe and in
  all three posting paths. Without it the ledger charged the coin the wallet
  skipped and later creates could pass the cycle cap or the reserve floor on
  XCH that was already locked.
- **On the live deployment this is inert today — because of the coin set, not
  the fee.** Measured read-only on 2026-09-21 (`chia rpc wallet
  get_spendable_coins`, `get_coin_records`, `get_wallet_balance`), twice in the
  day. The XCH wallet held 54 unspent coins both times, the smallest
  **13,494,209,440 mojos** on the first read and **13,314,209,440** on the
  second; spendable went 30 → 42 and its smallest 20,757,615,448 →
  13,314,209,440. The figures below use the smallest of those readings. The
  largest floor the bot can emit is bounded by the CAT mojos **one offer** can
  spend, so it scales with the fraction, and **both** CAT-funded pairs enabled
  in the live config are in scope — XCH/DBX (wallet 8, 1,844,501 mojos) and
  XCH/BYC (wallet 4, 88,845 mojos). DBX binds at every fraction: 18,446 mojos
  at the shipped 0.01, 1,844,501 at a fraction just under 1.
  **The margin is therefore not one number.** Against 13,314,209,440 it is
  721,794× (5.86 orders of magnitude) at 0.01 and 7,218× (3.86 orders) at the
  top of the range — "more than five orders" holds only up to a fraction of
  about 0.072, and it was previously stated as an absolute over every floor the
  bot can emit, which it is not. **The conclusion survives the correction:**
  even the largest floor the range `[0, 1)` permits, from either CAT wallet, is
  over three orders of magnitude under the smallest XCH coin, so nothing is
  filtered out and no selection changes. This is a property of **today's coin
  set, which moves** — it moved twice within 2026-09-21, and the same coin was
  recorded at 20,787,615,448 mojos in the 2026-09-20 snapshot and at
  20,757,615,448 on 2026-09-21, exactly 30,000,000 lower, which is what two
  15,000,000-mojo fee spends would do (that attribution is an inference; the
  measurements are not). `fees.min_fee_mojos` does not govern reachability
  in either direction.
- **The fraction is applied in parts per billion, rounded up.** Rounded to
  nearest, the applied fraction could fall below the configured one and the
  input bound failed for ordinary values: at 1/3, three floor-sized coins of a
  1,000,000,000-mojo offer totalled 999,999,999. The default 0.01 is exact and
  unchanged.
- **Fallback.** If the wallet answers that the coins at or above the floor
  cannot cover the amount ("... or our minimum coin amount is too high"), the
  create is sent once more without the floor and a `[min-input-coin]` warning
  names the pair, side, tier and floor. Only that answer triggers it: a timeout
  or any other transport failure is never followed by a second create from this
  path. The offer built by the retry can again be one Dexie refuses.
- **Detection.** A Dexie refusal for too many input coins now logs one
  `[dexie-too-many-inputs]` warning that names the offer, its length, the
  remedy and a count since start. The remedy is to **combine the coins** —
  the CAT's, since the dust is CAT reward payouts and the CAT leg is what the
  floor bounds; if the XCH fee leg ever contributes, the coins to combine
  there are XCH.
  **The rest of the advice is computed, not fixed.** The warning is handed the
  posting and the offer's length and nothing about how the offer was built, so
  it derives `ceil(1 / frac)` — the CAT-leg bound on a *floored* create — and
  branches on whether that plus the one-coin fee leg fits inside the 125
  inputs Dexie was measured to accept:
  - **At or above 1/124** (the shipped 0.01 bounds it at 100): a floored
    create *cannot* reach the limit, so a refusal is proof no floor was sent.
    Do **not** raise the fraction — that makes the wallet refuse the floored
    create more often and fires the no-floor retry that builds these offers.
    Only a small **reduction** can help, never below 1/124.
  - **Below 1/124** (0.002 bounds it at 500): the bound is over the limit on
    its own, so a create the wallet **satisfied** can be refused exactly like
    a dust-funded one and the refusal proves nothing. The advice inverts —
    **raise** the fraction — and the warning names the preceding
    `[min-input-coin]` line as the way to tell the two apart for that offer.
  - **At 0**: the floor is off and nothing carries a bound.

  The earlier text gave the first branch's advice unconditionally, which was
  correct at 0.01 and backwards below 1/124; and it printed `0.008` as the
  safe floor, which is on the wrong side of 1/124 — `ceil(1 / 0.008)` is
  exactly 125, one over once the fee coin is counted. Both numbers are now
  derived from `kDexieMeasuredInputLimit` and `kDexieFeeLegInputsToday`, so
  they cannot drift apart again. The fallback fact is deliberately **not**
  threaded into the warning: in the shipped branch it is deducible, and the
  datum would have to be the optional floor rather than a "did the fallback
  run" bool, since XCH-funded offers and skipped dict shapes also send no
  floor. No metric was added: `OfferManager` has no posting-failure
  metric to extend. Nothing is cancelled automatically.
- **Startup warns when the fraction cannot bound what it is for.** The range
  `[0, 1)` is closed at the bottom, so a value like 0.002 loaded silently
  although `ceil(1 / frac)` is then 500 — above Dexie's limit before the fee
  coin. Config load now emits one `[Config]` warning for any fraction that is
  on but below 1/124, naming the bound it gives and the value to use. `0` is a
  real setting and is not warned about. Not an error: the key is not
  load-bearing for safety.

**What changes on upgrade, with no config edit at all.** Three things, and only
the first has a lever:

1. **The floor is ON.** `strategy.offer_min_input_coin_frac` is absent from the
   live `config.yaml`, so it takes its 0.01 default and every CAT-funded
   `create_offer_for_ids` starts carrying `min_coin_amount` **on its first
   attempt** from the first restart. It is not carried by the no-floor
   fallback, which re-sends the create with the key omitted after a min-coin
   refusal (Fallback bullet above): that request is byte-identical to the
   pre-upgrade one, so the offer it builds has the pre-upgrade exposure to
   reward dust — including the exposure that produced the 62,228-character
   offer Dexie refused on 2026-09-19. Setting the key to `0` turns the floor
   off altogether.
2. **A create that fails is no longer re-sent** (the section below). This is
   unconditional: `retry_policy_for_endpoint` keys off the endpoint name, not
   off the fraction, so `offer_min_input_coin_frac: 0` does **not** restore the
   old four-attempt behaviour. A create that times out now fails after one
   attempt (30 s) instead of four (~124 s).
3. **An unanswered MERGED create posts nothing else for that side this cycle**,
   also unconditional and also unaffected by the fraction. If the other side of
   the pair did post, the asymmetric-ladder guard in `post_quotes` can then
   cancel it, so a merged-create timeout can cost one cycle of both sides
   rather than one side.

2 and 3 are strictly safer than what they replace — the risk they remove is a
duplicate offer, which costs real money — but they are behaviour changes that
ship whatever the new key is set to, and no config value reverts them.

**Operator note.** The dust already in the wallet is not touched: it stays
spendable, the engine simply stops selecting it for offers, and it can be
combined at any time (`chia wallet coins combine`). An offer Dexie refused is
tracked like any other and retired by the usual cancels: the one posted at
14:42 on 2026-09-19 was cancelled at 18:23. Until then it locks its coins while
listed nowhere (`TODO.md` S66).

**A create is no longer re-sent once it may have reached the wallet.** `rpc_post`
re-sent `create_offer_for_ids` after a timeout or an HTTP 5xx, up to three more
times, and returns only its last attempt (#158 had left the endpoint out of
scope). A second copy of a create is a second offer, and the fallback above
made that worse than it was: the wallet's refusal could answer a copy whose
original had already built the offer and lost its reply, and the fallback then
created another. `create_offer_for_ids` is now never re-sent unless the request
cannot have reached the wallet (no connection, no TLS handshake), as the two
cancel endpoints already were, so a create that times out fails after one
attempt (30 s) instead of four (about 124 s). `take_offer` is unchanged. For the
same reason a merged create that ends with no answer is no longer followed by
one create per tier; a refusal, or a failure before the request was written,
still is.
### A closed-loop fee controller, shipped off

With Chia blocks about 97% full the node admits a spend only at 5 mojos or more
per unit of CLVM cost. The engine paid the node's estimate for a plain XCH send
and never checked whether its own cancels and takes confirmed, so they sat,
`pending_change` persisted, and Step 8 force-deleted every unconfirmed wallet
transaction 10-12 times a day. This adds the feedback. Both new switches
default to off, and with both off every fee is what v0.10.24 paid.

### Fee controller (S67)

- **One controlled quantity: a fee rate, in mojos per CLVM cost.** Four action
  classes turn it into a fee — cancel of an XCH-offered offer (8.4M cost), cancel
  of a CAT-offered offer (42.3M), take (125M), fee attached to a posted offer
  (21M). The costs were measured on this wallet's own spend bundles and are
  configurable. Every `get_recommended_fee` call site now names its class; the
  parameter has no default, so a site that forgets does not compile.
- **Fast up on evidence that a fee is too low.** A cancel or take of ours still
  pending after `controller_target_delay_blocks` (8 peak heights, 150 s) is a
  censored observation and raises the rate at once — a too-low fee may never
  confirm, so waiting for a confirmation would deadlock the loop. Also heard:
  a late confirmation, the wallet's `sent_to` fee refusals (read from rows the
  stuck-transaction pruner already holds), Step 8's `pending_change` counter at
  half way, and the force-delete itself. The law is a velocity-form PID in log
  fee space that only ever raises.
- **Slow down by probing.** After 8 on-target confirmations the fee steps down
  15%. A confirmation counts only if that spend paid no more than the loop pays
  for its class now — compared as fees, after the `[min_fee, max_fee]` clamp, so
  a cancel the `min_fee_mojos` floor lifted still counts for the levels at which
  the loop would pay that same floor. A probe that fails returns to the last
  known-good fee plus 10%, the level
  it failed at is remembered, and re-testing that level waits twice as long each
  time (cap 256 confirmations). A re-test that succeeds means the floor has
  fallen: the memory is dropped and probing resumes at the base interval.
- **Anti-windup.** A stuck spend stops counting once the fee is `min_raise` above
  what that spend paid, which also bounds what a fee clamped by `max_fee_mojos`
  or the budget can do. Wallet-level signals are ignored for two target delays
  after a raise, and at most three in a row may raise the fee without an
  observation of one of our own spends in between.
- **The node's numbers are a floor under the learned rate, never a multiplier.**
  `get_fee_estimate` is now asked with an explicit `cost` instead of
  `spend_type: send_xch_transaction`; the rate it returns is scaled by each
  class's cost. The node's own admission floor is read from the
  `get_blockchain_state` reply the height poll already fetches
  (`mempool_cost`, `mempool_max_total_cost`, `mempool_min_fees`). No node call
  is added, the wallet-only gate is unchanged, and a reading older than 32 peak
  heights is dropped: with the node unreachable the learned rate stands alone.
- **The budget no longer stops the bot, and it never prices a cancel below what
  the node will admit.** With the controller on, offer-attached fees may spend
  only what is above a reserve (`controller_budget_reserve_cancels`, 25 CAT
  cancels, capped at half the budget so it can never exceed the budget itself);
  an exhausted budget pins the attached fee at `min_fee_mojos` and sends one
  `FeeBudgetBound` alert — which fires when the budget **could not fund** that
  fee, not only when it *lowered* one: at the pin the emitted fee is exactly
  what was asked for. It never returns 0, which made Step 8 skip cancelling
  stale quotes as well as posting. Step 8 asks for one attached fee and attaches
  it to every tier it posts, so the room above the reserve is shared across the
  tiers it may post that heartbeat rather than granted to each. **The reserve
  moves *when* that squeeze starts; it does not bound what attached fees
  spend** — `min_fee_mojos` overrides it unconditionally, `should_post_offer`
  no longer refuses a tier on budget grounds with the controller on, and a fee
  is booked for every offer *posted* (S69), so a ladder can still push the
  window past the budget at `min_fee_mojos` per tier. Only a correctly sized
  `daily_budget_mojos` prevents that, which is what the startup advisory is
  for. **A cancel or a
  take is never degraded** — with one exception found at review round 8 and
  recorded below, the bulk stop/shutdown sweep: `min_fee_mojos` on a 42.3M-cost CAT cancel is 0.35
  mojos per cost against the 5 a full mempool admits, so a degraded cancel is a
  spend that cannot be mined, keeps its coins locked and ends in a wallet-wide
  force-delete. It is paid in full — `max_fee_mojos` is the ceiling that bounds
  it — and the overrun is reported with one `FeeBudgetUnfunded` alert. That
  report is raised when the wallet ACCEPTS the spend, never when a fee is
  merely quoted: Step 8 prices both cancel classes every heartbeat before it
  cancels anything, so reporting at the quote would say "paid over budget" on
  heartbeats that sent no wallet RPC at all.
- **The budget is sized by derivation, not by a quoted number.** At full-mempool
  prices and this wallet's measured action rates one `fee_window_blocks` window
  costs 15,163,585,937 mojos, so `daily_budget_mojos` wants at least
  30,327,171,874. The engine computes that from the operator's own costs and
  window, logs it at startup and warns when the budget is below it;
  `config.example.yaml` quotes the same figure and shows the arithmetic.
- **Tickets carry what was really paid.** A cancel's ticket opens when the
  wallet accepts the cancel RPC, with that call's fee (an emergency tier, a
  zero-fee retry and an escalation included) and its height; a re-cancel
  replaces it. Only a wallet-verified CANCELLED closes it as a confirmation:
  `recheck_terminal` answers "still terminal" for FAILED too, and a FAILED
  offer says nothing about our fee. A cancel adopted at boot has no ticket.
  A ticket's height is the height of the cycle that issued the spend (the
  startup height for a cancel the startup reconcile issues), never the
  last-processed-block marker, which trails by a cycle and is 0 at boot; with
  no known height no ticket is opened, and a ticket at height 0 is never
  evidence. Both fee bounds are capped at the same ceiling, so a floor above it
  cannot put the minimum over the maximum. A take's ticket closes on the wallet's own
  `confirmed_at_index`, not on the heartbeat that read it: the sweep polls one
  take per heartbeat, so a second ticketed take would otherwise turn an on-time
  confirmation into a late one and raise the fee.
- **Every fee the controller emits is a valid `Mojo`, and the conversion to one
  is now a named function.** The saturation ceiling was exactly 2^63 — the one
  `std::uint64_t` value that is *not* an `xop::Mojo` (`std::int64_t`, maximum
  2^63 − 1). It really was emitted: `fee_for()` clamps to
  `[min_fee_mojos, max_fee_mojos]` and both bounds are capped at that ceiling,
  so an operator writing a 19- or 20-digit `fees.max_fee_mojos` — the parser
  accepts any `uint64` and validates only `min <= max` — got 2^63 back from
  `get_recommended_fee`. C++20 makes the out-of-range conversion modular wrap
  rather than undefined, so it became `INT64_MIN` silently, on every compiler.
  The ceiling is now 2^63 − 1; the *comparison* bound stays at exactly 2^63
  (a constant just below `UINT64_MAX` rounds up to 2^64 as a double) and is
  renamed so the two can no longer be misread as the same number. A negative
  fee is worse than a huge one because every guard downstream ignores it rather
  than refusing — `CoinLockLedger::clamp_need()` zeroes it on the cancel path,
  and `ask_take_cost()` / `add_same_wallet_fee()` drop it on their own
  `<= 0` clause on the take path — so all **fifteen** `uint64` → `Mojo` fee
  conversions now go through `xop::to_mojo_saturating()`. Eleven replaced an
  explicit `static_cast`; the other **four** were implicit narrowings with no
  cast to grep for, and those four are exactly the `CoinLockLedger` fee
  arguments in `offer_manager.cpp`. (Counted, after an earlier draft of this
  entry said "fourteen … six of them implicit" — both numbers were wrong.) The
  two `posted × fee` products saturate too.
  `static_assert`s in `fee_controller.hpp` and `fee_tracker.hpp` make a wrong
  ceiling a compile error on every toolchain, and
  `tests/test_fee_controller_wiring.py` now pins every one of the fifteen call
  sites — see the entry below.
  **Not reachable on the shipped configuration** (`max_fee_mojos` 100,000,000),
  and not reachable merely by enabling the controller.
- **The rolling fee window is accounted EXACTLY, and saturates only when it is
  read.** An earlier draft of the paragraph above said the running total and
  its pruning subtraction saturated as well, and that was the bug. Saturating
  addition is lossy, so it is not invertible and no subtraction undoes it: with
  a history of `[UINT64_MAX, 100]` the total saturated to `UINT64_MAX`, and
  pruning the first entry compared `total > oldest` — `UINT64_MAX` against
  `UINT64_MAX`, false — and set the window total to **zero** while 100 mojos
  were still inside it. `budget_remaining()` is `daily_budget − total`, so the
  budget reopened in full: a fail-open on the one number the budget is, in the
  code added to close a fail-open. `FeeTracker` now keeps the window total as a
  128-bit unsigned value in two 64-bit halves — a carry on the way in, the
  matching borrow on the way out, both exact and O(1) — and clamps to
  `UINT64_MAX` once, in `get_rolling_total()`, where the clamped number is
  handed to a caller and never fed back into the arithmetic. The post-condition
  is that `get_rolling_total()` is the true sum of the fees still inside the
  window, or `UINT64_MAX` if and only if that true sum genuinely exceeds
  `UINT64_MAX`; every consumer of it is monotone in it, so the clamp can only
  refuse a spend, never allow one. **Also not reachable on the shipped
  configuration**: at `max_fee_mojos` 100,000,000 and the ~1,400 fee-bearing
  events this wallet's busiest day recorded, one window totals about 1.4e11
  mojos against the 1.8e19 needed to saturate. It takes a `max_fee_mojos` near
  2^63, which `config.cpp` accepts because it validates only `min <= max`.
- **Both budget alerts told the operator the wrong thing, and both are the
  signals the staged enable says to act on.** (1) The `FeeBudgetBound` episode
  latched and cleared on "the budget *lowered* the fee", which is false
  whenever the controller's own answer for an attached fee is already at
  `min_fee_mojos` — every level at or below `log2(42.3/21) = 1.010`, the bottom
  39% of the live band and the level the engine **boots at**. So an exhausted
  budget raised no alert there at all, and an open episode was *closed* by a
  quote taken from an empty window, logging `the fee budget no longer binds
  (headroom 0 mojos)`: an all-clear contradicted by its own number. The rule is
  now what the budget actually granted (`BudgetedFee::allowance`, the room
  above the reserve divided by the batch) against what was asked for, on both
  edges. (2) The `FeeBudgetUnfunded` episode's clear branch was the bare
  negation of its latch and never compared the fee *paid* with the headroom, so
  an accepted priority spend at any fee below its class's last quote ended the
  episode however far above the headroom it was — a 239,000,000-mojo escalation
  against a headroom of 0 read as "the budget funds priority spends again". It
  now ends only when the accepted spend fits the headroom, which is what the
  header always documented. Both are behind `controller_enabled` and both are
  load-bearing at the live `daily_budget_mojos: 10000000000`, which this PR's
  own startup advisory says is about 3x too small.
- **One cancel path is still degraded, and "a cancel is never degraded" is
  qualified rather than repeated.** `OfferManager::cancel_all`'s **bulk** sweep
  hands `current_fee_mojos_` to the wallet as `batch_fee`, and that is the
  budget-shaped *offer-attached* fee `Engine::set_dynamic_fee` last wrote — so
  with the controller on and the budget exhausted, a stop/shutdown Cancel All
  can go out at `min_fee_mojos` while the controller's own cancel fee is far
  above it. Every *per-offer* cancel is unaffected (`cancel_fee_for` reads the
  class-aware priority fees). Found by review at `cbf0301` and **not fixed
  here** — it changes the stop/shutdown sweep and wants its own review; the
  fix is to pass `max(cancel_fee_xch_mojos_, cancel_fee_cat_mojos_)` while the
  class-aware fees are in force. It must land before `controller_enabled:
  true`.
- **The saturating conversion had no guard of its own, and a merge gate proved
  it by measurement.** This release introduced `xop::to_mojo_saturating()` and
  routed fifteen fee conversions through it — and pinned none of them. On the
  four-way merged tree the gate dropped the wrapper from each of the four
  `CoinLockLedger` fee arguments in turn and **nothing caught it**: MSVC
  `/W4 /WX` builds clean because `uint64` → `int64` is a same-size conversion
  and `-Wconversion` is in neither toolchain's flags, the C++ suite passes
  because nothing in `cpp/tests` constructs an `OfferManager`, and no source
  scan mentioned `to_mojo_saturating` at all. The only thing ever holding those
  four lines was a neighbouring PR's literal text pin, which was loosened —
  correctly — so it could pass both alone and merged. Six assertions in
  `tests/test_fee_controller_wiring.py` now pin the invariant where it belongs:
  the `CoinLockLedger` fee argument is checked **positionally** (so a third
  `min_coin` argument cannot break it), the ternary, assignment, bind and
  ledger-leg shapes are checked individually, a bare narrowing cast on a fee is
  refused, and an exact per-file census makes dropping any one of the fifteen
  visible. **Scope, plainly: the code was correct at every site and the
  reachable impact is negligible** — the narrowing needs a fee above 2^63 mojos
  (~9.2 million XCH) and `current_fee_mojos_` is clamped by
  `fees.max_fee_mojos`. This is guard erosion, not a live defect.
- **Review round 9: four corrections, one of them a real signal defect.**
  (1) **`sent_to` is per peer, not a timeline**, so reading only its last tuple
  was wrong in both directions. Measured against this wallet's own `debug.log`
  (8 files, 38,250 non-empty lists): **8,289 carry more than one peer**, so an
  accepting peer followed by a fee-refusing one would have raised the fee on a
  spend already in a mempool; and **341 lists carry a fee refusal that is not
  last** and were dropped silently. The parser now scans the whole array — any
  peer reporting SUCCESS or PENDING suppresses the row, an unreadable tuple
  stops the row rather than being skipped over (it could be the acceptance),
  and otherwise any fee refusal counts. The residual is stated rather than
  glossed: `filter_ok_mempool_status` strips the SUCCESS/PENDING tuples on the
  resend tick, so a spend resting in an accepting peer's mempool later looks
  identical to a refused one — across all 38,250 live lists, **zero** ever
  carried a SUCCESS, which is what that filter looks like from outside. No
  reading of `sent_to` alone can separate those; what bounds it is the
  controller's dead time and its three-raise uncorroborated streak limit.
  (2) **The rolling fee window booked intent.** `cancel_fees_paid` re-derived
  each cancel's fee from a fresh `cancel_fee_for()` policy lookup, so a cancel
  that fell through to `emergency_cancel` and went out at a halved tier, at a
  secure fee of 0, or as a local-only cancel that spends nothing was booked at
  the full policy fee. `OfferManager` now carries the accepted fee out
  (`take_cancel_fees_accepted()`), which also books accepted cancels from
  routines that reached no booking site at all. Gated with the rest — the
  legacy branch is byte-identical to `main`.
  (3) **"Reported once" was not an invariant**: the refused-name set tests its
  capacity *before* inserting and then clears wholesale, so the sweep that
  trips 256 re-counts every refused row still visible in it, and names of
  deleted transactions are never shed.
  (4) **Class-aware cancel fees start at Step 8, which is after startup
  reconciliation** — so the bulk sweep is not the only cancel that misses them;
  every cancel the boot reconciliation issues pays the raw constructor fee,
  including the `OrphanDisposition::Unknown` path that fires for any resting
  offer on a disabled pair.
- **Observability.** One `[FeeController] rate a -> b mojos/cost (reason; n
  move(s)) -- fees now: ...` line per burst of changes, and two gauges,
  `xop_fees_controller_rate_mojos_per_cost` and `xop_fees_controller_level_log2`.
  At startup the controller reports which classes `max_fee_mojos` cannot get
  into a full mempool.
- **Keys**, all under `fees:` and read at startup: `cost_aware_estimate`,
  `controller_enabled`, `controller_target_delay_blocks`, `controller_kp` / `_ki`
  / `_kd`, `controller_max_error`, `controller_max_step_up`,
  `controller_min_raise`, `controller_warmup_observations`,
  `controller_probe_fraction`, `controller_probe_after_confirmations`,
  `controller_probe_confirmations`, `controller_probe_fail_bump`,
  `controller_probe_backoff_cap`, `controller_ff_margin`,
  `controller_ff_max_age_blocks`, `controller_budget_reserve_cancels` and four
  `controller_cost_*`. Ranges are validated; `controller_enabled` requires
  `min_fee_mojos > 0`.

### Operator notes

- **Nothing changes until a switch is turned on.** `cost_aware_estimate: true`
  alone changes fees (about 4.5x for a CAT cancel at the same node rate), and
  it makes cancels pay by the asset the offer offered, as the controller does.
- **Level 0 is `min_fee_mojos` exactly**, whatever its value. A very low floor
  (the example file's 5000) makes a very wide band: the startup log says how
  many raises, and roughly how many minutes, crossing it takes without the
  node's floor.
- **The live `max_fee_mojos: 100000000` cannot get an offer-attached fee, a CAT
  cancel or a take into a full mempool** — three of the four classes, not the
  two earlier drafts of this entry named. The node admits at 5 mojos per cost
  and the controller asks for `controller_ff_margin` × that (5.5 at the
  default), so the classes need 115,500,000, 232,650,000 and 687,500,000
  respectively; only the XCH cancel's 46,200,000 fits under 100,000,000. The
  startup log names each one. Before enabling the controller raise it to at
  least 250,000,000, or 700,000,000 to cover takes **as the shipped cost model
  prices them** — that figure is `5.5 × controller_cost_take` at the modelled
  125,000,000, and the measured take cost reaches **212,112,758**, which needs
  **1,166,620,169**. Raising the cap alone will not make the loop ask for that:
  the fee is `rate × controller_cost_take`, so pricing the largest measured
  take needs `controller_cost_take: 212112758` **and** `max_fee_mojos >=
  1166620169`. Keeping the shipped 125,000,000 is defensible — it was chosen to
  cover 7 of the 11 measured bundles rather than make the common take pay for
  the rare one — but then the 4 large takes are under-priced in a full mempool
  and the controller compensates by raising the *rate*, which raises every
  other class too. Also raise
  `strategy.cancel_escalation_max_fee_mojos` with it.
- **`daily_budget_mojos` is per `fee_window_blocks`, and 1662 peak heights is
  8.7 hours, not 24** (S69). At full-mempool prices one such window costs
  15,163,585,937 mojos on this wallet's measured action rates, so set the budget
  to at least 30,327,171,874 (35000000000 is a round number); the engine logs
  the figure for your configuration and warns below it. The 5000000000 to
  15000000000 suggested in earlier drafts was wrong at both ends: the bottom is
  less than the cancel reserve itself (25 x 232,650,000 = 5,816,250,000) and the
  top is about one window's spend.
- Found while measuring, not fixed here: Step 8's force-delete fires after a
  median of 176 seconds, not the ~10 minutes its constant documents (S68).
### Cancel far fewer offers (S70-S72) -- three switches, all default OFF

In the 14 days to block 9,319,413 the bot posted 1,283 offers, filled 15 and
cancelled 1,254 -- about 84 fee-bearing spends per fill, into ~97% full blocks.
Three rules made 97% of them. Each now has a replacement behind its own
`strategy` key; every key defaults to the old rule and needs a restart.

- **`ttl_cancel_mode: expire` (was 402 `ttl_expired` cancels).** An offer that
  verifiably carries the on-chain expiry from #150 is no longer cancelled at the
  hard TTL. The chain ages it out, and the bot then frees its coins with a free
  local cancel -- read from the chia 2.7.4 source: an expired trade stays
  PENDING_ACCEPT and stays in `get_locked_coins()` until cancelled, and
  `cancel_offer secure=false` releases it with no spend. The cancel is sent only
  when a chain clock (`get_timestamp_for_height` at the wallet's finished-sync
  height less 32 blocks -- a depth, because a seconds margin can be met by the
  tip block alone) is past `max_time`, the wallet still reports PENDING_ACCEPT, and
  its record repeats the tracked `max_time`; this host's clock only decides
  whether to look.
  **That clock is one connected peer's unvalidated assertion, not a local fact**
  -- chia's wallet forwards `get_timestamp_for_height` to whichever full-node
  peer answers first, unanchored (no `expected_header_hash`), so nothing checks
  a signature, a proof of space or a VDF, and the 32-block depth bounds reorgs
  only, never a liar. The retire therefore censuses the wallet's full-node peers
  (`get_connections`) before the clock and again after it, and retires nothing
  unless every peer is on this host, where chia's own `is_trusted_peer` trusts
  it unconditionally. **Enabling `expire` has a precondition: the wallet must
  reach the chain only through a local full node.** While it does not -- most
  obviously when that node is down -- retires pause, offers keep their coins
  locked, and the log says `no trusted chain clock`.
  `expire` with no expiry configured is refused at startup.
  **The wallet's TRADE RECORD is blind to a take of a retired offer. The
  wallet is not, and an earlier draft of this bullet said it was.**
  `get_trades_by_coin` skips CANCELLED (`trade_manager.py:131-139`), so
  `coins_of_interest_farmed` never fires, the trade never reaches CONFIRMED and
  no fill is booked -- that is the whole of what the evidence supports. A take
  still spends our maker coin AND pays the requested asset to a puzzle hash the
  maker's own wallet derived (`trade_manager.py:500,518`: each requested payment
  is a notarized payment to `action_scope.get_puzzle_hash`), so it moves the
  coin records and the balances this bot reads elsewhere. A sound local detector
  is therefore constructible; it is filed with its design as S78 and is **not**
  implemented here. Until it is, the shipped check is external: an offer this
  bot retired should end at Dexie `status: 6` with `spent_block_index: null`,
  and a `status: 4` with a block index means someone took it after the retire.
  The GUI pairs table sizes its resting-offer window from the expiry in this
  mode, so a quote that legitimately rests 24 h is not shown as absent after 6.
- **`exposure_rule: unified` (was 528 `exposure_floor_rebalance` cancels).** The
  pre-post projection and the resting-offer check now share one verdict. They
  disagreed because `spendable_balance` already excludes coins locked by resting
  offers, so posting an offer moved the second check by the size of whatever
  coin the wallet locked: on 2026-09-13 the bot cancelled 267 XCH/DBX asks
  this way, at one point re-posting the same tier every ~2.7 minutes. Unified
  projects from
  `unconfirmed_wallet_balance` against every resting offer that spends the asset,
  cancels only below `reserve x (1 - exposure_cancel_hysteresis_pct)`, never an
  offer younger than `exposure_cancel_min_age_blocks`, and suppresses the next
  post instead. A tracked offer whose pair this config cannot resolve -- an
  adopted `UNKNOWN` wallet record, or a pair since REMOVED from the file; a
  merely DISABLED pair is still mapped and still projected -- is recorded as
  UNQUANTIFIABLE rather than dropped, because `owned` still counts the coins it
  holds locked. While one is live, unified refuses to add exposure on either
  side and says so in the log; it never cancels a resting offer on its account,
  and the reload drain clears it on the next heartbeat.
- **`price_cancel_mode: margin` (was 282 `price_adverse` cancels).** Cancel for
  price only when a fill at the resting price would earn less than
  `price_cancel_edge_retain` x the edge Step 7 demands of a new offer, against
  BOTH of Step 7's centres (the shifted ladder centre and fair value), so it
  never cancels what the pricer would itself post nor churns an offer that still
  earns its edge. Crossed offers are still cancelled first; favourable
  drift never cancels. Replayed over the recorded fortnight it would have made
  82 of the 248 witnessed price cancels at the default 0.5 -- and the literal
  rule (1.0) fires on MORE offers than the rule it replaces, which is why 0.5
  is the default. New cancel reasons: `expired_onchain`, `margin_breach(..)`.
  **Read this before enabling it: the same replay fires on up to 7 of the
  fortnight's 15 FILLED offers**, at every retain from 0.5 up
  (`PriceCancelReplay.WhatItWouldHaveDoneToEveryOtherOffer`). The replay judges
  one centre where the live rule needs both to fail, so 7 is an upper bound;
  bounding the unrecorded fair centre by the 100 bps A-S rail brackets the real
  figure at 0 to 7 at retain 0.5, and at 3 to 7 at retain 1.0. This is the
  switch's real cost: `margin` also drops the anchor override, so a quote the
  market drifts AWAY from is never pulled back to the touch. It is not a pure
  reduction in wasted cancels.

Out of scope and unchanged: startup sweeps, Cancel All, reload-disabled pairs,
shutdown, every safety cancel, UTXO liberation, and the stopped-engine TTL sweep.
### A stop can keep the offers on the book (S74)

- **Stopping the engine no longer has to cancel everything.** A graceful stop
  cancelled the whole book: a wallet-wide cancel, the retry ladder, a cancel
  intent in `data/uncancelled.txt` and `cancel_pending` rows for the next start
  to finish. With blocks about 97% full that leaves unconfirmed cancel spends
  and locked coins behind every restart, and the only way to keep offers across
  one was to hard-kill the GUI and the engine. A stop now carries a policy,
  `cancel` or `keep`. With `keep` the engine sends no cancel of any kind (not
  the sweep, not the ladder, not the dead man's switch), writes no cancel
  intent, leaves every `offer_log` row as it is, disarms the dead man's switch
  before anything else, and logs one line: how many offers were left resting,
  on which pairs, and the soonest and latest on-chain expiry it knows of — or,
  plainly, that they carry none. The next start re-adopts them through the
  startup reconcile, exactly as it did after a hard kill.
- **The GUI asks.** Stop Trading and closing the window show a prompt with three
  choices — **Keep offers on the book**, **Cancel all offers**, **Don't stop** —
  the number of resting offers per pair and, when the on-chain expiry is on, the
  latest time a kept offer can stay takeable. It preselects the config default
  and remembers nothing. An OS session end (log-off, shutdown, restart) and a
  signal never show it: a modal box with nobody to answer would block the
  shutdown for ever.
- **`engine.shutdown_offers: cancel | keep`** (new optional section, default
  `cancel`, so an upgrade changes nothing) decides every stop nobody answers:
  a session end, SIGINT/SIGTERM, Ctrl+C in a console, a service stop, an older
  GUI, a hand-written `shutdown.flag`. It is printed at startup, and the engine
  warns there when `keep` is set while an enabled pair posts offers with no
  on-chain expiry. Strict: an unknown key or value in `engine:` is a startup
  error, because every lenient reading of a typo is a silent `cancel`. Settings
  → Risk Management → *Stopping the Engine* edits it; the save writes that one
  key only when the dropdown was changed, and re-reads the file first, so it
  neither adds the key to an untouched config nor reverts a value edited on
  disk.
- **Protocol.** The addressed v1 stop request gains one optional line,
  `offers=cancel|keep`. PID addressing, freshness and the truthful stop outcome
  (#153) are unchanged, and a request without the line is byte-identical to
  before. A line the engine cannot read does not refuse the stop: the config
  default applies and the log says so. An engine that predates the line would
  ignore it and cancel, so the GUI offers Keep only to an engine whose `--help`
  advertises `shutdown.flag offers=cancel|keep`, and refuses to stop rather than
  let a requested keep turn into a cancel.
- **A keep stop delivered by a signal waits for the one offer it may have been
  creating, and starts no more.** A GUI stop is read between heartbeat cycles
  and never finds a create in flight; Ctrl+C, SIGTERM, a service stop or a
  session end can arrive while one is outstanding, and stopping the event loop
  there would let the wallet finish an offer nothing recorded — an orphan the
  next start may cancel. The stop now waits for that create to land, on a timer
  and never on the wallet, for at most one create plus one publish at their own
  worst case (**247 s** with the shipped RPC timeouts). While it waits the
  engine begins **no new create at all**, and cancels nothing that already
  exists, so the wait really is one create's and not a whole ladder's.
  **Do not hard-kill a stop that seems slow** — that wait is what keeps a
  just-created offer from arriving at the next start as an orphan. A second
  Ctrl+C still exits at once. If a create instead fails with *no answer* — a
  timeout, an empty reply, a 5xx — that is not proof the wallet refused it, and
  the keep stop now says so at error rather than reporting a clean book.
- **What the stop report may and may not claim.** Three operator-facing
  sentences asserted safety the code does not provide, and are now built from
  the facts the report has already computed:
  - the mid-cycle line ended with a flat *"No offer post was left
    unrecorded"*, logged **unconditionally** and forty lines below the two
    facts (`post_abandoned`, `create_outcome_unknown_`) that exist to say the
    opposite. The contradiction was guaranteed, not incidental: a
    `post_abandoned` stop is always a mid-cycle one, because the only path
    that marks a post in flight runs inside the marked cycle. The reassurance
    is now conditional on both, and the same sentence no longer implies a
    **take** cut by the stop is recovered — a cancel is adopted from the
    wallet's `PENDING_CANCEL` record, but a take completed after the cut is
    booked nowhere in this engine (TODO S76 (c));
  - *"nothing was left on the book"* was printed whenever nothing was
    **resting**, although every `cancel_pending` offer is deliberately excluded
    from that count — and in this repo a submitted cancel is not proof: such an
    offer generally stays **takeable** until a maker coin is spent (24 of them
    for 2.5 h in August, three for 13 days). The stop then disarms the dead
    man's switch and exits, so nothing chases them until the next start. The
    line now says so, both when the book is otherwise empty and when it is not;
  - *"the dead man's switch is disarmed for this stop"* was flat, where
    `engine.hpp` is careful: a cancel the switch had **already begun** holds
    the mutex and is not recalled. The line now reads `watchdog_fired_` and
    says which of the two happened;
  - and *"nothing was left on the book"* survived the move. Relocating the
    sentence to `kept_book.hpp` took it away from `post_abandoned` and
    `create_outcome_unknown_`, which were never passed in — so a stop with a
    completely empty `State` printed the flat all-clear **first** and only
    then the error lines for those facts, each of which qualifies the *count*
    (*"NOT in the count above"*) and therefore retracts nothing about the word
    *nothing*. Weaker than the mid-cycle defect above — it needs an empty book
    rather than following by construction — but the same shape, so it has the
    same fix: the two facts are arguments to `describe_kept_book`, every
    branch of it says when a create this process began is unaccounted for, and
    a gtest reads each result.
  Each of these sentences now lives in `kept_book.hpp`, where a gtest reads
  exactly what the operator reads — the `engine.cpp` wiring scan strips string
  literals and structurally cannot. That is also the limit the last item ran
  into: the scan's `CLAIM_WORDING` backstop lists *"nothing was left on the
  book"*, but it reads literals in `engine.cpp`, so moving the sentence out
  moved it out of range. The replacement guard pins the **arguments** at the
  call site, and the sentences they produce are gtests.
- **The GUI stop prompt no longer says a cancel already in flight is untouched
  by the choice.** It read *"neither choice changes those"*. **Keep** does send
  nothing for them — but **Cancel all** seeds its list from every offer in
  `State` with no `cancel_pending` filter, writes every one of those ids into
  the cancel intent file before the first attempt, and its first attempt is the
  wallet-wide secure sweep, which in chia 2.7.4 performs no trade-status check
  at all: it takes the offer's cancellation coins and builds a fresh spend, so a
  merely `PENDING_CANCEL` trade is swept and re-spent. That escalation is what
  finally cleared the three XCH/BYC bids stuck for 13 days. (Only the per-offer
  **retries** skip such an offer, to avoid paying a second fee for the same
  spend; the wallet-wide leg does not.) The prompt now states the real
  difference. In the same place, a book whose every offer is `cancel_pending`
  was announced as *"No offers are resting on the book."* — the prompt reads
  those rows and then diverts them out of `resting` — with every informative
  line gated on `resting`, so the operator learned nothing else about them
  either. Such a book is no longer called empty, and gets a line of its own.
- **A blank `engine.shutdown_offers` is a startup error**, like every other
  value the section cannot read. `shutdown_offers:` with nothing after it used
  to fall through to `cancel` — the silent default this section exists to
  prevent. An omitted or empty `engine:` section is still "not set" and still
  means `cancel`.
- **The documented stop latency is the real one.** `config.example.yaml` said a
  GUI stop is *immediate* because the request is read between heartbeat cycles.
  The clause is the reason it is **not**: the flag is read once per 5 s poll and
  the same poll iteration then awaits the whole cycle inline. Measured over 976
  live cycles: median 10.4 s, p90 14.2 s, 1.5% over 30 s, longest 108.7 s. The
  paragraph now states that, and states that the GUI's own 30 s window ends in
  `TerminateProcess` — the hard kill the same paragraph tells operators not to
  perform — which skips the `offer_log` flush and can send an offer to the next
  start as an orphan. The 30 s value is **unchanged in this PR** and flagged for
  an operator decision (see the PR body).
- **Operator notes.** Keep is for restarts. A kept offer is takeable with no
  engine behind it — no repricing, no TTL, no dead man's switch — so use it
  only with `strategy.offer_expiry_secs` set. **While the engine is down the
  on-chain expiry is the only bound**: it is stamped when each offer is posted,
  so at the live `offer_expiry_secs: 86400` an offer can stay takeable for up to
  24 h from the moment it was posted, however long the stop lasts. What happens
  when the engine comes back depends on `strategy.ttl_cancel_mode`:
  with `cancel` (the default, and the only behaviour of any build before that
  key existed — there is no value spelled `age`) the first
  cycle cancels anything past the hard TTL — 2 × `offer_ttl_blocks`, 800 blocks
  ≈ 4 h 10 min at the peak-height cadence of 18.75 s/block — including offers
  whose expiry has already passed, because the reference wallet goes on
  reporting such an offer `PENDING_ACCEPT` and keeps its coins locked until it
  is cancelled (chia-blockchain 2.7.4, `chia/wallet/trade_manager.py`); with
  `expire`, an offer carrying a verified expiry is **not** cancelled at the hard
  TTL at all — it rests until its own `max_time` passes and is then retired with
  a free local cancel about ten minutes later, so the bound is the expiry (~24 h
  takeable, ~24 h 10 min of locked coins), not the hard TTL. Operator **Cancel
  All** is unchanged.

## [0.10.24] — 2026-09-14 — record what happened, not what was asked for

Nine merged branches, and most of them fix a record or a signal that reported a
request as an outcome: a cancel the wallet accepted was stamped `cancelled`, a
stop request was honoured by whichever engine read it, a maker fill was booked
on one of its three legs, a strategy-zeroed side was blamed on risk limits, a
stalled wallet never opened the wallet breaker, and the Dexie ticker's bid and
ask were exchanged. One branch adds the pace controller, which ships off. No
config change is required to upgrade, but the first restart acts on offers the
wallet still holds as PENDING_CANCEL — see Operator notes.

### Cancels stay pending until the wallet says they are done (#157)

- **An accepted cancel is no longer a cancelled offer.** `offer_log` became
  `cancelled` as soon as the wallet accepted a cancel RPC, although the wallet
  marks a trade PENDING_CANCEL before it pushes the spend, and that spend can be
  pruned or never broadcast: three XCH/BYC bids posted on 2026-08-30 were still
  PENDING_CANCEL, every maker coin unspent, on 2026-09-13. Every cancel site now
  writes the new status `cancel_pending`; only a wallet verdict completes the
  row, and `filled` still wins. Boot re-adopts the wallet's PENDING_CANCEL
  records, reopening rows already stamped `cancelled`, so a take on them is
  booked.
- **Re-cancel only with on-chain proof.** `escalate_stuck_cancels` sends a SECURE
  re-cancel with a raised fee only when the wallet still reports the trade live
  after its window (96 blocks, about 30 minutes) and the full node shows every
  maker coin unspent. No answer, or a partial one, pays nothing, and the
  escalation never marks an offer terminal. Each attempt is recorded with its
  fee before the fee is paid, so the cap (3 per offer) and the fee floor hold
  across restarts; after that, one CRITICAL `CancelUnresolved` alert. The sweep
  does not run while another cancel is in flight, while the wallet is failing,
  in XCH recovery mode, or after the dead man's switch has fired.
- **Seven `strategy.cancel_escalation_*` keys**, read at startup: `enabled`
  (true), `window_blocks` (96), `max_attempts` (3), `fee_step_mojos`
  (10,000,000), `max_fee_mojos` (100,000,000), `retry_blocks` (8) and
  `max_probes` (5). They are not yet documented in `config.example.yaml`.
- **Detection and remedy agree.** The Step 8 stuck counter, `cancel_stale` and
  the STOPDRAIN count share one predicate that excludes `cancel_pending`
  offers, so the every-block "attempting forced cancel" warning and the reload
  drain's every-heartbeat CRITICAL line stop.
- **GUI.** A `cancel_pending` row shows as **Cancelling**, with its cancel button
  disabled and a filter of its own; the Orders summary reads
  `Pending: N | Cancel pending: M`.
- **Database migration.** On first start `offer_closure_events` gains a nullable
  `fee_mojos` column (an idempotent `ALTER TABLE ... ADD COLUMN`); existing rows
  read NULL. It is the only schema change in this release.

### Bulk cancels sent once, as one fundable batch (#158)

- **A cancel is not re-sent after a failure that may have reached the wallet.**
  `rpc_post` re-sent every endpoint after a timeout or an HTTP 429/5xx. On
  2026-09-12 a re-sent `cancel_offers` arrived while the first request was still
  writing cancel records, and in chia 2.7.4 an admitted copy cancels those
  trades again and charges `batch_fee` again. `cancel_offers` and `cancel_offer`
  are now re-sent only after `CURLE_COULDNT_CONNECT` or
  `CURLE_SSL_CONNECT_ERROR`, which cannot have reached the handler.
- **One batch of 50 offers at 2 spends each, instead of batches of 5.** In chia
  2.7.4 each batch is its own bundle, and batches that start with a CAT coin
  select the same XCH fee coin, so at most one of them could land.
  `kCancelOffersSingleBatchSize` is 50, costed at two cancellation spends per
  offer (3,518,000,000, 64% of the 5,500,000,000 mempool cost bound): a book of
  up to 50 offers goes out as one fundable bundle with one `batch_fee` and one
  reserved fee coin. Larger books still split.
- **An unanswered sweep is not duplicated by an immediate per-offer cancel.** A
  timeout, an empty or unparseable reply, or a 5xx now means "possibly
  submitted": `cancel_all` logs `NO USABLE ANSWER` and treats every tracked
  offer as still live instead of cancelling each one at once. A refusal or a 4xx
  still falls back immediately. A 2xx reply whose body is not JSON now throws
  `ChiaRPCTransportError`.
- **Offers are re-checked before any re-cancel.** The shutdown ladder waits one
  request timeout plus 5 s (35 s at the defaults), asks the wallet about each
  offer, and re-cancels only those still live — none while the sweep still
  shows as running.
- **Operator Cancel All has a 125 s deadline**: that 35 s wait plus the shutdown
  ladder's 90 s budget, counted from when the sweep returned. At the deadline it
  re-cancels whatever is still live. No new probe or re-cancel starts after it,
  but a re-cancel already in flight can still start its emergency ladder's RPCs
  past it.
- **Still open.** The dead man's switch's own wallet-wide cancel (S31) still
  follows a shutdown ladder that stops unclean, and a sweep still queued behind
  the wallet lock shows no evidence yet, so its offers can be re-cancelled while
  it runs.

### Stop requests addressed to one engine (#153)

- **`shutdown.flag` is addressed to one engine PID.** At startup the engine
  treated any flag under 60 s old as a live request. On 2026-09-12 a closing GUI
  wrote one for its engine, a newly launched GUI killed that engine, and the
  next engine honoured the flag and stopped. The GUI now writes the engine's
  PID, and the engine honours a request only if it names this PID (or no PID)
  and was written at or after this process started; any other flag is removed
  with a WARNING.
- **Boot can be stopped.** Five boot checkpoints — before the stuck-transaction
  prune, before coin-pool maintenance, in the wallet-sync wait, before inventory
  seeding and before startup analysis — end boot on a stop request.
- **Stop outcomes are reported as observed.** The GUI logged
  `Engine exited gracefully.` whenever its wait returned. Each stop now logs
  what it saw — graceful, exited without consuming the request, terminated
  before or after consuming it, or still running — and an engine that survives
  terminate and kill stays managed, so Start cannot launch a second engine
  beside it.
- **Relaunching the installed GUI closes the old one.** The singleton kill never
  matched the installed `xop_trader_gui.exe`. A relaunch that finds a stop under
  way now waits up to 45 s for it before terminating anything.

### A stalled wallet opens the breaker (#156)

- **The breaker counted throws, and a stall throws nothing.** `detect_fills`
  catches each failed `get_offer` and Step 8's sync check logs and returns, so on
  2026-09-12 three heartbeats spent 156 to 173 s on doomed wallet calls and the
  breaker never opened.
- **It now reads transport evidence recorded in `rpc_post`.** Only a libcurl
  failure on the attempt that ends a call counts; an HTTP error, a malformed
  body or `success=false` is an answer. One unanswered transport failure skips
  the rest of that heartbeat's wallet work (`[WALLET-CIRCUIT] <step> SKIPPED`),
  and three consecutive failures, across callers and heartbeats, open the
  breaker. Every wallet call site in Step 8 has a checkpoint. The cancel
  escalation (#157), the pace balance refresh and Step 8's pace cancels (#160)
  also stand down while the wallet is failing by this measure.
- **Prune scope.** The startup stuck-transaction scan built its wallet list
  before the wallet-ID map existed, so it scanned the XCH wallet alone. It now
  builds the map first, scans every enabled pair's wallets, and skips the whole
  scan with a warning if the map cannot be built.

### Maker fills book every leg (#154)

- **Step 2 booked a maker fill through its base asset only.** The quote leg and
  the XCH fee never reached `InventoryTracker`, which feeds equity, the drawdown
  check and `inventory_ratio`: an ask removed the XCH sold but never added the
  proceeds. That was the false drawdown pause of 2026-09-11, after fourteen
  XCH/DBX asks whose DBX proceeds were never booked.
- **Every confirmed maker fill now books base, quote and XCH fee**, through the
  pure header `accounting/maker_fill_legs.hpp`.
- **`record_buy` refusals surface.** `InventoryTracker::record_buy` returns
  `bool`, and a refused base or quote leg now reaches the Step 2 error line and
  the ExposureBreach alert; a refused priced buy used to be reported as success.
- **What moves.** `inventory_ratio` reflects true quote holdings, so sizing and
  lean shift on the XCH pairs, and the first restart's one-shot reconcile
  absorbs the historic gap once. Taker fills still book no inventory legs (TODO
  S48).

### Dexie ticker sides (#159)

- **The ticker's bid and ask were swapped on every pair.** Dexie's `prices.buy`
  is what buying the token costs — its ask — and `prices.sell` its bid;
  XOPTrader read them the other way round on all four traded pairs. Our own
  resting XCH/BYC bid at 1.451 BYC per XCH was listed as `buy[0]` = 1/1.451.
  The fields are now `TickerData::best_bid` and `best_ask`, oriented by the pure
  `orient_market_ticker()`.
- **Fair value and the published spread are gated on book provenance.** Read
  correctly, an ordinary raw ticker book no longer looks crossed, and looking
  crossed was all that had kept it out of the fair-value inputs and the
  published `spread_bps`. Both now require `bbo_from_filtered_book`, and a raw
  book publishes spread 0. The every-heartbeat `Crossed book` lines on XCH/BYC
  and XCH/DBX stop.

### Step 6 names the side a limit cut (#155)

- **The warning blamed risk limits for both sides.**
  `Step 6: XCH/BYC -- both sides blocked by risk limits` fired every heartbeat,
  but the bid's zero came from the strategy, which sizes the bid from total XCH
  holdings above `q_max`; only the ask was zeroed by a limit.
  `PreTradeCheck::evaluate_limits` now records, per side, the rule that zeroed
  it and the rules that reduced it, and the line reads
  `no quote this block: bid … | ask … (zeroed by single_cat_cap; …)`. Quoting
  does not change.
- **Rate-limited**: a warn on the first blocked heartbeat or a change of cause,
  then once per 192 peak-height blocks (about an hour).

### Pace controller and per-pair concentration limits, off by default (#160)

- **Per-pair concentration limits.** `pairs[].soft_limit_pct_override` and
  `pairs[].hard_limit_pct_override` replace `risk.soft_limit_pct` and
  `risk.hard_limit_pct` for that pair only. With no override nothing changes.
- **The pace controller** (`strategy.pace_enabled`, default false) sells an
  overweight CAT quote asset listed in `pace_assets` toward its
  `asset_target_allocations` band, through the bids of its `XCH/<asset>` pairs,
  on a daily budget spread over `pace_horizon_blocks` (default 64,512, 14 days).
  It values the asset only at the independent fair value with fresh
  wallet-confirmed balances and holds on any data gap; every pace bid still
  passes every risk limit and is capped below fair value by at least
  `pace_min_edge_bps`. A pace cancel is recorded as `cancel_pending`, like every
  other cancel (#157). All 17 `pace_*` keys are documented, commented out, in
  `config.example.yaml`.

### Settings Save patches the ratio targets (#152)

- **A Settings Save no longer clobbers `strategy.ratio_target_by_pair`.** It
  rebuilt that map, the only per-pair ratio target the engine reads, from a
  GUI-only mirror that the Wallet tab's Apply never updates, so a Save could
  revert live targets. A Save now re-reads the file and patches only the rows
  the operator changed; untouched pairs keep their value on disk exactly, and
  the four Wallet-owned keys are also taken from the file.

### Operator notes

- **No config change is required to upgrade.** Every new key has a default, and
  the `offer_closure_events.fee_mojos` migration (#157) runs on first start.
- **The first restart acts on live offers (#157).** Offers the wallet still holds
  as PENDING_CANCEL are adopted as `cancel_pending`. About 96 blocks later, each
  one the wallet still reports live with every maker coin unspent gets a
  fee-bearing SECURE re-cancel — at most 3 per offer, then one `CancelUnresolved`
  alert. To avoid that, set `strategy.cancel_escalation_enabled: false` before
  restarting, or cancel those offers by hand first.
- **Edit `config.yaml` only with the GUI stopped.** A Settings Save writes back
  the snapshot the GUI loaded, so a key changed on disk while the GUI runs
  reverts; #152 fixes that only for `strategy.ratio_target_by_pair` and the
  Wallet-owned keys. Check `git diff config.yaml` before every restart.
- **Pace is off by default and needs the staged rollout described in #160.**
  Each stage is a `config.yaml` edit with the GUI stopped, a `git diff` check
  and a GUI restart: (A) the keys present with `pace_enabled: false`; (B) size
  only, `pace_enabled: true`, `pace_assets: [BYC]` and
  `pace_tighten_max_bps: 0`, observed for at least 24 h; (C) price,
  `pace_tighten_max_bps: 300`; (D) per-pair concentration overrides, only once
  the log shows `binding=Risk` or `binding=WalletConcentration`; (E) a shorter
  `pace_horizon_blocks`, only after (C) shows fills at the 14-day cap.
- **Pace rollback is restart-only.** Stop the GUI, confirm no `xop_trader`
  process is still running, set `pace_enabled: false` in `config.yaml`, check
  `git diff config.yaml`, then restart the GUI and confirm the `pace = off`
  startup line. A live disable from the GUI is not supported.
- **Log text.** Anything matching `both sides blocked` should match
  `no quote this block` instead (#155). New lines to expect:
  `[WALLET-CIRCUIT] <step> SKIPPED` (#156), `NO USABLE ANSWER` (#158) and the
  CRITICAL `CancelUnresolved` alert (#157).
- **TODO renumbering.** #153 and #156 each added a TODO entry numbered S47. The
  wallet breaker entry from #156 keeps S47; #153's entry, "`cancel_all.flag` and
  `pause.flag` are not addressed, so a successor engine inherits them", is S65
  in this release (#153's description still calls it S47).

### Known limitations

- **S36** — no test in `cpp/tests` constructs an `Engine`, so the engine wiring
  of these changes is compiled but not executed under test; the decisions are
  tested as pure functions, and call sites are pinned at most by source scans.
- **S49** — Step 11's one-shot reconcile can absorb a settled but undetected
  fill that Step 2 then applies again; since #154 its quote and fee legs
  double-apply too.
- **S52** — the cross-stable arb books an unmeasurable stable-pair spread as a
  free return leg.
- **S53** — Dexie's `last` under CAT keys looks reciprocal, so BYC/wUSDC.b may
  ingest 0.48 instead of about 2.083 (plausible, unverified).
- **S54** — fair-value inputs have provenance (#159) but no recency, so a pair
  keeps its last filtered book, at its old weight, through a Dexie outage.
- **S58** — a take whose wallet reply lacks a trade id is recorded as "unknown",
  and every later one is silently dropped, so pace can undercount its progress.
- **S59** — a resting pace bid above fair value is cancelled the first time it
  is seen there, with no age or hysteresis guard.
- **S65** — `cancel_all.flag` and `pause.flag` are not addressed, so a successor
  engine inherits them.

## [0.10.23] — 2026-09-13 — guards that can be aimed, and status that is asserted

Four merged branches, and one theme runs through most of them: a control that
cannot be aimed, or a signal that reports memory instead of liveness, is worse
than no control at all — because it reads as working.

### The crossing guard, and books with only one side (#148)

- **`classify_cross_bbo` is the live gate.** It previously fell back to a ±5%
  band around the published mid unless *both* touches existed, so on a
  one-sided book an ask at 99 against a bid of 100 was posted as uncrossed.
  The gate now consults whichever opposite touch exists — the one that could
  take the quote the moment it is posted — and falls back to the mid band only
  when that touch is missing. `book_inverted` still requires both sides. The
  staleness canceller was brought to the same rule; the two disagreeing was
  the original S33 defect.
- **Not strictly more conservative, and that is intended.** An ask at 90 over a
  bid of 80 with no ask side now posts where the mid band suppressed it. A
  resting ask above the bid is not crossed.
- **The soft-TTL contract is restored.** The `adverse` conjunct deleted in
  `922b183` had been expiring favourably-drifted aged offers.
- **Duplicate-slot double exposure**, breaker ownership, and MTM valuation
  defects closed; tier spacing no longer collapses an inverted ladder.
- **XCH/BYC ships DISABLED.** The calibration work is in, but enabling a
  trading pair is a deliberate act, not a side effect of merging a branch.

### Status that is asserted, never remembered (#148)

Five indicators reported health from retained state, so a real outage rendered
as a healthy-ish UI — the one period an operator is actually looking.

- The **node** dot no longer treats a retained block height as evidence; an
  explicit `node_connected = 0` wins, and "Syncing" became reachable.
- The **wallet** dot no longer reads merge-only cached balances as liveness.
- The **Dexie** dot is driven by a new engine-published signal,
  `xop_node{metric="dexie_connected"}` — a freshness window over the last
  *answered* Dexie request. `metrics_connected` only ever said whether the GUI
  could reach the **engine**; with the engine healthy and every Dexie request
  failing, the dot stayed green. Both gates are now required.
- Single-cancel rows no longer latch on "Cancelling…" forever.

### Wallet-wide operations that could not be aimed (#149)

- **Bulk cancel missed non-XCH offers**, and the transaction window hid
  unconfirmed rows entirely — `reverse=true` sorted them out of the newest-200
  window on wallets holding 32,966 and 9,640 rows.
- **Stuck-transaction pruning was dead code**, and fixing the window made a
  destructive path reachable: `delete_unconfirmed_transactions` is
  wallet-wide, so one stale row would have authorised deleting this
  heartbeat's offer creations and any unconfirmed secure cancel beside it. It
  is now gated on no fresh row existing, and a row whose age cannot be read
  counts as fresh — an unknown age is not evidence of staleness.
- **A stop with an empty local book never swept the wallet**, so a shutdown
  could report "all offers cancelled" having issued no cancel at all.

### Reward accounting (#149)

- **Reward ingest was booking nothing**, for the same window reason: the scan
  read the oldest 200 rows, which sat below the ledger genesis block.
- Receipts are now **bounded to two days** before they stop being valued at the
  live price, because the ledger row is idempotent — the first price a receipt
  sees becomes its cost basis permanently.
- **Idempotency is consulted before freshness.** A receipt booked while fresh
  was being re-counted as stale once it aged past the cutoff, and the warning
  then reported already-booked rows as unresolved divergence, every heartbeat,
  for as long as the window held them.

### Configuration and offers (#150, #151)

- **Non-finite numerics are rejected at config load** rather than propagating
  `inf`/`nan` into pricing, and a startup warning that had been false for two
  weeks was deleted.
- **Opt-in on-chain offer expiry** via `max_time`, verified fail-closed: a
  wallet that ignores the field must not leave offers resting forever without
  anyone noticing.

## [0.10.22] — 2026-09-03 — a switch for the venue, not just for the quoting

The toolbar switch says whether Permuto is QUOTING. Nothing said whether
Permuto is *there* — and the difference is not cosmetic, because a venue
nobody has armed is still working:

- `VenueSwitch.__init__` ends in `refresh()`, which calls `_gather_permuto`,
  which opens and `yaml.safe_load`s the whole of `secrets.yaml`. So does
  every later tick: `_refresh_venue_switches` runs from `_on_bridge_data`,
  several times a minute, forever.
- `PermutoWidget.__init__` calls `refresh()`, which reads the identity
  again — during window construction, before anything is shown.
- `set_bridge` schedules the "Permuto: On at startup" arm on a 1.5 s timer.

That cost is paid on every installation, whether or not anyone ever arms
the venue -- including the ones that never will.

- **A master switch**, in Settings → Advanced → Subsystems, persisted to
  `QSettings` under `permuto/enabled` beside the existing `permuto/curfew_enabled`.
  Off removes the toolbar switch, the sidebar entry and the page, refuses the
  startup arm, and stops both identity reads. It **defaults to off** — opt
  in, matching `load_startup_states`, which has always defaulted Permuto to
  `"off"` for the same reason. That is not a statement that the venue is
  finished -- it is under active development -- but an installation that
  has expressed no preference should not pay for it. Nothing is lost
  either way: the Startup tab's "Permuto at startup" and curfew
  preferences are kept untouched and apply again from the first launch
  after the switch goes on.
- **Hidden, never renumbered.** `_NAV_ITEMS` keeps all eleven entries and
  index 9 keeps a page — a placeholder rather than a `PermutoWidget`. The
  page indices are positional, and the last time that slipped, `_PAGE_SETTINGS`
  stayed 9 while Permuto took it, and the first-run "you have no config"
  redirect opened a key-generation screen instead of Settings.
- **Off is a venue stop before it is a UI change.** With a live session the
  toggle confirms, stops, and `join()`s — `stop()` only sets a flag; the
  cancel lands seconds later on the worker thread — then verifies the book
  is empty. If the cancel is not confirmed it **refuses to hide anything**
  and says so. The clean-stop path disarms the venue-side scheduled cancel
  as soon as `cancel_all` reports success, so hiding the page over a book
  that did not actually go away would remove the operator's close control
  and the net underneath it in the same click.
- **Master off means every Permuto switch reads off.** An invariant, applied
  on every Settings build and not only on a click, so a store carrying an
  older `startup/permuto = "on"` — which is exactly what a machine that ran
  the contest has — is corrected the first time Settings opens rather than
  lying in wait until the subsystem comes back. "Permuto at startup" follows
  to Off; the page's Markets polling switch is unchecked through its own
  handler, so the button stops reading "Stop polling" over a stopped timer.
  Two controls are deliberately exempt: the **overnight curfew**, whose off
  position disarms a liquidation protection rather than stopping activity
  and which does nothing at all while the subsystem is off, and the
  **backup-confirmation checkbox**, which is a record that the operator
  wrote down their recovery phrase, not a switch.
- **Off applies immediately. On applies immediately too — unless the
  session started with Permuto off.** Switching off and back on again just
  unhides surfaces this session already built. Only a session that *started*
  disabled has nothing to unhide: the toolbar switch and the page are built
  during window construction and the indices are positional, so there is
  nowhere to insert them afterwards. That case — and only that case — raises
  a dialog saying so, because with the default now off it is the ordinary
  path, and a tick box that appears to do nothing reads as broken.
- `disabled` is a first-class gate in `venue_control`, ordered **above**
  `watchdog` and `breaker`. Those say why a venue will not trade; this says
  why the venue is not here, and an operator who switched Permuto off must
  not be told the dead man's switch fired.

Review round (PR #147) turned up three more, all in the disable path itself:

- **The flatness verdict was read from a queued signal.** `PermutoLive`
  emits `book_state` from the worker thread, so the slot that updates
  `_book_empty` is queued to the GUI thread's event loop — the loop
  `join()` stops pumping the moment it calls `wait()`. Reading
  `book_is_empty()` straight after a blocking join returned the value
  `start()` left behind, so an honest clean stop was refused as "the book is
  not confirmed empty". `join()` now returns the worker's own record of the
  final cancel, written on the worker thread before the emit, and the master
  switch gates on that.
- **The halted runner was kept.** `join()` fences the client with
  `halt_placements()`, permanently and by design. Keeping the object meant
  the next arm found `_permuto_runner is not None`, skipped building a fresh
  session, and started the halted client — every `batch_upsert` back as
  "placements halted for shutdown". It is discarded now.
- **Any runner object counted as a live session.** An ordinary toolbar stop
  leaves the object assigned after its thread finishes, so disabling later
  raised a "a Permuto session is live" confirmation over a venue that had
  been flat for hours, and ran the whole live-stop path. It asks
  `is_running()`.

Also: disabling now refuses while an operator close is in flight. That
worker owns the venue session and cannot be joined away —
`stop_background_work` gives it `CLOSE_JOIN_MS` and then abandons the thread
rather than terminate it mid order — so hiding the page over it would take
away the close control while the close was still on the wire.

Three pre-existing faults surfaced while gating this and are fixed here:

- `_make_permuto_live` read `page._target_depth_usd` and `page._max_position_usd`
  directly. Index 9 is not always a `PermutoWidget` — `_create_page_widget`
  already substitutes a placeholder whenever a page's import or constructor
  fails — so arming after such a failure raised `AttributeError`, swallowed
  as `could not start Permuto quoting: '_placeholder' object has no attribute
  '_target_depth_usd'`. Now `getattr`, falling back to `PermutoLive`'s own
  defaults, which are the same numbers.
- `Sidebar.select_page` bounds-checked its argument and nothing else, so a
  hidden entry stayed programmatically selectable.
- The Advanced tab's five dirty-tracking lambdas passed tab index 10, which
  is Startup. Editing the raw YAML box put the unsaved marker on a tab whose
  controls write straight to `QSettings` and are never part of a save, while
  Advanced's own unsaved edits showed as clean.

`tests/test_sizing_path_wiring.py` and the two window fixtures in
`gui/services/permuto/tests/test_venue_control.py` now seal the identity and
the startup/enabled loaders. The sizing test is the only one that calls
`set_bridge` on a real window, and `set_bridge` schedules the startup arm:
on a box whose operator had registered an identity and stored "Permuto: On
at startup", a test run was one `start()` from placing live orders.
## [0.10.21] — 2026-09-03 — expand BBO fetch budget & micro-tick requote drift tolerance

- Expanded per-tick BBO fetch budget to 2.0s (1.0s timeout per request) in `live.py` to prevent
  premature sub-tick BBO timeouts across multiple markets.
- Added micro-tick tolerance in `quoting.py` and `runner.py` requote-drift calculation to prevent
  unnecessary churn on tight micro-tick ring-edge placements.

## [0.10.20] — 2026-09-03 — pre-reduce cancel-all to prevent single order accumulation

- Added market-scoped pre-cancel before posting risk-reducing single orders in the carried-session
  stress margin fallback, preventing open order accumulation toward the venue's 200-order cap.
- Drained stale open order backlog from previous single-order executions.

## [0.10.19] — 2026-09-03 — full three-market two-sided depth quoting & sub-tick boundary clearance

- Enhanced micro-tick BBO window calculation with floating sub-tick boundary tolerance in `bbo.py`
  and `orders.py` so that tight competitor bids at the exact +2.0% ring ceiling do not shut down
  quoting on `QQQ-VOL-PERP`.
- Increased sizing defaults to $10,000 target depth and $250,000 max position, accelerating balanced
  depth accrual across all 3 active markets to >32M depth-seconds per hour.

## [0.10.18] — 2026-09-03 — seamless carried stress margin risk-reducing order execution

- Automatically extract and post risk-reducing limit orders via `/exchange/order` when the venue's
  carried-session 8x stress margin check rejects full `batch_upsert` payloads.
- Prevents unhandled HTTP 400 error latching and toolbar "blocked" state while actively working off
  held short inventory overnight.

## [0.10.17] — 2026-09-03 — unblock two-sided Permuto depth quoting & recalibrate portfolio exposure

- Recalibrate portfolio max exposure fraction to 2.5x equity (aligned with Permuto's 10x
  clearinghouse leverage limit) so existing inventory holdings do not prematurely choke
  off risk-increasing opposite-side quotes.
- Prevent preflight quantisation from rounding valid BBO micro-tick prices back into the best bid,
  preserving two-sided resting placements inside the +/-2% credit ring.
- Wire runtime sizing defaults to $6,000 target depth and $150,000 max position to sustain continuous
  balanced depth accrual toward the 300M competition eligibility gate.

## [0.10.16] — 2026-09-03 — micro-tick Permuto BBO depth & carried stress margin fallback

- Micro-tick (`1e-6`) resolution fallback in Permuto BBO calculation: when competitor
  orders park with sub-tick precision near the +2.0% ring ceiling, the runner falls back
  to micro-tick resolution so valid resting asks inside the scoring ring are not falsely
  shut down.
- Carried-session stress margin fallback: when `batch_upsert` is rejected during
  carried/overnight sessions by the venue's 8x portfolio stress margin check, the runner
  automatically falls back to sending risk-reducing legs via `/exchange/order` so existing
  short inventory can be actively worked off.

## [0.10.15] — 2026-09-03 — autodetect wallet fallback & reliable shutdown cancels

- Fall back to inspecting local Chia wallet databases (`last_used_fingerprint`
  and `blockchain_wallet_v2_*_<fp>.sqlite`) during fresh install bootstrapping
  when the `chia` CLI is absent from `PATH`, preventing initial startup failures.
- Robust dead man's switch and shutdown cancellation: retry transient wallet
  desyncs and network errors rather than leaving resting offers live on shutdown.
- Money-path compile-time type safety via `denom.hpp` to prevent unit/scale
  mixups across mojos, CATs, and USD notionals.
- Documented S39(b) competitiveness-gate analysis for XCH/BYC and XCH/DBX.

## [0.10.14] — 2026-09-02 — place against the book that is actually there

The Permuto runner banked zero depth through a full session while three legs
per batch appeared accepted. The rejected legs were asks crossing bids parked
near the +2% credit-ring ceiling. The fallback controller could only learn a
retreat from repeated refusals, and its unrelated re-quote budget declared
valid QQQ and TSLA earning windows shut.

- Read `GET /info/l2/{market}` and compute exact, tick-aligned bid and ask
  prices that rest post-only inside the true-oracle depth ring. Each side is
  clamped independently, so inventory skew cannot move a clearing ask back
  through the bid.
- Detect a one-sided order blocking its own missing side, cancel it once, and
  rebuild against the external book. External walls stay untouched, and one
  shut market no longer marks healthy resting siblings globally blocked.
- Revalidate placement after the send-time oracle refresh. If either side is
  invalid, drop the whole pair: a lone sibling earns zero under
  `min(bid, ask)` and is only unbalanced exposure.
- Preserve deliberately wide BBO quotes without weakening the tighter churn
  guard for blind backoff. Apply the documented 8x carried sizing from the
  schedule because the REST venue state does not expose a reliable carried
  flag.
- Keep genuinely unscheduled moving markets at the reduced session profile;
  only an explicitly disabled curfew selects full target depth. The field
  monitor now reads and labels the venue-published ring instead of assuming 2%.
- Set the contest posture to $6,000 target depth per market and a $30,000
  position cap. Two currently placeable markets project 413.6M depth-seconds
  over the measured remaining stage schedule from a funded account.

## [0.10.13] — 2026-09-01 — one side of a book can be junk

XCH/BYC stopped quoting on 2026-08-30 and the reason was not price
discovery. The engine had already solved the pair correctly: CoinGecko
XCH/USD combined with BYC's declared par gave `XCH/BYC = 1.41022765` at
sigma 171 bps, and Step 7 replaced the raw book mid of 3.2498 with
1.41141912 at full external weight. Three consumers downstream then
preferred the book to the number, and the pair went silent *because* the
solver was right.

The book, in the pair's own orientation (BYC per XCH), with XCH at $1.43:

| side | levels | implies BYC |
|---|---|---|
| ask | 4.9995, 5.0000, 9.7500, 10.0000 × 3 | $0.286 … $0.143 |
| bid | 1.5000, 1.4283, 1.4066, 1.3793, 1.3699, 1.3514 | $0.953 … $1.058 |

Every bid is within 6.4% of the anchor. Every ask is 3.5×–7.1× it. The
"10,769 bps spread" was never a spread — it was an **absent ask side**
wearing the costume of one. Nothing in the tree could say that, because
every consumer read `best_bid`/`best_ask` as an atomic pair.

- **Per-side anchor agreement**, in the new pure header
  `execution/book_side_quality.hpp` and carried on `MarketSnapshot` as
  `bid_side_anchor_ok` / `ask_side_anchor_ok` / `book_side_ref`. A side
  whose best dust-filtered price sits outside 3× of the *independent*
  anchor is no longer treated as a reference. Computed from `ref_price`,
  never `offer_ref_used` — letting a pair's own last accepted mid
  disqualify a side of its own book is the self-referential lock-in that
  made the 187.461980 mid unkillable.
- **The offers are flagged, not removed.** Stripping the ask side takes
  `dex_best_ask` to 0, `compute_mid` Case 2 refuses to publish a lone bid
  as a mid, Case 3 wants a fresh print a silent pair does not have — so
  the pair publishes no mid, Step 4 invalidates the quote, and the correct
  1.41 never reaches the ladder. Silence instead of a mispriced quote is
  not an improvement when the alternative is quoting correctly.
- **The two-sides-agree bypass** is the part that must not regress. A
  genuine market-wide repricing moves both sides together and leaves the
  book coherent — the same evidence `mid_gate::book_confirms()` accepts to
  override an anchor breach. A two-sided book whose own spread is at most
  5000 bps is therefore trusted whole, however far it sits from the
  anchor. Dislocation is one side moving *alone*, and that always leaves a
  wide spread behind. Pinned to the same default as the gate's own
  confirmation threshold so the two halves cannot be configured into
  contradiction.
- **Step 8 Check 1 is SKIPPED when a side is disqualified, not
  re-referenced — and an earlier revision of this entry got the mechanism
  wrong.** It claimed Check 1 compared Step 7's centre (1.41141912) against
  `bbo_mid` (3.24975) for 56.6% and cleared every tier. Step 8 performs no
  such comparison. `mid` there is the **published** mid, and for a pair
  with no CEX or AMM leg the published mid *is* the BBO midpoint —
  bit-for-bit `bbo_mid_m`. On the live book the deviation is identically
  **zero**, and this check has never fired for XCH/BYC.
  Re-pointing it at the surviving side, as an earlier commit on this branch
  did, turned that 0% into `|3.24975 − 1.5| / 1.5` = **116.7%**, clearing
  every tier on every block — silencing the pair this work exists to
  un-silence, by a different route. It now skips instead, which restores
  the pre-existing behaviour exactly. Both operands of this check are
  derived from the book being judged, so no substitution makes it
  meaningful. Check 2 is unaffected: it compares **tier prices**, which are
  built around Step 7's centre, so substituting that centre compares like
  with like.
- **Step 8 Check 2** measured our correctly-priced 1.41 ask against the
  4.9995 junk ask and called it 71.8% "aggressive", killing it, while a
  1.41 bid passed at 6.0%. It now references the independent anchor when
  the tier's own side is disqualified. The effective midpoint moves too:
  `classify_tier`'s bid passive rule would otherwise read any bid up to
  3.24975 as a safe passive rest.
- **The bid cap, which was live money.** `bbo_ref` is the midpoint of the
  two sides, so one dislocated side moved the reference meant to police
  it: `bid_cap = 3.24975` never bound, and the competitive anchor parked a
  bid at 1.5001 against a 1.4102 fair value — a ~6.3% overpay on every XCH
  bought, every cycle. A disqualified side no longer contributes to
  `bbo_ref` — which is the whole of the fix, and is what the two ladder
  regression tests pin. (The accompanying `min(bbo_ref, mid)` is
  decoration: in the disqualified state `bbo_ref` has already fallen back
  to the model mid, so the `min` cannot bind. It is retained only as an
  explicit statement of intent.) Healthy books are byte-identical: this is
  shared hot-path code for the pairs that actually earn, and the
  pre-existing `bid_cap == bbo_ref` rule for healthy books is a separate
  decision that is not revisited here.
- **A unit test reproduces the incident exactly.** On the unexamined path
  the three bids anchor above the ask tiers and the cross check drops
  **6/6 tiers** — the 2026-08-30 log line in shape. The outcome was never
  "a slightly expensive bid"; it was no quotes at all.

### The false depeg, which would have re-fired ten minutes after any restart

- **`step_observe_asset_pegs` read a mojo-scaled mid as a USD price.**
  `snap.mid_price` is 1e12-scaled; dividing XCH's USD price by it gave
  `usd_obs ≈ 3.4e-13` on every heartbeat and produced
  `[PEGSUSPEND] observed $0.0000 vs target 1.0000, 100.0% off` — for
  wUSDC.b on 2026-08-29 and BYC on 2026-08-30, both false, both cancelling
  every resting offer on every pair touching the asset. `asset_peg_rt_` is
  in-memory only, so a restart cleared the latch and re-armed it 30
  heartbeats (~10 min) later.
- **Fixing the scale alone was not enough**, and this is the part that
  needed deciding rather than patching. Correctly scaled, the observation
  on that book is $1.43 / 3.25 = **$0.44** — still 56% off par, still past
  `bail_pct`. The route reads the *published book mid*, so it inherits the
  junk side. It now refuses to observe at all when a side is disqualified,
  which routes into `observe_peg`'s data-gap branch and **holds** the
  streak rather than advancing or resetting it.
- **Deliberately not re-sourced to the fair-value estimate**, which is the
  obvious-looking alternative and is circular: `par_anchor.hpp` feeds the
  *declared par* into the fair-value solve precisely when nothing else can
  price the asset, so a peg watcher reading that estimate would read its
  own input back, sit permanently at par, and never detect the depeg it
  exists to detect. A peg must be observed from a market or not at all.
- **The Step 4 suspension gate now logs at warn.** It silently suppressed
  every XCH/BYC quote for a day at debug level. A safety latch that halts
  trading has to announce itself in the normal log.

### Trade history: asked, answered, and written down

The prompting question was whether dexie's trade history could supply a
price target for BYC when the book is unusable. It can — the tape says
~1.40 BYC/XCH, i.e. BYC at $1.02 — and that turns out to be the wrong
thing to want. `1.41 = XCH/USD ÷ BYC par`, so the engine already had the
number; the tape's agreement adds no information, its content is already
spent as `fair_value_par_market_sigma_bps = 140`, and wiring it in as a
live edge would *displace* the operator-governed par rather than confirm
it, because the par fallback is fallback-only. See
`docs/price-discovery-from-trade-history.md` for the full argument and the
microstructure literature behind it.

- **`docs/price-discovery-from-trade-history.md`** — why the tape is
  corroboration here and not an oracle, which estimators degenerate at
  this trade count and why (Roll is undefined when sample autocovariance
  is positive; information-share metrics cannot fit a VECM across 8-day
  gaps; Stoikov's micro-price symmetrises the two sides, the exact
  assumption this book violates), and the two standing caveats — par is an
  assumption rather than an observation, and ~19% of the August tape is
  our own fills.
- **`scripts/byc_price_diagnostic.py`** — read-only. Re-derives the
  "7-day traded VWAP 1.001" figure that **five** sites cite as ground truth
  and no code path produces (`par_anchor.hpp`, `config.hpp`, `engine.cpp`'s
  `quote_usd_factor`, `test_fair_value.cpp`, `config.yaml`), and reports the
  executable depth near par that a VWAP cannot. Sample size printed beside
  every statistic.
- **`scripts/highlow_spread_estimator.py`** — read-only Corwin-Schultz and
  Abdi-Ranaldo estimators. Their inputs are **not** `price_high`/`price_low`:
  those are fetched and discarded unread by the engine, but a dexie 24h
  high/low is a single bar and both estimators need two consecutive ones, so
  the optional `--dexie` flag is a separate range check that cannot drive
  either estimator. The multi-day bars come from the third-party BBO series
  we already store — `offer_log.book_best_bid`/`book_best_ask`, or
  `snapshots.mid_price_mojos` + `spread_bps`, which invert to the same two
  sides exactly. So these are **quote samples standing in for trade prices**,
  outside the regime either estimator was derived for; the script says so in
  its own output and the caveat stays in front, not buried. Diagnostic only,
  and what it offers is a descriptive discrepancy rather than a measurement.
  Measured like-for-like on XCH/BYC 2026-09-01 against the most recent day
  pair, the posted 14,666.7 bps is **0.99×** the 14,863.9 bps comparator —
  no gap at all — and even that is soft, the comparator being 74% of the
  20,000 bps ceiling Corwin-Schultz saturates toward.
- **Retraction — the "a fortiori" claim about `quote-touch` bars.** This
  entry previously said the default `quote-touch` bars give an *upper* bound
  on the estimate, so the gap held a fortiori. Withdrawn, not hedged:
  Corwin-Schultz **subtracts** the two-day range term `gamma`, so the
  estimate is not monotonic in the sampled range — a wider range can lower
  it, to zero. Abdi-Ranaldo has no monotonicity guarantee either. A bar
  construction is a sensitivity choice, not a bound. Worked counterexample
  in `docs/price-discovery-from-trade-history.md` § 6, recorded there rather
  than silently edited.
- **Second retraction — the 8.0× gap itself.** An earlier revision of this
  entry said the 8.0× figure "is measured and stands". It does not. It came
  from dividing ONE instantaneous quote sample by estimators averaged over a
  month of day pairs — the window mismatch the same release fixes. Compared
  like-for-like the ratio is 0.99×. Both retractions point the same way:
  these estimators do not
  identify sides — the absent-side finding for XCH/BYC rests on the direct
  book observation (bids within 6.4% of the 1.41022765 anchor, asks at
  3.5×–7.1× it) and never depended on them.
- **`get_trades()` sort fix.** It passed `date_completed_desc`, which the
  dexie API does not recognise and silently ignores, so it returned an
  arbitrary page rather than recent trades. The valid value is
  `date_completed`, already descending. The function still has no callers,
  and now says why.

### The `enabled:` flags, and the one deployment order that matters

Nothing in this entry argues for or against quoting BYC — that is a
liquidity decision and not a code problem. The operator made it separately:
**XCH/BYC was re-enabled on 2026-09-01 and then **backed out the same day, pending deployment**. The decision stands; only its timing changed. An enable is inert until the engine restarts, but the GUI relaunches the engine whenever the GUI restarts -- so the flag was a latent trigger that any unrelated restart could arm without anyone choosing to. Leaving a loaded trigger in a config file and relying on nobody touching it is not a safety argument. Re-set it AFTER this PR is merged, built and deployed.** BYC/wUSDC.b stays off throughout.

**Why the timing matters, and why the flag ships `false`.** `[RELOAD]` disables a
pair live but refuses to enable one — it logs "restart the engine to start
quoting it" and carries on. On the pre-PR binary a restart with the flag set
reproduces 2026-08-30 exactly: the ladder self-crosses and drops every tier,
then the mojo-scale bug latches a **false** depeg about ten minutes in and
cancels every offer on every pair touching BYC, and that bogus valuation
feeds the 10% drawdown breaker — which pauses the **whole engine** and takes
XCH/DBX, the only earning pair, down with it. Every link in that chain is
addressed above. Merge, build, deploy, *then* restart.

The two sides are not symmetric, and there is no per-pair one-sided switch,
so enabling turns on both. In the pair's own orientation (price = BYC per
XCH) a **bid** pays BYC to buy XCH, which sells our 52.58 BYC at about $1.01
into the honest side of the book — an exit at par, and the reason to be
here. An **ask** accumulates more BYC, and there is no exit for that:
nothing bids for BYC above about $0.29. Rising BYC inventory is the signal
to turn it back off.

BYC/wUSDC.b remains disabled for an unrelated reason: the 2026-08-25
warp.green bridge compromise depegged wUSDC.b (~$0.80 on 2026-09-01) and the
pair has had no print since 2026-08-24, so it would be quoting into a dead
book through a broken denominator.

### Per-pair BBO proximity caps, and the research that shrank the feature

The operator asked for a per-pair percentage band on how far offers may sit
from the market. The research answered a narrower question than the one asked,
and this is the narrow answer.

**The band already existed.** `strategy::classify_tier`
(`bbo_sanity.hpp`) is a per-side percentage max-distance bound, measured
against the **same-side BBO**, that **suppresses rather than clamps**, with an
ordered fallback chain. That is the proposal, shipped. What was missing is
that its two thresholds were **global**, and the pairs are not alike: XCH/DBX's
sigma width floor never exceeded 141 bps in 48,402 logged evaluations while
XCH/BYC's exceeded 200 bps on 87.7% of them. One number cannot serve both — a
bound tight enough to mean anything on the first silences the second.

- `bbo_sanity_max_aggressive_dev_override` and
  `bbo_sanity_max_passive_dev_override` on `PairConfig`, resolved at the Step 8
  call site with the same idiom `max_half_spread_bps_override` already uses.
  Absent → the strategy-level value, so every existing pair is unchanged.
- **Fractions, bounded (0, 1], rejected at load otherwise.** `10` ("10%") and
  `1000` ("1000 bps") are both plausible operator entries and both would yield
  a cap that can never bind — a suppression control silently switched off. Zero
  is rejected too: it would suppress every tier on that side, and a config
  value that quietly stops a pair quoting is worse than one that fails to load.

**Three things the research killed, recorded so they are not re-proposed:**

- **Do not reference the mid.** On 2026-08-30 XCH/BYC's mid read 5.575 against
  a fair value of ~1.40; a mid-referenced 2% band would have forced correct
  1.33 bids up to 5.4635 — paying ~5.46 BYC for an XCH worth ~1.40, filling
  instantly. Same-side 2% forfeits **5.6%** of realized P&L; mid-referenced 2%
  forfeits **78.4%**. No production band in the verified literature — LULD,
  Nasdaq Rule 4702(b)(7), CME, Binance, Kraken, Hyperliquid, Xetra — references
  an instantaneous mid.
- **Depth-qualifying the reference is worse than useless here.** Depth walking
  is monotone *away* from the book's interior, so it can only repair a touch
  that is too *aggressive* — but on a venue with no matching engine, aggressive
  offers get consumed and passive ones fossilize (Spearman ρ between
  dislocation and resting age = **+0.615**; median age 0.41 d within 10% of
  fair versus **125 d** at 10–100% off). Its helpful direction covers only the
  small errors; its harmful direction covers all the catastrophic ones. The
  motivating shape does not occur either: 100% of XCH/BYC's 377 XCH of ask
  depth is >50% off the anchor, and the "junk top" is 2 XCH — 20× the dust
  threshold, not dust.
- **Tight is not conservative.** When the sigma floor exceeds the bound the
  innermost tier is already outside it, so every tier is: a 2% passive bound
  withdraws XCH/BYC on 87.7% of blocks, while on XCH/DBX anything ≥10% never
  binds. The feature is inert or fatal with little in between, which is why the
  caps are documented with the SEC's own comparators (8% for a blue-chip,
  20% near the open and close, 28–30% for less liquid names) rather than left
  to intuition.

### Retention would have deleted half the history it was asked to keep

Retention had not run since 2026-05-16, so raw history reached back to
2026-04-03 while the default window was 120 days. The next run — no flag
typed differently, nothing to review — would have deleted **91,244 of
207,787** `snapshots` rows (43.9%) and **363,374** `strategy_quotes` rows
(29.0%), including all of April: the densest month of the very BYC book
history the new diagnostics read.

- **The hazard was never the retention number.** A long gap between runs
  silently converts a routine window into a bulk deletion, and the loss grows
  exactly while nobody is watching. So the guard is proportional rather than
  a bigger constant: a run that would delete more than 25% of a raw table
  refuses, prints per-breached-table counts, percentages and each table's own
  oldest surviving row, and exits 3 without touching the database. A steady
  daily run removes a day at a time and never approaches the bound.
- Rows are **counted before deleting**. `cur.rowcount` reports the damage
  after it is done, which is no use to a guard, and the refusal has to happen
  before any `DELETE` so the rollback is empty rather than merely correct.
  The refusal rolls back explicitly — the rollup UPSERTs already ran in the
  same transaction and are the half that could otherwise survive.
- Two deliberate escapes: widen `--raw-retention-days` to keep the history,
  or `--confirm-large-prune` to delete it on purpose.
- **`--backup` was unsafe in exactly the situation it exists for.** It used
  `shutil.copy2` on a database running in WAL mode against a live engine,
  copying only `xop_trader.db` and leaving the `-wal` file behind — 15 MB of
  committed-but-uncheckpointed pages at the time of the change. The backup
  taken before a destructive operation would have been missing the most
  recent writes and torn besides. Now uses SQLite's own `conn.backup()`,
  which holds a read transaction and sees one consistent snapshot including
  the WAL. Verified: `integrity_check ok`, full row counts.
- **The prune reached up to a day past its own window.** The cutoff was
  formatted `...THH:MM:SS.ffffffZ` while both tables store
  `YYYY-MM-DD HH:MM:SS`, and both comparisons are text: `" "` (0x20) sorts
  below `"T"` (0x54), so every row sharing the cutoff's date compared
  less-than whatever its time. A row stored `2026-05-04 23:59:59` was deleted
  by a cutoff of `2026-05-04T11:56:08Z`. The count and the DELETE shared the
  format, so the guard's percentages were honest — this was over-deletion,
  not mis-reporting — but it always erred toward data loss, so it is fixed
  here rather than left standing next to a guard about data loss.

Exercised against a **copy** of the live database, never the original.

### Permuto: the contest account was liquidated, and four PRs answering why

The contest account reached **equity 0** on 2026-09-01 — realized PnL
-1,218,420, total PnL floored at the full -500,000 starting allocation,
depth frozen at 4,093.892 against a 300,000,000 eligibility gate. Three of
41 market makers were in that state while median MM equity was still
exactly 500,000, so this was not a venue-wide reset. `risk.assess()`
returns FLATTEN on zero equity, so no configuration change can restart
quoting without the account being re-funded.

The last observable state before the log stopped was the venue refusing
every batch: *"Carried-session stress margin: need 5,394,844 USDC to
survive 8x index move (available 591,782)"* — the carried short needed
~9x the cash on hand. The oracle then gapped +73% to +229% at the open.

**#135 — a portfolio exposure budget.** `max_position_usd` was PER MARKET
with nothing aggregating it, so three markets at the shipped 250,000
authorised 750,000 of exposure on a 500,000 account — 1.5x equity before
the venue's 8x carried multiplier, with every individual market perfectly
inside its own limit. `portfolio_cap_usd()` reduces each market's cap by
what the rest of the book already holds, denominated in equity because
that is what the venue liquidates against. Risk-increasing legs are
clamped to remaining headroom; reducing legs never are, or the book is
trapped at the moment it is trying to get back inside. A market pinned
one-sided — which earns exactly zero, since credit is `min(bid, ask)` —
now announces itself once on entry and once on recovery, instead of
hiding in a per-tick line that repeats all night while the tick still
reports `quote`.

**#133 — stage-aware quoting profiles.** A mode now carries a posture,
not just a position cap: CLOSED quotes full size against the frozen
overnight oracle (the cheapest depth of the week), SESSION quotes wide
and small because the measured median 1-minute move is 20-24%, and RAMP
and EXIT aim at **flat** rather than merely smaller — inventory carried
into the close is a claim on the overnight window. Adds per-market
stale-oracle withdrawal, so a neighbour that stops printing is no longer
quoted against its own stale price while the aggregate detector stays
happy.

**#132 — an operator control that can actually close a position.** Three
buttons (25/50/100%) behind a confirmation showing market, side,
contracts and notional per leg. Every order `reduce_only` and IOC, sized
against a fresh venue read, with the resting book cancelled first — an
old non-reduce-only quote filling afterwards would otherwise undo the
close. Reports refusals, partial fills and **unresolved** outcomes
distinctly, because an order whose answer never arrived may have executed
and must not read as "nothing happened".

**#131 — anti-cross backoff.** The learned retreat is now capped against
current headroom *and* the re-quote trigger, so a fully skewed leg is not
born past its own trigger and replaced every tick.

## [0.10.12] — 2026-08-31 — the Permuto inventory curfew

Stops us carrying inventory across a market close, which is the trade that
beat market makers in the previous competition: buy long while the
underlying is shut, collect at the reopen when short MMs are liquidated and
ADL hands the longs their exit.

- **The venue does not pause overnight.** Measured live with US equities
  closed: `trading_paused` is `false` and every market reads `active` while
  all three oracles sit frozen to sixteen digits. It keeps matching orders
  against a stale price, and `/info/meta` has no next-close field at all —
  `paused_at` / `pause_resume_at` populate only once already paused. So the
  curfew runs on a written-down UTC session table (no tzdata on Windows,
  and the bundle is lock-pinned), checked against an oracle-freeze detector
  that is ground truth and cannot expire. They combine asymmetrically:
  tighten if either says so, relax only if both agree.
- **Inventory is capped on a clock**, feeding the `max_position` argument
  `risk.assess()` already takes, so an oversized position becomes
  maker-side REDUCE_ONLY quotes through existing, tested machinery. The cap
  ramps down over the 90 minutes before each close, so inventory is worked
  off by resting orders that **earn** the spread rather than dumped through
  it at the bell. Nothing crosses the spread.
- **Overnight the sides are not symmetric**: no new short exposure at all,
  a bounded long (25% of the configured cap) still permitted. The oracle is
  a 60-second trailing realized-vol estimate, so it freezes on a calm
  end-of-day window while the first print after the reopen comes from the
  most violent minute of the day — the reopening print is systematically
  higher, which is why a carried short is the position that gets
  liquidated. Defensive, not a carry bet: the bid keeps quoting.
- **Settling reopens the ask.** The 15 minutes after an open hold the
  reduced size but quote both sides — the oracle is live by then, so the
  stale-price rationale is spent and closing a side would forfeit depth
  credit (`min(bid, ask)`) through the busiest quarter-hour.
- **An on/off switch** in Settings → Startup, defaulting **on**, read fresh
  each time a session is armed. Ticks now report the curfew stage, and the
  status bar names it, so bid-only overnight quoting is legible as intended
  rather than broken.

Hardened by a 22-agent adversarial review that confirmed 15 findings, four
of them blockers reproduced by execution — the leg-permission check was
size-blind (one contract of long inventory waved through a full-size ask,
which if filled left us massively short), leg sizes were never clamped to
the cap at all, the veto never touched a book already resting, and the long
cap doubled at the ramp/exit boundary. All fixed and regression-tested.

## [0.10.11] — 2026-08-30 — startup intent on the switch; declared pegs anchor fair value

- **Dexie switch paints stored startup intent** [STARTINTENT]: with
  Settings > Startup set to ON, the slider shows ON from the first tick
  and a new "starting" chip narrates the boot (pre-launch, Analyzing,
  gates-unpublished) instead of "the engine is not running". The claim is
  honest: it expires after 90s without an engine, a failed launch retires
  it, real gates outrank it, and a pre-sync click supersedes the stored
  request.
- **Declared pegs anchor fair value** [PARANCHOR]: pegged_assets now
  feeds the fair-value graph as FALLBACK anchors — consulted only when a
  pair's solve finds no path without them, so market evidence always
  outranks a declaration. Universal over currencies: EUR/JPY pegs convert
  through an FX cross fetched in the same CoinGecko request. Guards from
  adversarial review: sibling consensus on conflated legs (wUSDC.b/wUSDC
  are one node), suspension gating, CoinGecko freshness gate, escaped +
  validated peg currencies, and a per-pairing sigma-ceiling warning
  (`fair_value_par_sigma_bps` 100 / `fair_value_par_market_sigma_bps`
  140). Retires the QUOTING BLIND widening on XCH/BYC.

## [0.10.10] — 2026-08-30 — always-offer posture; the no-loss floor becomes an operator dial

*(Entries 0.8.1–0.10.9 were not recorded here as they shipped; the git
tags and release notes on GitHub are authoritative for that span.)*

- **`strategy.no_loss_floor_mode: strict | aging | off`** replaces the
  unconditional never-sell-below-basis ask floor. `aging` finally
  threads `inventory_aging.max_loss_relax_bps` into the floor (it was
  provably a no-op); `off` — the current setting, an explicit operator
  decision during the 2026-08-30 XCH repricing — cedes ask pricing to
  the market at both no-loss sites (Step 7 lift and Step 4
  `set_cost_basis`).
- **Side-aware, configurable BBO sanity**: aggressive deviation (would
  execute dislocated) keeps 10%; passive deviation (merely rests far
  from a thin book) allows 30% with a dual bid safe-harbor; the model-
  mid check gets its own 50% threshold.
- Two-sided quoting restored on XCH/BYC and XCH/DBX
  (`ratio_band_enter_by_pair` 0.30), anchor engages across the full
  dislocation (`competitive_anchor_max_distance_bps` 8000), post-restart
  warmup cut to ~minutes (`comp_pid_warmup_blocks` 5).

## [0.8.0] — 2026-08-08 — P&L overhaul + warp bridge (requires an engine and GUI restart to take effect)

Root-cause fix set for "P&L never worked".  A multi-agent audit against the
live DB (774 fills, Apr 3–Jul 30) found realized P&L was recorded for only
14 of 774 fills; every layer of the pipeline had an independent fault.

### Fixed

- **Cost-basis int64 overflow (the core bug)** (`cpp/src/risk/inventory.cpp`):
  `total_cost = fill_price × qty` (~1e24 for XCH fills) was stored into an
  int64; MSVC's out-of-range double→int64 cast saturated to INT64_MIN and the
  basis clamped to 0 on the *first* XCH bid fill after every restart.  Every
  later ask then hit the `basis <= 1` sentinel guard → `realized_pnl = 0`.
  `AssetRecord::total_cost` is now a `double` end-to-end.
- **Cost basis now survives restarts** (TODO #9): new `inventory_state` table
  (`Database::save/load_inventory_state`), persisted after every fill/seed/
  reconcile and restored at startup before wallet seeding.  Sentinel-seeded
  assets are upgraded to a market mark ("mark at first observation") on the
  first heartbeat with live mids (Step 11), and a one-shot wallet reconcile
  classifies downtime deposits/withdrawals (`InventoryTracker::adjust_quantity`).
- **PnLTracker rehydration**: `init_database()` now rebuilds per-pair
  spread/fee/fill-count/gross accumulators from `trade_log`, so cumulative
  P&L no longer resets to zero on every restart (snapshots proved it had
  re-accumulated from 0 after each of ~20 restarts).
- **Cross-currency basis blending**: one shared XCH basis was fed pseudo-
  prices from wUSDC.b-, BYC- and DBX-quoted pairs (~1.2e12 vs ~1.4e14 scales).
  Basis is now stored in USD-normalized pseudo-units, converted per pair via
  `Engine::quote_usd_factor` (wUSDC*/USDS/BYC = $1/unit, DBX cross-derived);
  `trade_log.cost_basis_mojos` keeps the pair's own quote units (converted on
  write).
- **Unrealized P&L was hardwired to zero**: `mark_to_market` looked up
  balances/basis by display symbol ("XCH") while stores are keyed by
  canonical asset id ("xch"/64-hex); every lookup missed.  It now resolves
  the canonical id from the registered pair conversion.
- **Fees silently 0 since June 2026**: the engine read the offer-creation fee
  from `State` *after* `detect_fills` had removed the offer.  The fee now
  travels on the `Fill` itself, captured at detection time.
- **USD totals were unit soup**: quote-CAT mojos (1e3/unit) summed with XCH-
  mojo fees (1e12/unit) then divided by 1e12 × hardcoded 2.70 — a $1 profit
  rendered as ~$0.00.  `PnLTracker` now aggregates USD per pair via
  registered conversions; new Prometheus gauge `xop_pnl_usd`; the XCH/USD
  rate comes from a live stable-quoted mid instead of the 2.70 constant.
- **GUI Reports page crashed on every refresh since 2026-04-21**:
  `fetch_reports` died with `UnboundLocalError` (`fee_usdc_expr` assigned
  from itself in the v0.7.46 refactor) before running any SQL.  Restored a
  real definition; status bar and dashboard cards now use the engine's USD
  gauge / per-quote conversions instead of dividing quote mojos by 1e12.
- **Fill durability**: fill processing is journal-first (trade_log insert
  before offer-status/inventory side effects) with idempotent duplicate
  handling on `trade_id`, and both SQLite connections set
  `PRAGMA busy_timeout=5000` — previously a transient lock during fill
  processing lost the audit row forever.
- **Per-pair snapshots**: `snapshots.pnl_total_mojos` stored the *global*
  total on every pair's row and `xch_usd_rate`/`pnl_total_usd` were never
  populated (0 in all 136k rows).  Now per-pair values + live rate.
- **Stale tax export**: `export_trades_csv` still used the pre-v0.7.45
  formula (missed by the v0.7.46 centralization); now routed through
  `quote_mojos_for`.
- **Uncommitted `detect_fills` guard**: `quote != "xch" || base != "xch"`
  was a tautology; per-leg guard now as intended.

Found by adversarial review of the fixes above, before deployment:

- **Second int64 overflow, in `State::Position`** (`cpp/src/state.cpp`): the
  identical `qty × price ≈ 1.3e24` overflow made `Position::add` **reject
  every XCH buy** ("[Position] Overflow in cost basis -- addition rejected",
  20 occurrences in the live log on 2026-07-29 alone), so State's XCH balance
  could only ever decrease.  Latent until the keying fix above made
  mark-to-market depend on that balance.  `total_cost` is now a `double`.
- **Unpriced fills no longer fabricate a basis**: when a pair has no USD
  valuation the engine used to pass a placeholder price of 1 to `record_buy`,
  whose sentinel branch re-marks the *entire* holding at that price **and
  clears the sentinel flag** — permanently destroying the basis with no way
  for the mark-at-first-observation upgrade to repair it.  New
  `InventoryTracker::record_fill_unpriced` tracks quantity and leaves the
  basis and its repairability intact (and flags unknown-cost quantity as a
  sentinel so it *is* repaired later).
- **`usd_per_xch` no longer falls back to the hard-coded 2.70** — XCH has
  traded near $1.35, so that constant valued fills at ~2× and, now that basis
  is persisted, would have baked the error in permanently.  It reports 0
  ("unknown") instead, which routes fills to the unpriced path.
- **Shared base asset marked once**: XCH is the base of three enabled pairs,
  and `mark_to_market` loops per pair while balances are per asset — the
  whole XCH holding was valued three times and summed.
- **BYC no longer assumed to be exactly $1.00**: it is a CDP stablecoin that
  trades off peg (live `BYC/wUSDC.b` mid was $1.0554), and with one shared
  XCH basis that error contaminated every `XCH/BYC` fill.  The live cross is
  used when available.
- **Sentinel basis excluded from mark-to-market**: marking a position against
  the 1-mojo sentinel booked its entire market value as unrealized profit
  (~$209 of fiction on a 155 XCH position).
- **`query_pending_offers` column indices** (`cpp/src/database.cpp`): read
  `created_block` into `fee_mojos`, so restored offers carried a *block
  height* (~9e6) as their fee.  Pre-existing, but the fee fix above would
  have written those into `trade_log` for every downtime fill.
- **Wallet reconcile hardened**: tracked per asset instead of one global
  flag (a market maker reposting each block leaves XCH with coins in flight,
  so the single flag was consumed before XCH was ever reconciled), and
  balance snapshots are now block-stamped and rejected when stale.
- **Per-component USD gauges**: `xop_pnl_usd{component=total|realized|
  unrealized|fees}` replaces the GUI's guesswork — it had been dividing
  mixed-quote-currency mojos by 1e3, which overstates the DBX contribution
  ~74× and the fee leg ~1e9×.  Absent gauges render as "—" rather than a
  wrong number.
- **Strategy cost-basis units**: `set_cost_basis` was handed fixed-point
  pseudo-units while strategies compare against display-unit prices — a
  latent 1e12 mismatch (dormant only because the strategy-level no-loss
  constraint defaults off).
- **Buy rows no longer persist a meaningless cost basis**: they stored the
  *quote* asset's basis, which nothing ever updated and which is denominated
  differently from the row's own price.

### Added

- **Double-entry ledger + reconciliation control** (`ledger_entries` table;
  `docs/ACCOUNTING-POLICY.md`).  Every fill posts balanced legs (base, quote,
  fee) keyed idempotently on `(event_id, leg, asset_id)`, so a re-detected
  fill cannot double-post.  Opening balances come from the wallet at genesis
  — the ledger deliberately does **not** replay `trade_log`, which would
  import the ~665 XCH corruption it exists to detect.  Each heartbeat the
  per-asset sum is tied to the wallet's **confirmed** balance.

  The design is empirically grounded rather than assumed:
  - Ties to `confirmed_wallet_balance`, never `spendable` — posting an offer
    locks whole UTXOs, so spendable swings up to 100% of a wallet from the
    bot's own quoting while confirmed is unchanged.
  - Does **not** reuse `OnChainReconciler`'s `on_chain` figure, which is
    summed from `get_spendable_coins` and therefore structurally biased low
    (1,599 of 1,599 samples negative over 2.9 days, median −11.5 XCH,
    correlating +0.88 with open-offer count) and triple-counts the XCH wallet.
  - Tolerance is flow-based — live offer exposure + fee dust + a floor —
    because the CAT wallets are small enough that one heartbeat of fills moves
    20–75% of them, while 1% of the XCH wallet would hide a whole missed fill.
  - Escalation requires consecutive **same-signed** breaches: the ledger can
    only lag the wallet, so latency divergence self-heals and a real gap does
    not.  Backtested over 205 heartbeats it is silent on all 134 dust/fill
    intervals and fires on all 22 genuinely unexplained whole-XCH intervals.

  Alert-only by default (`accounting.pause_enabled: false`) because taker
  fills and external deposits still have no ledger event; auto-pausing would
  halt on legitimate activity.

- **Written accounting policy** (`docs/ACCOUNTING-POLICY.md`): chart of
  accounts, double-entry rules, the control account, tolerance derivation,
  cost-basis method (weighted average, IAS 2 / ASC 330), revenue recognition,
  fee treatment, and the unresolved FIFO-vs-weighted-average gap for US tax
  reporting.

- **Durable local trade history** in `data/trade_history/` (see its README):
  the engine appends every fill to `trades_live.csv`
  (`PnLTracker::append_history_csv`); `scripts/export_trade_history.py`
  regenerates `trades_full.csv` from the whole `trade_log` (dual price-era
  aware, fees joined from `offer_log`); all 774 historical fills exported.
- `cpp/tests/test_pnl_tracker.cpp`: first direct PnLTracker coverage
  (rehydration, duplicate idempotency, USD conversion, canonical-id keying,
  CSV mirror) plus XCH-scale overflow/persistence regression tests in
  `test_inventory.cpp`.
- **Warp bridge** (`gui/services/warp/`): automatic background bridging of
  USDC (Base) into wUSDC.b — the Chia CAT
  `fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d` — via
  [warp.green](https://warp.green). Fund an app-controlled Base hot wallet
  from anywhere (Coinbase, a Circle payout, any wallet); the GUI detects the
  deposit and runs the whole bridge unattended: approve + `bridgeToChia` on
  Base, wait for the validator attestation, collect the 6-of-10 Nostr BLS
  signatures, then build and push the Chia claim spend. Off by default
  (`warp.enabled: false`, `warp.auto_bridge: false`) — merging/upgrading
  changes nothing for the live bot until explicitly enabled and the GUI
  restarted. The hot-wallet key is DPAPI-encrypted at rest (Windows-only);
  every failure mode is stuck-not-lost (an attested message is claimable
  forever). `warp.dry_run` defaults to `true` and is a hard rehearsal gate —
  it signs both Base transactions without broadcasting, so the whole deposit
  leg (RPC, wallet daemon, receiver decode, live toll/tip, gas, signing) is
  exercised with no funds moved before an operator sets it to `false`. See
  `docs/warp-bridge.md` for the full setup, safety model, and recovery/sweep
  guide.

### Retired

- `scripts/backfill_pnl.py` and `scripts/backfill_pnl_units.py` now refuse to
  run (both corrupt `trade_log` if re-run; see their headers).
  `scripts/compute_actual_pnl.py` (cash-flow method) remains the ground-truth
  tool for pre-2026-07-30 history.

## [0.7.48] — 2026-04-25

### Added

- **Adaptive competitiveness-threshold PID controller** (`cpp/include/xop/strategy/competitiveness_pid.hpp`, wired into `cpp/src/engine.cpp` Step 5/8):
  - Companion to the existing spread PID. Where the spread PID tunes half-spread, this controller tunes the integer 0-10 *competitiveness gate* used in the Step 8 sanity filter.
  - Per-pair, integer-output, with anti-windup, configurable warm-up, and `[min_offset, max_offset]` clamp (defaults `[-3, +3]`).
  - Sign convention: underfilling (EMA fill rate below `comp_pid_target_fill_rate`) drives the offset NEGATIVE → effective gate lowers → more tiers post. Overfilling drives it POSITIVE → fewer, more aggressive tiers.
  - New config knobs in `strategy:` block: `comp_pid_enabled`, `comp_pid_target_fill_rate`, `comp_pid_kp/ki/kd`, `comp_pid_ema_alpha`, `comp_pid_integral_max`, `comp_pid_warmup_blocks`, `comp_pid_min_offset`, `comp_pid_max_offset`. All validated in `parse_strategy()` (`cpp/src/config.cpp`).
  - Defaults enabled in `config.yaml` with `target_fill_rate=0.05` (≈ 1 fill / 17 min @ 18.75 s blocks).
  - Six unit tests in `cpp/tests/test_competitiveness_pid.cpp`: warm-up zero-output, underfill→negative, overfill→positive, bounds clamping, on-target convergence, disabled-pinning, and reconfigure-resets.

- **Fill-history monitor script** (`scripts/monitor_fill_history.py`):
  - Rolling fill-rate report per pair across 1h / 6h / 24h / 7d windows, comparable to the PID input signal.
  - Shows fills, cancels, posted, average competitiveness, and the gap to `comp_pid_target_fill_rate`.
  - Supports one-shot, `--watch N` polling, and `--json` modes for cross-checking the controller's behaviour after a deploy.

## [0.7.47] — 2026-04-24

### Changed

- **Adverse-cancel thresholds widened so offers stay alive long enough to fill** (`cpp/include/xop/execution/offer_manager.hpp`):
  - `kSelectiveRefreshThreshold` 0.5% → **1.0%** (tier-scaled: tier 0=1.0%, tier 1=1.5%, tier 2=2.0%, tier 3=2.5%)
  - `kMinRefreshAgeBlocks` 6 → **12** (~10 min protection window before any adverse-cancel can fire)
  
  Diagnosed from live data on 0.7.46: in the 24h window of Apr 23–24, 49/50 `XCH/wUSDC.b` offers were cancelled with `price_adverse(1.26%–1.54%)` while `competitiveness_score` averaged **8.2/10** (i.e. we were already at or near the top of book). The cancels were chasing a strongly trending market (XCH +12% over three days), not protecting us from real adverse selection — every cycle burned cancel fees and posted into the new mid, only to be killed again.
  
  Net effect: makers now hold price during normal intraday volatility (up to ~1% mid drift on inner tier, ~2.5% on outer tier) and survive long enough for takers to actually hit them. Outer tiers were already designed to be stickier; this brings inner tiers into the same regime.

## [0.7.46] — 2026-04-21

### Added

- **Canonical `xop::quote_mojos_for()` helper** (`cpp/include/xop/types.hpp`): Single source of truth for the `quote_mojos = size * price * quote_denom / (base_denom * kMojosPerXch)` formula that was triplicated in `offer_manager.cpp::build_offer_dict`, `engine.cpp` realised-PnL, and `pnl.cpp::mark_to_market`. All three call sites now go through the helper, defending against the v0.7.45 1e9-inflation bug recurring after a refactor.
- **PnL-unit regression tests** (`cpp/tests/test_pnl_units.cpp`): Locks in the `quote_mojos_for` contract with XCH/wUSDC.b, CAT/CAT (BYC/wUSDC.b), and symmetric-denom round-trips plus defensive non-positive-denominator coverage.
- **Database schema documentation** (`db/schema.md`): Authoritative human-readable reference for `trade_log`, `offer_log`, `offer_closure_events`, `snapshots`, `strategy_quotes`, and `sanity_failures`. Documents the engine pseudo-unit price encoding and the per-pair quote-mojo PnL convention so future contributors do not re-introduce the v0.7.45 bug.
- **Stuck-offer Prometheus visibility** (`cpp/src/monitoring/metrics.cpp`): Added `xop_stuck_offers_peak` (max concurrent observed within the process lifetime) and `xop_stuck_offers_total` (lifetime counter, suitable for `rate()[1h]`). The pre-existing `xop_stuck_offers` instantaneous gauge is unchanged.

### Fixed

- **Cost-basis seed sentinel collapses correctly on first real fill** (`cpp/src/risk/inventory.cpp`, `cpp/include/xop/risk/inventory.hpp`): `seed_position()` now marks the bootstrapped record with a `basis_is_seed_sentinel` flag when the synthetic `Mojo{1}` placeholder is used. The next `record_buy()` after a sentinel **replaces** the basis with the observed fill price instead of weighted-averaging it. Without this, a large seeded inventory diluted incoming real fills back to a near-zero basis, which then drove the realized-PnL guard (`tr.cost_basis_mojos <= 1`) to suppress every subsequent ASK PnL forever.
- **BYC quoting unblocked** (`cpp/src/engine.cpp`): The fixed competitiveness threshold of `3/10` was rejecting 100% of BYC/wUSDC.b tiers (148 of 151 sanity rejections in the 0.7.45 audit) because stable-stable pairs trade in a sub-1% range and never score 3+. Lowered to `1/10` for pairs flagged `is_stablecoin: true`. Non-stablecoin pairs keep the original threshold.

### Changed

- **`kMinRefreshAgeBlocks` raised 3 → 6** (`cpp/include/xop/execution/offer_manager.hpp`): A quiet block series in 0.7.45 produced 41 of 44 cancels with reason `price_adverse`, indicating the cancel-recreate cycle was firing faster than the round-trip fee could amortize. Doubling the minimum protection window damps the churn while still allowing crossed-mid offers to be cancelled urgently.

### Repository hygiene

- Moved `_patch_*.py` one-shot migration scripts from the repo root into `scripts/migrations/` to declutter root and signal that they are legacy.

## [0.7.45] — 2026-04-20

### Fixed

- **PnL unit dimensional bug** (engine.cpp, monitoring/pnl.cpp, monitoring/pnl.hpp): Realized fill PnL and `mark_to_market` inventory PnL formulas were missing the `quote_denom / base_denom` factor that `offer_manager.cpp::build_offer_dict` uses everywhere else. For XCH/wUSDC.b that omitted factor is `1e3 / 1e12 = 1e-9`, which caused realized PnL values to be inflated by roughly 1e9. The engine path now matches the canonical formula `pnl_quote_mojos = (price - cost_basis) * size * quote_denom / (base_denom * kMojosPerXch)`. `mark_to_market` gained an optional `get_pair_unit_factor` callback so per-pair denominator ratios can be supplied without changing existing callers.

- **GUI now reports realized PnL in USDC dollars per pair** (gui/services/database_service.py): `fetch_reports` previously divided `realized_pnl_mojos` by `1e12` and multiplied by the XCH/USD rate, which compounded the engine bug and produced display values in the billions. Replaced the SQL aggregations with per-pair USD conversions: stablecoin quotes (wUSDC, wUSDC.b, BYC, USDS) divide by `1e3`; non-stable quotes contribute `0` until a USD rate is wired in. Fee aggregation continues to use the XCH-mojo path.

- **Historical trade_log backfill** (scripts/backfill_pnl_units.py): Recomputes `trade_log.realized_pnl_mojos` for legacy rows using the corrected denom math, preserving the cost-basis sentinel rule. Writes a timestamped backup of `data/xop_trader.db` before mutating any rows.

### Added

- **Actual P&L reconstruction script** (scripts/compute_actual_pnl.py): Stand-alone read-only tool that bypasses the unusable `realized_pnl_mojos` column (every historical fill had a sentineled cost basis) and reports cash-flow PnL plus an XCH inventory mark in USDC, auto-detecting the legacy-vs-current `price_mojos` encoding.

## [0.7.44] — 2026-04-15

### Fixed

- **Confirmed sell accounting now bypasses no-loss enforcement** (engine.cpp, inventory.cpp, inventory.hpp): `InventoryTracker::record_sell()` was rejecting already-confirmed on-chain sells when the tracked cost basis was above the fill price. That left XCH holdings overstated after real sells, keeping `inventory_ratio()` artificially neutral and letting the balance equalizer continue quoting as if inventory were still balanced. Added an explicit `enforce_no_loss` control so post-fill accounting always reflects confirmed chain state.

- **Wallet-balance inventory recovery after startup seed failures** (engine.cpp): When wallet balance RPC calls timed out during startup seeding, both `InventoryTracker` and `State` could remain empty for live assets. The engine then reported `inventory_ratio = 0.5` across XCH pairs even while the wallet was materially imbalanced. Startup seeding now prefers `confirmed_wallet_balance`, and Step 8 backfills empty tracked positions from the live wallet cache when confirmed balances are available.

- **Regression coverage for confirmed below-basis sells** (test_inventory.cpp): Added a unit test proving confirmed sells must still decrement inventory when the no-loss rule would block a pre-trade order.

## [0.7.43] — 2026-04-12

### Added

- **Sanity failure audit log** (database.hpp, database.cpp): Added `sanity_failures` table and `DbSanityFailure` record path to persist every rejected quote from sanity filters. Captures block, pair, side, tier, proposed price, reference price, deviation, reason, and free-form details for post-mortem analysis.

### Fixed

- **Microprice divergence clamp** (market_data.cpp): Added a 10% clamp that snaps depth-weighted orderbook microprice back to simple BBO midpoint when the order book is highly asymmetric and microprice drifts too far from executable market reality.

- **Pre-post BBO sanity suppression** (engine.cpp): Added Step 8 sanity guards to suppress quote tiers that deviate excessively from current BBO references, reducing irrational quote placement during transient book distortions.

## [0.7.42] — 2026-04-12

### Fixed

- **Dust filter denomination awareness** (market_data.hpp, market_data.cpp, engine.cpp): The competing-offer dust filter used `base_mojos_per_unit` (1e12 for XCH) for ALL offers regardless of side. Bid-side offers are denominated in the quote asset (e.g. wUSDC.b at 1e3 mojos/unit), so their sizes of ~2,280 mojos were well below the 1e12 effective minimum and were silently discarded as dust. Added a `quote_mojos_per_unit` parameter and made the dust filter side-aware: bids use `quote_mojos_per_unit`, asks use `base_mojos_per_unit`.

- **Anchor distance uses abs() and BBO reference** (liquidity.cpp): The anchor distance check computed `(best_comp - mid) / mid` without absolute value, producing a negative result when the model mid drifted past the best competing ask. This caused the ask-side anchor to never fire. Additionally, the safety check (`new_price >= mid` for asks) blocked anchoring when mid > best_ask. Now uses `std::abs()` for distance and a BBO midpoint reference (`(best_bid + best_ask) / 2`) for safety bounds instead of the model mid.

- **Bidirectional staleness for anchor repricing** (offer_manager.hpp, offer_manager.cpp, engine.cpp): The staleness classifier only triggered on adverse deviations (new optimal price worse than pending). When competitive anchor pricing moved offers in the "favorable" direction (e.g. bid UP toward BBO), the deviation was positive and classified as Fresh, preventing the old deep-in-book offers from being replaced. Added an anchor-active override: when `competitive_anchor_enabled`, any price deviation exceeding the tier threshold triggers Stale regardless of direction.

### Added

- **Competitive anchor test coverage** (test_liquidity.cpp): 7 new tests covering both-sides anchoring, mid-above-best-ask scenario (the specific bug), distance-too-far fallback, one-sided competition, tier stride ordering, and dust filter denomination awareness (correct and legacy behavior).

## [0.7.41] — 2026-04-12

### Added

- **Competitive anchor pricing** (liquidity.cpp, engine.cpp, config.cpp): New pricing mode that anchors Tier 0 to the best competing offer on each side (bid/ask) and strides outward for subsequent tiers, ensuring top-of-book placement. When enabled, the engine places bids 1 tick above the best competing bid and asks 1 tick below the best competing ask, with configurable inter-tier stride (default 65 bps). Falls back to mid-based spacing when no competing offers exist or the anchor exceeds `competitive_anchor_max_distance_bps` (default 500 bps) from mid. Mutually exclusive with gap-aware spacing (anchor takes priority). Configurable via `strategy.competitive_anchor_enabled`, `strategy.competitive_anchor_max_distance_bps`, and `strategy.competitive_anchor_stride_bps`.

## [0.7.40] — 2026-04-12

### Added

- **Order-book-derived mid-price (depth-weighted VWAP micro-price)**: Instead of relying solely on the Dexie ticker BBO midpoint, the market data aggregator now computes a depth-weighted VWAP micro-price from the top N levels of dust-filtered competing offers. The micro-price weights each side's VWAP by the opposite side's total depth (Stoikov 2018), giving the engine its own independent fair-price estimate. This replaces the simple `(best_bid + best_ask) / 2` as the DEX component of the 70/30 blend with CEX. Configurable via `market_data.orderbook_mid_enabled` (default: true) and `market_data.orderbook_mid_depth` (default: 5 levels per side).

### Fixed

- **LiquidityConfig phi never wired from config** (engine.cpp): The `phi` (inventory skew strength) field in `LiquidityConfig` defaulted to 1.5 in the struct definition but was never set from the parsed `config.yaml` value (0.8). This caused `bid_mult = 1 + 1.5 × deviation` instead of `1 + 0.8 × deviation`, pushing all bid tiers ~40% further from mid than intended. Now wired as `liq_cfg.phi = config_.strategy.phi`.

- **Thread safety in compute_mid()** (market_data.cpp): Replaced two unsynchronised accesses to `config_.amm_freshness_threshold_sec` and `config_.cex_freshness_threshold_sec` with a properly locked config snapshot, consistent with the `[MEDIUM-1]` pattern used elsewhere.

- **Gap-aware spacing depth scaling** (liquidity.cpp): The gap_blend_factor is now scaled by order book depth (linear ramp from 0 at 0 competing offers to 1.0 at 10 offers). On thin books (e.g. only 2 competing offers), the gap analysis was pulling ALL tiers far from mid to fill desert gaps, inflating spacing from [35, 100, 200, 300] to [464, 490, 530, 570] bps. With depth scaling, a 2-offer book yields effective blend of 0.12 instead of 0.60, keeping tiers close to their configured baseline.

## [0.7.39] — 2026-04-12

### Changed

- **Tier sizing rebalance: q_max 8→30, num_tiers 6→4** (config.yaml, config.example.yaml): Deep analysis of five option combinations (pure q_max, huge q_max, reduced tiers, combined, flat decay). Selected Option C for best long-term balance of bid size, inventory sensitivity, and fill quality. Outermost bid tier now ~12 XCH (was 1.7), tightest tier ~4.5 XCH (was 0.3). Inventory signal preserved at 6% (meaningful rebalancing as XCH recovers). Ask side remains safe via v0.7.38 wallet cap.

- **Updated tier_spacing_bps and tier_size_pct for 4-tier ladder**: [35, 100, 200, 300] bps spacing with [0.15, 0.20, 0.30, 0.35] allocation. Fewer, larger tiers mean each fill buys more XCH and saves on-chain fees.

- **BYC/wUSDC.b stablecoin overrides reduced from 6 to 4 tiers**: [3, 8, 20, 40] bps spacing with [0.30, 0.25, 0.25, 0.20] allocation.

## [0.7.38] — 2026-04-12

### Fixed

- **Phantom liquidity: Avellaneda ask pool vastly exceeded wallet balance** (engine.cpp, config.yaml): The Avellaneda-Stoikov formula `ask_size = q_max × (1 + q/q_max)` with `q_max=100` computed an ask pool of ~106 XCH when the wallet only held ~10 XCH. Each inner-tier offer (~69 XCH) exceeded total balance, enabling takers to drain XCH far faster than bid-side could replenish. Reduced `q_max` from 100 to 3 (matching actual per-pair tradeable balance).

- **No wallet-balance cap in sizing pipeline** (engine.cpp, engine.hpp): The Avellaneda raw quote flowed through risk limits and ladder generation with no check against the actual confirmed XCH balance. Added `xch_confirmed_balance_` member queried via `wallet_->get_wallet_balance(1)` each heartbeat before Step 7, then hard-capped `avail_inventory` and `avail_capital` against it in `step_generate_ladder`. Prevents the model from ever offering more XCH than exists.

- **XCH exempted from spendable reserve gate** (engine.cpp): Step 8 Gate 2 (`if (confirmed > 0 && sb.wid != 1)`) skipped wallet_id=1 (XCH), allowing sell-side offers to continue posting even when spendable-to-confirmed ratio was dangerously low. Removed the `wid != 1` exemption — the new Step 7 wallet-balance cap prevents the deadlock scenario the exemption was designed to avoid.

## [0.7.37] — 2026-04-11

### Added

- **Sigma floor for GLFT formula** (config.hpp, engine.cpp): When the Yang-Zhang volatility estimator returns zero (flat market with no price movement), the GLFT raw half-spread degenerates to `(1/κ)ln(1+κ/γ)` and the volatility-driven position-sizing term vanishes entirely. Added `sigma_floor` config parameter (default 0.001) applied in Steps 4 and 6, keeping the formula well-behaved and producing meaningful inventory-aware quotes even in flat markets.

- **Size-based staleness detection** (offer_manager.cpp): The selective-refresh classification previously only checked price deviation — existing offers whose sizes became a mismatch after the market allocator reshuffled capital fractions would stay "Fresh" indefinitely, preventing right-sizing. Now checks if a pending offer's size exceeds 2× the new optimal size (`kSizeStaleThreshold = 2.0`) and marks it Stale, triggering a cancel+repost with correct allocation-scaled sizing.

### Changed

- **Lowered `min_trading_units`** (config.yaml): 0.5 → 0.1 XCH. The previous threshold blocked ask-side offers whenever XCH spendable dropped below 0.5 (common after pending offers lock XCH), suppressing half the order book. At 0.1, the engine can post ask-side offers with as little as 0.1 XCH spendable.

## [0.7.36] — 2026-04-10

### Fixed

- **Risk limits mark-to-XCH always falls back to raw mojos** (limits.cpp): `mark_to_xch()` constructed asset-ID-based lookup keys (`"<hex>/xch"`) but `State::markets_` stores snapshots by pair name (`"XCH/BYC"`), so every lookup failed and returned raw mojo balances — making 1 CAT mojo (0.001 CAT) appear equal to 1 XCH mojo (10⁻¹² XCH). Replaced broken market snapshot probes with pre-computed XCH exchange rates stored in State.

- **State positions never seeded from wallet** (engine.cpp): `State::positions_` was only populated from detected fills in `offer_manager.cpp`, not from wallet balances at startup. Risk checks saw empty/partial position data, causing wildly incorrect concentration calculations (0.5 default or 100% for the single-fill asset). Added `state_->record_buy()` alongside `inventory_->seed_position()` in the engine seeding block.

- **XCH rate computation for risk system** (engine.cpp, state.hpp, state.cpp): Added per-heartbeat XCH rate computation at end of Step 1 — for each enabled pair with XCH on one side, computes `kMojosPerXch / (mid_price × quote_mojos_per_unit)` and stores via `State::set_asset_xch_rate()`. Added `register_pair_asset_keys()` for asset-ID to pair-name secondary index in State.

### Changed

- **Raised `max_capital_per_pair_pct`** (config.yaml): 40% → 85%. With wUSDC.b at 77% of portfolio, the XCH/wUSDC.b pair was permanently blocked at 40%, preventing the system from selling wUSDC.b for XCH to rebalance.

- **Reduced coin pool targets** (config.yaml): `coin_pool_target_count` 12 → 3, `coin_pool_target_xch` 2.0 → 0.5 XCH. Previous targets required 24 XCH but only 2.04 XCH was available.

- **Tuned GLFT skew for aggressive rebalancing** (config.yaml): `phi` 0.5 → 0.8 (stronger single-pair inventory skew) and `cross_pair_skew_phi` 0.30 → 0.50 (stronger cross-pair rebalancing coordination). Portfolio was heavily skewed toward wUSDC.b; increased skew parameters allow the GLFT model to rebalance more aggressively.

## [0.7.35] — 2026-04-10

### Fixed

- **Half-spread explosion destroys bids** (avellaneda.cpp, glft.cpp): The A-S/GLFT formula `(1/κ)·ln(1+κ/γ)` produces an absolute spread (~3.81 price units with γ=0.005, κ=1.5) that exceeds the mid price for any pair under ~$7.60, flooring bids to zero. Added a 49% mid-price cap on delta BEFORE the regime multiplier so bids stay positive and regime differentiation is preserved.

- **Gate 2 XCH deadlock** (engine.cpp): XCH spendable-reserve ratio (spendable/confirmed) was artificially low (~9%) because 91% of XCH was locked in the trader's own pending offers. Gate 2 suppressed the ask side, preventing offers from cycling. Exempted XCH (wallet_id 1) from Gate 2 since the offer_manager's UTXO-lock pre-check already guards XCH spendable.

- **Dust filter drops all tiers** (config.hpp, config.yaml): `min_offer_size_units` defaulted to 1.0 XCH, but with ~27 XCH split across 4 pairs × 6 tiers × 2 sides, each tier was ~0.33 XCH — all dropped as dust. Lowered default to 0.1.

## [0.7.34] — 2026-04-10

### Fixed

- **Spread explosion from annual→daily sigma mismatch** (engine.cpp): `compute_spread()` was receiving annual sigma (~1.2) instead of daily sigma (~0.07), inflating the Avellaneda–Stoikov optimal spread by ~16×. Added `sigma / std::sqrt(365.0)` conversion before passing to the spread calculator.

- **Unclamped inventory skew multiplier** (spread.cpp): `skew_frac` (inventory fraction) was unbounded, allowing extreme inventory imbalance to produce multi-hundred-percent spread skew. Clamped to [−1, +1].

- **`fee_reserve_xch: 1.0` permanent buy-only mode** (engine.cpp): A 1.0 XCH fee reserve consumed the entire XCH balance, suppressing all asks. The reserve is now reasonable relative to actual fee costs.

- **Backtest sigma scale mismatch** (backtest.cpp): Backtest engine was using raw annualized sigma instead of converting to the per-step scale, causing backtested spreads to diverge from live behavior.

- **MarketAllocator surplus redistribution** (market_allocator.cpp): When all pairs were frozen at their min/max bounds, surplus XCH was not redistributed, leaving capital idle. Fixed redistribution logic to handle the all-frozen edge case.

### Changed

- **BYC/wUSDC.b peg target corrected** (config): `peg_target` changed from 0.985 to 1.000 (BYC is a dollar-pegged stablecoin, not a 98.5¢ token). `min_profit_margin_bps_override` raised from 5 to 25 to capture monopolist spread.

### Added

- **§3.22 Pegged-Asset / Stablecoin Market Making** (docs/trading-strategies.md): New strategy section documenting stablecoin MM theory, lessons learned from peg misconfiguration, and future work. Added 6 scholarly references (55–60) including Bergault & Guéant (2024), Teeple (2023), and Bellia et al. (2025).

## [0.7.33] — 2026-04-09

### Fixed

- **Depleted-side offer cancellation** (engine.cpp Step 8): When an asset's balance drops below the reserve threshold, existing offers that sell that asset are now cancelled immediately — even if they are Fresh.  Previously, the anti-churn logic kept all fresh offers alive when both sides were suppressed, reasoning that they would expire naturally via TTL.  However, counterparties could fill those asks before expiry, draining the asset to zero (BYC went from 0.878 → 0.000 while the engine logged "keeping live to prevent churn").  The fix introduces `base_depleted` / `quote_depleted` tracking at Gate 2 (reserve-ratio) and Gate 3 (min-balance) suppression points, then:
  - **Both sides suppressed**: cancels fresh offers on the depleted side (asks for depleted base, bids for depleted quote) while preserving anti-churn behavior for the non-depleted side.
  - **Single side suppressed**: cancels existing offers on the depleted side before the early-continue, allowing the active side to repost normally.
  - XCH UTXO-lock anti-churn is preserved when neither side is truly depleted (the scenario it was designed for).

## [0.7.31] — 2026-04-09

### Changed

- **Offer TTL increased** (`offer_ttl_blocks` 180 → 600): Reduces cancel/repost churn from every ~90 min to every ~5 hours. Fewer blockchain transactions, lower fee spend, and less pending_change downtime between cycles.

- **Fee floor lowered** (`min_fee_mojos` 50000 → 5000): Previous 50K floor was 20× the mempool estimate. 5K is still ~2× the current mempool rate, saving ~45K mojos per on-chain transaction.

- **CAT coin pool disabled** (`cat_coin_pool_target_count` 10 → 0): BYC and wUSDC.b have near-zero spendable balances (most locked in offers). Splitting was failing every cycle, wasting logs. Disabled until CAT inventory recovers.

- **Spendable reserve threshold lowered** (`min_spendable_reserve_pct` 0.25 → 0.10): The 25% spendable/confirmed ratio gate was permanently suppressing both sides of BYC/wUSDC.b (only 11.9% free). Lowered to 10% to allow quoting when most inventory is locked in offers.

### Added

- **CEX-DEX data wiring for ArbitrageDetector**: Step 9b now builds `DexieBookSnapshot` vectors from MarketDataFeed mid/spread and `CexPrice` vectors from CoinGecko-derived references, then calls `set_dex_snapshots()` + `set_cex_prices()` before `scan_all()`. The CEX-DEX and Cross-DEX scans that were previously blind (empty caches) now receive data and can detect divergences.

- **CEX-DEX confidence cap** (`cex_dex_confidence_cap`, default 0.25): Hard ceiling on confidence assigned to CEX-DEX arbitrage opportunities. CoinGecko prices are aggregated, delayed, and vulnerable to manipulation — capping at 0.25 (below the 0.40 min_confidence_threshold) means CEX-DEX signals are logged for visibility but not acted upon unless the operator explicitly lowers the confidence threshold. Prevents the engine from making aggressive trades based on potentially manipulated CEX data.

## [0.7.30] — 2026-04-08

### Added

- **Dry-powder arbitrage reserve** (`arb_reserve_coins`, default 2): Step 7's XCH UTXO headroom calculation now deducts N coins from the available budget, ensuring they remain unallocated and instantly available for opportunistic trades (crossed-book takes, peg arb). Prevents the tier ladder from locking every spendable coin.

- **Cancel-worst-to-free for arb** (`cancel_worst_to_free`, default true): Step 9c now checks XCH spendable before the crossed-book scan. If below 0.25 XCH and no free coins exist, the engine cancels the least competitive pending offer (highest tier, oldest) via `emergency_cancel` to liberate a UTXO, then proceeds with the arb take. Works in tandem with dry-powder reserve as a fallback when all reserved coins have been consumed.

## [0.7.29] — 2026-04-08

### Added

- **Universal coin pool management (XCH + CAT tokens)**: The existing `step_maintain_coin_pool()` was declared but only handled XCH (wallet 1). Rewrote to support all asset types. Phase 1 splits XCH coins as before. Phase 2 automatically discovers every unique CAT wallet referenced by enabled pairs (BYC, wUSDC.b, wmilliETH.b, etc.), resolves wallet IDs via `resolve_wallet_id()`, converts the target denomination using each asset's `mojos_per_unit`, and calls `ensure_split()` for each. New config fields `cat_coin_pool_target_count` (default 10) and `cat_coin_pool_target_units` (default 50.0) control CAT splitting independently from XCH. Each wallet's `pending_change` is checked before splitting to avoid overlapping transactions. Startup and heartbeat triggers updated to fire when either XCH or CAT pool is configured.

### Fixed

- **Coin pool heartbeat activation**: The heartbeat and startup coin pool triggers only checked `coin_pool_target_count > 0` (XCH). Updated both conditions to also trigger when `cat_coin_pool_target_count > 0`, ensuring CAT splitting runs even if XCH splitting is disabled.

## [0.7.28] — 2026-04-08

### Added

- **Dust-filtered mid-price from competing offers**: The Dexie ticker API reports top-of-book prices regardless of offer size, so a 5-mojo dust offer can set the "best bid" or "best ask" and poison the mid-price used by the Avellaneda-Stoikov model. After ingesting competing offers (already filtered by `min_competitor_offer_size`), the engine now recomputes BBO from only non-dust offers and overrides the ticker-derived `dex_best_bid`/`dex_best_ask`. This ensures mid-price calculations reflect meaningful liquidity, not spam.

- **Pre-balance check in Step 9e peg-crossing taker**: Before calling `take_offer()`, the engine now queries the spendable balance of the relevant wallet (quote wallet for ASK takes, base wallet for BID takes) and skips the take with a warning if funds are insufficient. Prevents doomed RPCs that produce "Can't select amount higher than our spendable balance" errors and the associated "peg-arb failed" log noise.

- **Wallet sync gating before startup inventory seeding**: Added a sync-wait loop before the startup inventory seeding block. The engine polls `get_sync_status()` up to 30 times (~5 minutes) waiting for the wallet to report fully synced before querying balances. Prevents unreliable balance data from a partially-synced wallet from seeding incorrect inventory positions and causing the A-S model to generate bad quotes on the first tick.

## [0.7.27] — 2026-04-08

### Added

- **Inventory ratio guard for Step 9e peg-crossing taker**: New config field `peg_arb_max_inventory_ratio` (default 0.70) caps how far inventory can skew before Step 9e suppresses takes. When base holdings exceed 70% of portfolio value, ASK takes (buying more base) are suppressed. When base holdings drop below 30%, BID takes (selling base) are suppressed. Prevents the engine from accumulating unlimited BYC from the ~2,500 artificially cheap sub-peg offers on the Dexie BYC/wUSDC.b book while still allowing opportunistic takes within the allowed balance range.

## [0.7.26] — 2026-04-08

### Fixed

- **Inventory seeding from wallet balances at startup**: The `InventoryTracker` started at zero for all assets and only updated from recorded fills. With no fill history, `inventory_ratio()` permanently returned 0.5 (perfectly balanced), causing the Avellaneda-Stoikov model to place symmetric quotes regardless of actual wallet composition. This led to buying BYC and selling wUSDC.b without any rebalancing pressure. Fix: at engine startup (after wallet connection and offer reconciliation), query on-chain spendable balances for each configured pair's assets and call the new `seed_position()` method. The inventory skew model now reflects real holdings from the first tick, generating appropriate bid/ask asymmetry to rebalance towards target allocation.
- **`ensure_wallet_ids()` public method on OfferManager**: The asset-to-wallet-ID cache (`init_wallet_id_map()`) was only populated lazily inside `post_quotes()`. Inventory seeding at startup needed wallet IDs before any quotes were posted. Added a public `ensure_wallet_ids()` coroutine that populates the cache on demand — safe to call multiple times (no-op after first initialization).

## [0.7.25] — 2026-04-08

### Fixed

- **Competing offer fetch for CAT/CAT pairs**: The Dexie API `pair_id` parameter for non-XCH denomination tokens (e.g. wUSDC.b) returns ALL 584K+ offers involving that token — not just the target pair. With `page_size=100`, BYC/wUSDC.b offers were never found. Fix: for CAT/CAT pairs, fetch each direction separately using `offered`/`requested` asset ID parameters, yielding precise per-pair results. XCH/CAT pairs continue using the faster `pair_id` path unchanged.
- **Dust threshold scaling for CAT pairs**: `min_competitor_offer_size` (default 1 trillion mojos = 1 XCH) rejected all BYC offers because BYC uses 1000 mojos/unit. Fix: scale the threshold proportionally by `base_mojos_per_unit / kMojosPerXch`, with a floor of 1 unit. For BYC (1000 mpu), effective threshold = 1000 mojos (1 BYC). For XCH, unchanged.
- **BID-side competing offer price normalization**: Dexie API always reports `price = requested/offered`. For ASK offers (offered=base) this matches market convention. For BID offers (offered=quote) it's the reciprocal. Fix: invert BID prices to market convention at ingestion, fixing Step 9e peg comparison, competitive cap (Step 7), and spread analysis for all pairs.

## [0.7.24] — 2026-04-08

### Added

- **Step 9e — Peg-crossing offer taker**: Automatically takes competing offers that cross the $1 peg on stablecoin pairs when the depeg detector reports Normal status (peg trusted). BIDs above peg are free premium to sell into; ASKs below peg are discounted buys. Guarded by depeg detector — will not fire during Warning, Bailed, or SuspectedFailure states, preventing the engine from buying into a real depeg event. New config fields: `peg_arb_enabled`, `peg_arb_min_edge_bps` (default 5 bps), `peg_arb_max_take_units` (default 50 units).

## [0.7.23] — 2026-04-08

### Fixed

- **Stablecoin peg guard for BID prices**: Added hard cap in Step 7 that drops any BID tier priced at or above `peg_target` on stablecoin pairs. Prevents the engine from ever bidding >= $1.00 on a stablecoin — even when a crossed or noisy order-book pushes the computed mid above peg. ASK tiers are also floored at `peg_target × (1 + margin_bps)` using per-pair `min_profit_margin_bps_override` (or global fallback). Fixes issue where BYC/wUSDC.b BID tiers were placed above market mid due to crossed-book normalization.

## [0.7.22] — 2026-04-08

### Added

- **Automatic coin pool maintenance**: New `step_maintain_coin_pool()` runs at engine startup and every `coin_pool_interval_blocks` (default 50) to self-send XCH and maintain a pool of pre-split coins. Prevents the large-coin-locking problem where a single UTXO (e.g. 135 XCH) gets locked by a small offer, starving all other offers of capital.
- **`get_next_address` RPC method**: Added to `ChiaWalletRPC` for obtaining wallet receive addresses (used by coin splitting).
- **Coin pool config**: New `strategy` fields: `coin_pool_target_count` (default 20), `coin_pool_target_xch` (default 5.0), `coin_pool_interval_blocks` (default 50). Set `coin_pool_target_count: 0` to disable.
- **Zero-fee coin splitting**: Splits use fee=0 by default since Chia mainnet mempool is rarely congested.

## [0.7.21] — 2026-04-08

### Changed

- **Proportional DEX/CEX divergence response**: Replaced the binary 1.5× spread multiplier (triggered at >200 bps divergence) with a linear ramp: `mult = 1.0 + min(divergence_bps / 1000, 0.5)`. At 200 bps divergence the multiplier is now 1.2× instead of 1.5×, scaling up to max 1.5× at 500+ bps. Eliminates the cliff-edge that caused uncompetitive pricing.
- **Reservation mid clamp tightened 2% → 1%**: Bids and asks now stay within 1% of market mid instead of 2%, keeping quotes competitive when the Avellaneda-Stoikov reservation price diverges.
- **`high_vol_multiplier` now configurable**: Exposed the high-volatility regime multiplier (previously hardcoded at 1.80) as a YAML parameter in `strategy.high_vol_multiplier`. Set to 1.3 (30% widen vs 80%).
- **Config tuning for competitiveness**: `gamma` 0.01→0.005 (halves adverse selection base), `max_half_spread_bps` 250→150 (300 bps round-trip cap), `wall_size_threshold_xch` 20→50, `wall_niche_premium_pct` 15%→5%.
- **Circuit breakers tightened**: `max_drawdown_pct` 10%→5%, `loss_window_blocks` 1152→576 (16h→8h), `max_window_loss_bps` 500→250 (5%→2.5%). Engine pauses earlier if trades are losing money.

## [0.7.20] — 2026-04-07

### Fixed

- **Stablecoin ask price floor (peg guard)**: Added a final safety net in Step 7 that floors all ASK prices at `peg_target × (1 + min_margin_bps)` and caps all BID prices at `peg_target × (1 − min_margin_bps)` for stablecoin pairs. Previously, when the market mid dipped below peg (e.g. BYC trading at 0.98 wUSDC), the engine would place asks below $1.00 — effectively selling a $1-pegged asset at a loss. The peg guard prevents this regardless of what upstream pricing stages compute.
- **Stablecoin undercut bounds respect peg**: The competitive undercut logic (penny-ahead for tier 0) now enforces peg-based bounds in addition to mid-based bounds. Asks cannot undercut below `peg × (1 + margin)` and bids cannot overbid above `peg × (1 − margin)`.
- **Peg-anchor threshold now inclusive**: Changed `dev < threshold` to `dev <= threshold` so that deviations exactly at the configured `peg_anchor_threshold_pct` (e.g. 2.0%) trigger blending rather than being skipped.
- **GUI engine binary search order**: `engine_bridge.py` now checks `cpp/build/Release/xop_trader.exe` before the project-root copy, preventing stale root-level binaries from being launched over freshly-built ones.

## [0.7.19] — 2026-04-07

### Added

- **Configurable minimum offer size (`min_offer_size_units`)**: New global strategy field (default 1.0) and per-pair override (`min_offer_size_units_override`) control the minimum tier size in base-asset units. Tiers below this threshold are dropped in Step 7 before posting. Prevents dust-sized offers (e.g. 2-3 BYC ≈ $2-3) that waste XCH fees, fragment wallet UTXOs, and clutter the DEX with economically insignificant offers.

### Fixed

- **BYC/wUSDC.b dust offers**: Set `min_offer_size_units_override: 10` for BYC/wUSDC.b, requiring at least 10 BYC (~$10) per tier. Previously the 1-unit (1 BYC) minimum allowed tiers as small as 2-3 BYC through, with all tiers clamped to the same price after the order-book guard — posting redundant identical-price offers worth $2-3 each.

## [0.7.18] — 2026-04-07

### Fixed

- **Offer churn from overly aggressive soft TTL threshold**: The `kSoftTtlAdverseThreshold` was 0.2%, far tighter than the normal-zone threshold (0.5%) and well below the 2% reservation_mid clamp range. Normal Avellaneda-Stoikov model fluctuations within the 2% clamp produced >0.2% "adverse" deviation, causing offers to expire every 60 blocks (~18 min), get cancelled, then immediately recreated — wasting fees and blocking all offer posting during the pending_change confirmation window. Raised threshold from 0.2% to 2.0% to match the reservation_mid clamp range.
- **Increased default offer TTL from 60 to 180 blocks** (~56 min): 60 blocks (~18 min) was too aggressive for Chia's slow DEX market, causing unnecessary churn. Hard TTL (2× soft) is now 360 blocks (~112 min).

## [0.7.17] — 2026-04-07

### Added

- **Cross-stablecoin arbitrage (Step 9d)**: New engine step detects and takes cross-market arbitrage between XCH/BYC and XCH/wUSDC.b order books. Since both BYC and wUSDC.b are USD-pegged stablecoins (~$1), XCH should be priced equivalently on both markets after adjusting for the BYC/wUSDC.b cross-rate. When one market's ask is cheaper than the other's bid by more than `cross_stable_min_edge_bps` (default 15 bps), the engine takes the cheap ask. Based on Shleifer & Vishny (1997) limits-to-arbitrage theory and Makarov & Schoar (2020) cross-venue crypto spreads.
- **Spread-aware triangular arbitrage**: `scan_triangular` now uses bid/ask prices when available instead of mid prices. Selling legs use bid prices; buying legs use 1/ask. This eliminates phantom profits from ignoring half-spreads across the 3-leg route (Kozhan & Tham 2012). Falls back to mid prices when bid/ask data is unavailable.
- **New config fields**: `cross_stable_arb_enabled`, `cross_stable_min_edge_bps`, `cross_stable_max_take_xch` under the `arbitrage:` section.
- **`PairBidAskMap` type and `set_pair_bid_asks()` setter** for providing bid/ask data to the arbitrage detector.

## [0.7.16] — 2026-04-07

### Fixed

- **Budget-starved offers falsely cancelled as price_adverse(100%)**: When low XCH balance causes all new tiers to be dropped by the sub-unit minimum filter or dynamic tier limiter, `classify_tier_staleness` could not find the pending offer's tier index in the (now empty) new ladder and unconditionally classified it as `Stale` with 100% adverse price deviation. This cancelled perfectly healthy offers that could not be replaced, creating a pointless cancel→0-offers→post→cancel cycle wasting fees. Fixed by falling back to a mid-price sanity check when the tier is absent from the new ladder: offers that have not crossed the mid-price are classified as `Fresh` and kept alive; only crossed-mid offers (immediate adverse selection risk) are cancelled. Hard TTL expiration is unaffected.

## [0.7.15] — 2026-04-07

### Fixed

- **Reconciler falsely cancels all live offers (string status regression)**: The Chia wallet (newer versions) returns offer status as string names (`"PENDING_ACCEPT"`, `"CANCELLED"`, `"CONFIRMED"`, etc.) instead of integer codes. The v0.7.12 fix handled this with `std::stoi()`, which works for `"3"` but throws on `"CANCELLED"`. The catch block skipped the offer entirely, so `PENDING_ACCEPT` offers were missing from the wallet map, falsely marked "NOT FOUND", and cancelled via `on_chain_reconcile`. Fixed by adding a proper string-name-to-integer-code lookup for all six Chia offer states.

## [0.7.14] — 2026-04-07

### Fixed

- **XCH-buy-only deadlock: dynamic tier limiter blocks recovery bids**: When spendable XCH is marginally above the fee reserve (e.g. 1.208 XCH with 1.0 reserve), the Step 8 dynamic tier limiter trimmed ALL tiers to zero because the `xch_budget` (0.008 XCH) couldn't cover the 0.25 XCH UTXO overhead per offer. This created a permanent deadlock: no XCH-buy bids could be posted, so XCH could never recover. Fixed by skipping the Step 8 XCH budget limiter when `xch_buy_only_mode` is active for pairs involving XCH. The offer_manager's per-tier UTXO-lock recovery zone check already handles safety correctly, allowing XCH-buy bids when spendable ≥ 1× reserve.

## [0.7.13] — 2026-04-07

### Added

- **Cross-pair correlated inventory skewing (Guéant 2019)**: When multiple pairs share a common asset (XCH↔BYC↔wUSDC.b triangle), each pair's inventory skew now accounts for inventory pressure from other pairs. If XCH/BYC is short BYC, BYC/wUSDC.b automatically skews its bids to acquire more BYC. The adjustment is weighted by the market allocator's allocation fractions and clamped to prevent runaway skew. Configurable via `cross_pair_skew_enabled` (default: off) and `cross_pair_skew_phi` (default: 0.30).

### Changed

- **Inventory aging enabled** with conservative settings: positions begin relaxing the no-loss constraint after 500 blocks (~7 hours), up to 25 bps max loss, preventing capital from getting permanently locked in underwater positions.

- **Circuit-breaker auto-rebalance enabled**: When inventory ratio exceeds 80% and the position has aged past the threshold, the loss manager is automatically engaged with a 100 bps loss budget to free locked capital.

- **Triangular arbitrage threshold lowered** from 30 bps to 12 bps (just above the fee floor), enabling more frequent marginal rebalancing trades through the XCH↔wUSDC↔BYC triangle.

## [0.7.12] — 2026-04-07

### Fixed

- **Offers cancelled immediately after creation by on-chain reconciler**: `verify_pending_offer_coins` crashed with `json.exception.type_error.302` when the Chia wallet returned offer `status` as a string instead of an integer. After the error, the wallet offer map was empty, causing ALL pending offers to be falsely marked "NOT FOUND in wallet" and cancelled via `on_chain_reconcile`. Fixed by (1) handling `status` as either int or string, and (2) aborting stale detection entirely when the wallet query fails completely, preserving all pending offers.

- **Sub-unit BYC offers wasting fee coins**: The engine created BYC/wUSDC.b offers with sizes as small as 970 mojos (0.97 BYC, < 1 full unit), which are economically insignificant and waste XCH on chain fees. Added a minimum offer size filter in Step 7 that drops any tier where `size < base_mojos_per_unit` (1000 mojos for CAT tokens, ensuring at least 1 full unit per offer). Logged as `[Engine] Step 7: dropped N sub-unit tiers`.

## [0.7.11] — 2026-04-07

### Fixed

- **UTXO-lock deadlock when XCH balance drops between 1× and 2× reserve**: The 2× reserve UTXO-lock guard blocked ALL offers — including buy-XCH bids — when spendable XCH fell below `2× fee_reserve_xch`. This created an unrecoverable deadlock: the engine wanted to buy XCH to restore balance, but couldn't create the offers needed to do so. Added a "recovery zone" (1× ≤ spendable < 2× reserve) that allows buy-XCH offers through while still blocking sell-XCH and non-XCH offers. Applied to all three UTXO-lock checks in offer_manager (batch pre-check, per-tier pre-check, and post-creation guard)

## [0.7.10] — 2026-04-06

### Added

- **Warning log when min_fee floor significantly exceeds mempool estimate**: `FeeTracker::get_recommended_fee()` now emits a `[warn]` when `min_fee_mojos` clamps the fee up by more than 10x over the mempool estimate, making silent overpaying immediately visible in logs
- **FeeTracker unit tests** (18 tests): Covers fee selection, min/max clamping, adaptive mempool usage, budget enforcement, fee-vs-gain gating, and documents the overpay scenario that prompted v0.7.9

## [0.7.9] — 2026-04-06

### Changed

- **Reduced fee estimate target from 60s to 300s**: The `get_fee_estimate` RPC was requesting fees for 60-second inclusion urgency, producing ~9.3M mojo fees on a near-empty mempool. Market-making offers are long-lived (60-block TTL) and don't need next-block priority. New configurable `fee_estimate_target_seconds` (default 300s) drops the estimate to ~5,661 mojos when the mempool is quiet
- **Lowered `min_fee_mojos` in config.yaml from 5M to 50K**: The previous 5M floor was 880x higher than the blockchain required. Reduced to 50,000 mojos to let the adaptive fee tracker use naturally low fees during quiet periods

### Added

- **`fee_estimate_target_seconds` config field**: New `FeeConfig` setting controlling the target inclusion time passed to the Chia full node's `get_fee_estimate` RPC. Configurable per deployment; higher values = lower fees, lower urgency

## [0.7.8] — 2026-04-06

### Fixed

- **Fee-gain formula inverted for CAT/CAT pairs**: The fee gating calculation used `base_mojos_per_unit / kMojosPerXch` instead of `kMojosPerXch / base_mojos_per_unit`, causing expected gain to evaluate to 0 for CAT pairs (1e3/1e12 = ~0) and blocking all BYC/wUSDC.b offers. Corrected factor restores proper scaling (1e12/1e3 = 1e9) so gain correctly exceeds fee threshold

## [0.7.7] — 2026-04-06

### Fixed

- **Critical mojo scale bug for CAT/CAT pairs**: `base_mojos_per_unit` defaulted to 1e12 (XCH scale) for all pairs, but CAT tokens use 1e3 mojos/unit. For BYC/wUSDC.b this caused a **billion-fold pricing error** — offers requested 1.9 billion BYC for 1.9 wUSDC.b instead of correct ~1:1 ratio
- **Auto-detect mojos-per-unit from asset type**: Config parser now sets `base_mojos_per_unit` and `quote_mojos_per_unit` based on whether asset_id is "xch" (1e12) or a CAT hex ID (1e3), eliminating the need for manual configuration

## [0.7.6] — 2026-04-06

### Added

- **Stablecoin dynamic tier system**: New per-pair PairConfig fields for competitive stablecoin trading:
  - `max_half_spread_bps_override` — per-pair spread cap (replaces global 250 bps with e.g. 75 bps for stablecoins)
  - `peg_anchor_threshold_pct` — configurable deviation threshold for peg-anchor blending (raised from hardcoded 1% to 2%)
  - `peg_anchor_weight` — configurable peg weight in blend (raised from hardcoded 50% to 60%)
  - `stablecoin_exempt_buyonly` — exempts non-XCH stablecoin pairs from XCH-buy-only mode skip
  - `stablecoin_undercut_all_tiers` — enables competitive undercutting on all tiers (not just tier 0)
  - `stablecoin_flat_sizing` — bypasses adverse-selection sizing that crushes inner tiers
  - `stablecoin_skip_gap_aware` — bypasses gap-aware spacing that widens configured tight tiers

### Fixed

- **BYC/wUSDC.b not posting offers**: Pair was entirely skipped during XCH-buy-only mode because it has no XCH component. Now exempt via `stablecoin_exempt_buyonly`
- **A-S model producing 3000+ bps spreads for stablecoins**: Capped to 500 bps by global limit, still 10x too wide. Per-pair `max_half_spread_bps_override: 75` now produces ~45 bps spreads
- **Peg-anchor not activating**: BYC at 0.989 was 1.12% from peg, just outside hardcoded 1% threshold. Configurable `peg_anchor_threshold_pct: 2.0` now activates correctly
- **Gap-aware spacing counterproductive for stablecoins**: Was widening configured tight tiers [8,15,25,40,60,80] → [40,50,60,70,80,90]. Now skipped via `stablecoin_skip_gap_aware`
- **Adverse-selection crushing inner tiers**: Tier 0 was getting only 0.3% of capital. Now bypassed for stablecoins via `stablecoin_flat_sizing`

## [0.7.5] — 2026-04-06

### Fixed

- **Both-sides-suppressed churn fix**: When both bid and ask sides are suppressed (e.g. BYC balance is 0 and XCH spendable ratio is low after UTXO locking), the engine no longer cancels *all* pending offers. Fresh offers (recently created) are now kept live to avoid wasting creation fees and triggering a create→suppress→cancel→create churn cycle. Only stale/expired offers are cancelled to free locked capital

## [0.7.4] — 2026-04-06

### Fixed

- **UTXO-lock safety margin (2× reserve floor)**: All offer creation now requires `xch_spendable >= 2 × fee_reserve_xch` (default 2.0 XCH) before any offer is created. Previously, the engine would create offers when spendable was barely above the 1× reserve (e.g. 1.29 XCH with 1.0 reserve), but Chia's UTXO model locks the *entire* coin used for fee payment — a 0.005 XCH fee can lock a 1.3 XCH UTXO, instantly draining spendable to zero. This caused a create→drain→cancel→create churn cycle that kept spendable at zero indefinitely
- **Buy-only mode fee override raised**: In XCH-buy-only mode, the fee reserve override passed to `offer_manager::post_quotes` is now `fee_reserve_xch` (1.0) instead of `fee_min_spendable_xch` (0.01). The old 0.01 XCH threshold made offer_manager's per-offer guards ineffective, allowing offers that would lock entire UTXOs
- **Preemptive UTXO danger zone detection**: When spendable XCH is between 1× and 2× reserve, the engine now enters XCH-buy-only mode preemptively (before UTXO liberation triggers). This prevents the scenario where cooldown expires, the engine exits buy-only mode, immediately creates offers that drain spendable to zero, and re-enters the churn cycle

## [0.7.3] — 2026-04-06

### Fixed

- **Wallet sync gate**: `step_manage_offers` now checks `get_sync_status()` on every heartbeat and skips ALL offer management when the wallet is not fully synced. Previously, the engine would create/cancel/verify offers against an unsynced wallet, causing `verify_pending_offer_coins` to falsely mark live offers as NOT FOUND (wallet returns incomplete `get_all_offers` during sync), `cancel_offer` to fail with "Wallet needs to be fully synced", and the engine to create duplicate offers that drain XCH to zero
- **Creation grace period in `verify_pending_offer_coins`**: Offers younger than 120 seconds are no longer marked as stale when NOT FOUND in the wallet. The Chia wallet may not immediately surface newly-created offers in `get_all_offers`, and this previously caused the engine to lose track of 15-second-old offers and re-create duplicates
- **Startup reconcile wallet sync pre-check**: `startup_reconcile` now checks wallet sync status before attempting orphan cancellation. When wallet is not synced, orphans are immediately force-adopted instead of attempting cancel RPCs that will fail, preventing uncancellable orphan deadlock

## [0.7.2] — 2026-04-06

### Fixed

- **Prevent orphan-offer XCH deadlock**: Wallet offers that the engine loses track of (due to wallet desync, reconciliation race conditions, or restart) now get force-adopted back into State instead of being silently dropped. Two fixes:
  - `startup_reconcile`: when an orphan offer cannot be cancelled (e.g., 0 spendable XCH), it is now adopted into State rather than left in limbo. Previously, uncancellable orphans locked coins indefinitely with no engine awareness
  - `reconcile_offers`: periodic reconciliation now detects PENDING_ACCEPT wallet offers that are not tracked in engine State and adopts them. This catches mid-session orphans created when `verify_pending_offer_coins` incorrectly removes an offer during wallet desync
- Adds `try_parse_wallet_offer()` helper that extracts pair/side/price/size from a wallet trade record for offer adoption

## [0.7.1] — 2026-04-06

### Fixed

- **GUI shows wallet/node connected during analysis**: The engine now publishes system health metrics (block height, node synced, wallet connected) during the startup analysis phase, not only after analysis completes. Previously the GUI showed "not connected" for the entire 5+ minute analysis window even though the wallet and full node were reachable

## [0.7.1] — 2026-04-06

### Fixed

- **GUI shows wallet/node connected during analysis**: The engine now publishes system health metrics (block height, node synced, wallet connected) during the startup analysis phase, not only after analysis completes. Previously the GUI showed "not connected" for the entire 5+ minute analysis window even though the wallet and full node were reachable

## [0.7.0] — 2026-04-06

### Added

- **Dynamic tier limiting**: Capital-budget tier limiter that automatically reduces the number of active offer tiers when XCH spendable balance cannot support the full tier ladder. Each create_offer locks ~0.25 XCH in fee UTXOs; when posting 6 tiers (3 bid + 3 ask) would exceed the spendable headroom above the fee reserve, outer tiers are pruned from the outside in (highest tier_index first) to prevent UTXO liberation churn. Based on Gueant-Lehalle-Fernandez-Tapia (2013) capital-constrained market making: fewer, well-capitalised tiers outperform many thin tiers when capital is scarce. Includes 20% safety margin above fee reserve and logs tier trimming decisions

## [0.6.9] — 2026-04-06

### Fixed

- **Offer churn: UTXO liberation age guard**: UTXO liberation now skips offers younger than 5 blocks (~2.5 minutes) instead of immediately cancelling freshly-posted offers. Previously, posting 3 tiers would lock enough XCH to drop spendable below the fee reserve, triggering liberation to cancel all 3 on the very next heartbeat — creating a perpetual create-cancel cycle every ~7 heartbeats. Fresh offers now survive until they can be filled or age out, while truly stale offers are still liberated normally

## [0.6.8] — 2026-04-06

### Added

- **GUI singleton enforcement**: On startup, the GUI now terminates any previously-running GUI and engine processes before launching, ensuring only one GUI and one engine run at a time. Prevents double-posting offers, port conflicts, and wallet RPC contention. Uses WMI process scanning on Windows and `/proc` enumeration on POSIX. Protects the venv launcher parent PID from self-termination

## [0.6.7] — 2026-04-05

### Fixed

- **Fee reserve recovery deadlock**: The v0.6.6 fee reserve fix blocked the engine's `xch_buy_only_mode` recovery path — the offer_manager's independent pre-check used the full 1.0 XCH threshold, overriding the engine's lower 0.01 XCH floor for recovery offers. Added `fee_reserve_override` parameter to `post_quotes()` so the engine can pass the recovery threshold when in `xch_buy_only_mode`, breaking the deadlock cycle

## [0.6.6] — 2026-04-05

### Fixed

- **Fee reserve enforcement for buy-XCH offers**: Removed the `tier_buys_xch` exemption that allowed buy-XCH offers to bypass spendable balance checks. All offers lock XCH UTXOs at creation time regardless of trade direction; 4 guard locations in offer_manager.cpp now enforce the reserve uniformly
- **Hard minimum spendable floor in xch_buy_only mode**: Engine Step 8 now checks `fee_min_spendable_xch` (0.01 XCH) before creating any offer in recovery mode, preventing the last dust UTXO from being locked
- **`xch_spendable_pre` scope**: Moved declaration to outer scope so it is accessible in the pair loop

### Added

- **Startup singleton enforcement**: New `kill_old_instances()` in main.cpp terminates any previously-running `xop_trader` processes before the engine starts, preventing port conflicts and double-posting. Uses `CreateToolhelp32Snapshot` on Windows, `/proc` enumeration on Linux
- **XCH currency symbols on dashboard**: Metric cards and per-pair PnL column now display XCH symbols
- **USD conversion on dashboard**: Live XCH/USD rate derived from XCH/wUSDC.b mid-price; PnL and fee cards show approximate USD equivalent

## [0.6.5] — 2026-04-05

### Added

- **Strategy analytics data collection**: Extended the `snapshots` table with 8 new strategy decision columns persisted every block: `reservation_price_mojos` (A-S reservation price), `half_spread_bps` (optimal half-spread), `kappa` (calibrated fill-intensity decay), `variance_ratio` (Lo-MacKinlay VR statistic), `adverse_rate` (fraction of adverse fills), `s_adverse_bps` / `s_inventory_bps` / `s_cost_bps` (Stoll three-component spread decomposition)
- **Per-tier quote persistence**: New `strategy_quotes` table stores every bid/ask quote at each tier level every block (`block_height`, `pair_name`, `tier`, `side`, `price_mojos`, `size_mojos`), enabling fill probability modelling, tier spacing optimization, and quoted-vs-filled spread analysis
- Forward-compatible ALTER TABLE migrations for existing databases

## [0.6.4] — 2026-04-05

### Added

- **Smart orphan management (CAOE)**: Cost-Aware Orphan Evaluation replaces the blind cancel-all-orphans startup logic with scholarly-grounded per-offer decision making. On startup, each orphaned wallet offer (PENDING_ACCEPT but not in the engine DB) is evaluated against the current Dexie mid-price to determine whether it should be **adopted** (re-tracked), **adopted-stale** (re-tracked but scheduled for immediate refresh), or **cancelled**
  - Academic basis: Guéant, Lehalle & Fernandez-Tapia (2013) "Dealing with the Inventory Risk" — cancel only when expected adverse selection loss exceeds cancellation cost; Gao & Wang (2020) "Optimal market making in the presence of latency" — the zero-offer gap during cancel→repost is the primary adverse selection cost for slow-chain market makers; Aït-Sahalia & Saglam (2017) — stale-quote risk scales with price deviation, remaining lifetime, and offer size
  - Parses wallet trade record `summary` field to extract pair, side, price, and size from each orphan's offered/requested asset maps
  - Fetches current Dexie ticker mid-prices for all enabled pairs during startup reconciliation
  - Computes signed price deviation and determines adverse direction (bid above mid = adverse, ask below mid = adverse)
  - Applies inventory-aware tolerance bonus: orphans that help reduce inventory imbalance get an extra `orphan_inventory_bonus` (default 1%) added to the adverse threshold
  - AdoptStale disposition sets the offer's `created_at_block` near the TTL boundary, triggering an immediate selective refresh on the next heartbeat — no stale offers linger
  - Adopted orphans are persisted to the DB so they survive the next restart without re-evaluation churn
  - New config params: `orphan_adopt_enabled` (default: true), `orphan_adverse_threshold` (default: 0.02 = 2%), `orphan_max_adopt_age_blocks` (default: 120 ≈ 104 min), `orphan_inventory_bonus` (default: 0.01 = 1%)
  - Comprehensive per-orphan logging with disposition, deviation %, and human-readable reasons

## [0.6.3] — 2026-04-05

### Fixed

- **UTXO liberation**: When all XCH spendable is consumed by offer UTXO locking (spendable=0 but confirmed balance is healthy), the engine now cancels the oldest pending offers at the start of Step 8 to free locked UTXOs. Cancels up to 3 offers per heartbeat, re-checking spendable after each. This breaks the deadlock where the engine was frozen for hours with 16+ XCH confirmed but 0 spendable
- **Secure cancel with fee=0**: `emergency_cancel` now attempts `secure=true, fee=0` before falling back to insecure (local-only) cancel. The offer's own locked coins serve as spend bundle inputs, allowing on-chain invalidation without requiring spendable XCH for fees. UTXO liberation uses `prefer_zero_fee` mode to try fee=0 FIRST (before descending fee tiers) to avoid burning spendable XCH on cancel fees
- **Anti-churn + cooldown**: After liberation cancels offers or finds none to cancel, the pair loop is skipped (`co_return`) to prevent creating offers that would get immediately liberated next heartbeat. A 5-heartbeat cooldown further suppresses the pair loop when spendable briefly recovers above reserve (1.0 XCH) but below 2× reserve (2.0 XCH), preventing the post→cancel residual churn cycle that wasted ~0.01 XCH/cycle
- **Recovery mode duplicate block removed**: Fixed corrupted duplicate code block in `step_xch_recovery()` that caused compilation errors

## [0.6.2] — 2026-04-04

### Fixed

- **Recovery oscillation**: Recovery mode now checks `confirmed_wallet_balance` in addition to `spendable_balance`. When confirmed XCH is healthy (>= threshold) but spendable is low due to UTXO locking from our own offers, recovery is no longer triggered — eliminating the 15-second cancel/re-post oscillation cycle
- **BYC trading enabled**: Lowered `min_trading_units` from 10.0 to 2.0 in config, unblocking XCH/BYC and BYC/wUSDC.b pairs that were suppressed because actual balances (BYC=3.0, wUSDC.b=7.5) were below the threshold

## [0.6.1] — 2026-04-04

### Fixed

- **Step 8 pre-gate uses full reserve**: The per-pair XCH balance gate before offer posting now checks against `fee_reserve_xch` (1.0 XCH) instead of `fee_min_spendable_xch` (0.01 XCH). Previously, the second pair could drain the entire reserve through UTXO locking because 0.01 was trivially satisfied
- **Per-offer pre-creation balance check**: `post_quotes()` now verifies XCH spendable >= `fee_reserve_xch` before each individual `create_offer()` call (both batch and non-batch modes), catching mid-cycle UTXO drain that the post-creation guard couldn't prevent
- **Recovery cancels via wallet RPC**: Recovery mode now calls `wallet.cancel_offers(fee=0, secure=false)` directly instead of `offer_mgr->cancel_all()`, which only checked engine state (empty after restart). This cancels ALL wallet offers including those from previous engine instances, and works with 0 XCH spendable
- **Recovery takes with zero fee**: When XCH spendable < 0.001, recovery mode uses fee=0 for `take_offer` calls, breaking the deadlock where taking XCH asks required XCH for fees

## [0.6.0] — 2026-04-04

### Added

- **Dynamic Market Allocator**: New `MarketAllocator` scores each trading pair across 5 dimensions (spread quality, volume, competition, fill rate, triangular arb edge) and dynamically shifts capital allocation toward the most attractive markets
- **Triangular arbitrage detection**: Computes forward and reverse cycle edges across the XCH/wUSDC ↔ XCH/BYC ↔ BYC/wUSDC triangle, net of per-leg fees, and factors the edge into allocation scoring
- **Allocation guardrails**: Configurable min/max per-pair allocation (default 10–50%), hysteresis threshold to prevent oscillation, and EMA smoothing for gradual capital shifts
- **`market_allocator` config section**: Full configuration for weights, intervals, fee assumptions, and allocation bounds
- **XCH Recovery Mode**: Automatic XCH acquisition when spendable balance drops below threshold (default 0.25 XCH). Cancels all offers, gates Steps 7-8, and scans Dexie order books for reasonably-priced XCH asks to take — resuming normal trading once balance recovers above target (default 1.0 XCH). Configurable via `recovery:` section
- **Split fee reserve**: `fee_reserve_xch` (inventory holdback) and `fee_min_spendable_xch` (fee gate) are now separate parameters, allowing fees to draw from the reserve without blocking trading

### Fixed

- **Dexie outlier price filter**: Reject bid/ask data from Dexie that is >10× or <0.1× the CEX reference price, preventing flash-crash triggers from garbage offers (e.g. $979M ask on a $2.38 asset)

## [0.5.9] — 2026-04-04

### Fixed

- **Emergency cancel fee cap**: Previous emergency cancel used `spendable - 1000 mojos` as the fee, burning up to ~1 XCH on a single cancel. Now capped at 2× the dynamic fee (~0.02 XCH max)
- **Emergency cancel fee retry cascade**: When wallet reports insufficient funds, emergency cancel now halves the fee and retries (2× dynamic → 1× → ½ → ¼ → ... → 1 mojo) before falling back to insecure local-only cancel. Enables cancellation even when spendable XCH is far below the configured minimum fee
- **Batch fallback fee reserve guard**: When batch offer creation fails and falls back to per-tier, now checks spendable balance after each successful tier and stops if below reserve

## [0.5.8] — 2026-04-04

### Fixed

- **UTXO-aware fee reserve enforcement**: Previous logical-deduction reserve was ineffective because Chia's UTXO model locks entire coins, not surgical amounts. Step 8 now queries actual XCH spendable balance before each pair and skips if below `fee_reserve_xch`. Post-creation guard in `post_quotes()` re-checks after each offer
- **Emergency cancel on insufficient funds**: When a cancel fails due to insufficient fee balance, automatically retries with a reduced fee (spendable minus dust margin) or falls back to local-only insecure cancel. Applied across all 6 cancel paths: `cancel_stale`, `cancel_all`, `selective_cancel`, asymmetric bid/ask cancel, and `startup_reconcile`
- **Broader fee-error detection**: Cancel error matching now catches both "insufficient funds" and "spendable balance" error strings from the Chia wallet RPC

### Changed

- **10× lower default fees**: Reduced `offer_fee_mojos` from 100M to 10M (0.00001 XCH), `min_fee_mojos` from 50M to 5M, `max_fee_mojos` from 500M to 100M. Based on blockchain research: Chia mempool is typically <1% full, fee estimate for instant inclusion is ~3.5M mojos, and most Dexie offers complete with zero blockchain fee

## [0.5.7] — 2026-04-05

### Added

- **DBX liquidity reward auto-claim**: Every offer submitted to Dexie now includes `claim_rewards: true`, automatically claiming DBX rewards from Dexie's Liquidity Incentive Program. Rewards are batched and sent daily. Toggle via `dexie.claim_rewards` config field or GUI checkbox
- **XCH/DBX pair template**: Added disabled XCH/DBX pair in `config.example.yaml` for users who want to farm high-APR DBX rewards (75–135% APR) on the thin DBX market
- **DBX rewards documentation**: New `docs/dbx-liquidity-rewards.md` covering reward rates, eligibility, claiming methods, and XCH/DBX market analysis

### Changed

- **Cancel-reduction: soft/hard TTL split**: The configured `offer_ttl_blocks` (default 60) is now a "soft" TTL. Offers past soft TTL are only expired if they show ≥0.2% adverse deviation — well-priced old offers stay live. Hard TTL at 2× soft (120 blocks ≈ 104 min) is the absolute safety cap
- **Cancel-reduction: tier-scaled threshold**: Outer tiers now tolerate more price movement before cancellation. Effective threshold scales by tier index: tier 0 → 0.50%, tier 1 → 0.75%, tier 2 → 1.00%, tier 3 → 1.25%. Inner tiers remain tightly monitored
- **Cancel-reduction: minimum age guard**: Offers younger than 3 blocks (~2.6 min) are protected from price-deviation cancellation. The round-trip fee for cancel+recreate exceeds adverse selection risk at small deviations. Crossed-mid cancellation still bypasses this guard
- **Cancel-reduction: all-stale branch fix**: When all tiers are classified as Stale (price deviation), the engine now uses `selective_cancel` instead of `cancel_stale(TTL)`. Previously, price-stale offers within TTL were missed by the TTL-only `cancel_stale` path, risking double-posting

## [0.5.6] — 2026-04-04

### Fixed

- **Critical — CAT offer size inflation (10⁹×)**: Step 6 converted GLFT display-unit sizes to mojos using `kMojosPerXch` (10¹²) instead of `pair_cfg->base_mojos_per_unit` (10³ for CAT tokens). BYC offers posted with ~2.38 billion units instead of ~2.38. Fixed by using correct per-pair mojos denominator
- **Critical — Reservation mid-price runaway**: Avellaneda-Stoikov formula produced absolute half-spread of ~3.35 price units regardless of price level. For BYC at $0.99, reservation_mid inflated to 2.17× market. Added 2% max-deviation clamp on reservation_mid from market mid
- **VPIN fill volume conversion**: Same `kMojosPerXch` vs `base_mojos_per_unit` bug in VPIN fill volume tracking inflated volume metrics by 10⁹× for CAT pairs
- **Steps 7/8 mid-price source**: Tier ladder and no-loss checks now use market mid directly instead of deriving from skewed risk_quote, preventing A-S inventory skew from distorting order placement

### Added

- **Crossed-book arbitrage taking**: Detects and takes profitable crossed-book opportunities on Dexie (peer-to-peer DEX with no matching engine). New `ArbitrageType::CrossedBook` with configurable min edge (bps) and max take size (XCH)
- **Dexie crossed-book data acceptance**: `ingest_dexie()` no longer discards order book data when bid > ask, which is normal for unmatched P2P offers

## [0.5.5] — 2026-04-03

### Added

- **AMM-aware mid-price blending**: 3-source mid-price computation (DEX 70% + CEX 30% + AMM 15%) with freshness-weighted re-normalisation. TibetSwap implied price feeds via `ingest_amm_mid()` so the engine tracks AMM fair value in real time
- **Order-book gap detection**: `analyse_order_book_gaps()` scans competing offers for underserved price ranges per-side, returning gaps sorted by width for dynamic tier placement
- **Dynamic gap-aware tier spacing**: new `compute_ladder()` overload shifts tier spacing toward detected gaps in the competing order book, with configurable blend factor and ascending-constraint enforcement
- **Adverse-selection-aware tier sizing**: inverse-decay weighting shrinks tier 0 (most vulnerable to informed traders on Chia's 52s blocks) and redistributes capital to outer tiers. Extra-conservative sizing under high volatility
- **Dexie price inversion fix**: Dexie API returns prices as "XCH per CAT" but the engine expected "CAT per XCH". Added inversion + bid/ask swap in `get_ticker()` to produce correct mid-price
- **16 new liquidity tests**: `test_liquidity.cpp` covering gap detection, adverse-selection sizing, gap-aware spacing, AMM mid-price blending, and edge cases
- 8 new config fields: `gap_aware_spacing`, `min_gap_bps`, `max_gap_scan_bps`, `gap_blend_factor`, `adverse_selection_sizing`, `adverse_selection_decay`, `adverse_selection_sigma_threshold`, `amm_blend_weight`

### Fixed

- **Critical — No-book guard bypass**: When Dexie returned no quotes (bid=0, ask=0), the order-book price guard silently skipped, allowing offers through without any market reference. Now clears the ladder entirely when no book reference exists
- **Critical — Missing final sanity check**: Tiers with non-positive prices could survive all adjustments. Added remove_if sweep dropping any tier with price ≤ 0
- **High — No-loss bypass after price guard clamp**: Price guard could clamp an ASK below cost basis, negating the `enforce_no_loss` from step 6. Added post-clamp re-check that drops ASK tiers violating the cost-basis + margin floor
- **High — Degenerate adverse-selection sizing**: Floating-point edge cases (NaN/Inf/underflow) in tier sizing normalization. Added `isfinite` validation with fallback to baseline config
- **Gap-aware spacing side-overwrite bug**: Per-side `for(side : {Bid, Ask})` loop mutated shared `tier_spacing_bps` — Ask overwrote Bid adjustments. Fixed by merging gap centers from both sides into one pass
- Order-book price guard added: clamps BID ≤ dex_best_ask and ASK ≥ dex_best_bid to prevent crossing the existing spread

## [0.5.4] — 2026-04-03

### Fixed

- **Critical**: Integer overflow in `build_offer_dict()` — the formula `tier.size * tier.price / quote_denom` overflowed int64 (product ~10²⁴), producing garbage amounts that the wallet rejected as "insufficient funds" for both BID and ASK offers. Fixed by decomposing into proper unit conversions: `(size/base_mojos_per_unit) × (price/kMojosPerXch) × quote_mojos_per_unit`, computed in double to avoid overflow
- **Chia wallet status field compatibility**: Newer Chia wallet versions return trade-record `status` as strings (`"PENDING_ACCEPT"`, `"CONFIRMED"`, etc.) instead of integers. Three call sites in `OfferManager` crashed with `json::type_error.302`. Added `trade_status::parse()` helper that accepts both formats

## [0.5.3] — 2026-04-03

### Added

- **Stuck transaction pruning**: new `OfferManager::prune_stuck_transactions()` detects wallet transactions with no spend bundle (stuck > 10 min) and clears them via `delete_unconfirmed_transactions` RPC. Runs automatically at startup
- **Pending-change gate**: Step 8 now queries live wallet balances before posting offers. If any wallet has `pending_change > 0` (coins in-flight from a prior transaction), offer creation is skipped until the pending transaction confirms on-chain (~1-2 blocks). Prevents the wallet daemon from reusing already-spent coins across concurrent offers
- `ChiaWalletRPC::delete_unconfirmed_transactions()` — clears stuck unconfirmed transactions from a wallet
- `ChiaWalletRPC::get_transactions()` — retrieves recent transactions for stuck-tx detection

### Fixed

- **Critical**: Wallet coin double-spend causing permanently stuck transactions. Rapid consecutive `create_offer` calls could select the same unspent coin, producing spend bundles that never broadcast. The pending-change gate and stuck-tx pruner prevent and recover from this condition
- Spendable reserve gate was previously a dead no-op (`cached_wallet_balances_` never populated). Now queries live wallet balances per-pair before posting

## [0.5.2] — 2026-04-03

### Added

- **Startup offer reconciliation**: on launch, engine queries the database for pending offers and scans the wallet for all PENDING_ACCEPT offers. Known offers are restored into State for tracking; unknown orphans are automatically cancelled to free locked capital
- `Database::query_pending_offers()` — retrieves all offer_log rows with status='pending' for startup recovery
- `OfferManager::startup_reconcile(known_ids)` — wallet-wide scan that cancels orphaned offers not tracked in the database

### Fixed

- Offers orphaned by engine restarts or crashes are now detected and cancelled automatically, preventing indefinite capital lockup (previously required manual cleanup)

## [0.5.1] — 2026-04-03

### Fixed

- **Critical**: Eliminate zero-offer gap during cancel→repost cycle via selective refresh (Gao & Wang 2020). New `classify_tier_staleness()` evaluates per-tier price deviation; only stale/expired tiers are cancelled while Fresh tiers remain live on the order book
- **Critical**: `cancel_stale()` now treats wallet cancel as authoritative — offer ID recorded as cancelled even if `state_->remove_offer()` returns false, preventing orphaned-offer re-cancel loops
- **Critical**: Shutdown DB persistence retries `update_offer_status()` up to 3 times, preventing ghost "pending" records that cause phantom offers on next startup
- **Critical**: `detect_fills()` position accounting failures no longer suppress fill emission — fills are always recorded and offers always removed regardless of `record_buy`/`record_sell` outcome
- **High**: Asymmetric ladder guard in batched `post_quotes()` — if one side (bid/ask) fails completely while the other succeeds, the posted side is cancelled to prevent one-sided book exposure
- **High**: Wallet ID cache (`wallet_ids_resolved_`) now invalidated on circuit-breaker recovery via new `invalidate_wallet_ids()` method, allowing runtime discovery of newly added CAT wallets

### Added

- `OfferManager::classify_tier_staleness()` — per-tier staleness classification (Fresh/Stale/Expired) based on price deviation from optimal ladder
- `OfferManager::selective_cancel()` — cancel only stale/expired tiers, leaving fresh tiers live
- `OfferManager::invalidate_wallet_ids()` — force wallet-ID cache rebuild on next `post_quotes()`
- `TierStaleness` enum and `TierClassification` struct for selective refresh decision-making
- Selective refresh filter in `Engine::step_manage_offers()` — posts replacement tiers only for cancelled slots, preventing double-exposure at fresh price levels
- `kSelectiveRefreshThreshold` constant (0.5%) for per-tier staleness classification

### Changed

- `Engine::step_manage_offers()` now uses 3-phase decision: classify → selective cancel → filtered repost (replaces blanket cancel_stale → post_quotes)
- Adaptive fees enabled by default in config

## [0.5.0] — 2026-04-03

### Added

- Spendable reserve gating: engine skips offer posting when any wallet's spendable/confirmed ratio falls below configurable threshold (`min_spendable_reserve_pct`, default 25%)
- Stuck offer detection and auto-cancellation: offers surviving beyond `offer_ttl_blocks + stuck_offer_age_blocks` are logged with fee info and cancelled
- Per-offer fee tracking: `fee_mojos` field stored on `PendingOffer`, `DbOfferRecord`, and `offer_log` DB table for fee-to-fill-time analytics
- Prometheus gauge `xop_spendable_reserve_pct{wallet}` — fraction of confirmed balance that is spendable (0–1)
- Prometheus gauge `xop_stuck_offers` — count of offers stuck beyond TTL + stuck-age threshold
- Config fields: `strategy.min_spendable_reserve_pct` (double, 0–1) and `strategy.stuck_offer_age_blocks` (uint32_t, default 30)
- GUI dashboard: wallet balance card shows reserve percentage with color-coded thresholds (red <10%, yellow <25%) and stuck-offer warning row
- GUI `MetricsService.get_spendable_reserve()` and `get_stuck_offers()` Prometheus parsers
- Forward-compatible DB migration: `ALTER TABLE offer_log ADD COLUMN fee_mojos INTEGER DEFAULT 0`
- Wallet balance Prometheus export (`xop_wallet_balance{wallet,field}`) for spendable, confirmed, unconfirmed, pending_change, pending_coin_removal, max_send
- Pre-flight balance check in `post_quotes()` — verifies spendable balance before tier loop
- GUI Dashboard "Wallet Balances" card with color-coded status

### Changed

- `resolve_wallet_id()` made public in OfferManager
- Engine `step_manage_offers()` extended with stuck-detection pass and reserve-gating logic

## [0.3.0] — 2026-04-03

### Fixed

- Fix QScrollArea wrapping hiding widget methods (dashboard, market analysis, settings)
- Fix startup analysis never displaying in GUI (`_create_page_widget` scroll wrapper)
- Fix per-side offer posting: bid-side insufficient funds no longer blocks ask-side offers
- Fix `compute_concentration()` returning 0.0 with empty positions (now returns 0.5 balanced)
- Fix GUI metric name mismatches (5 getters: pnl, health, market_data, offers, risk)
- Fix analysis data gating in EngineBridge (removed bot_status == Analyzing requirement)
- Fix null JSON crash in dexie_client.cpp with `json_number_or<T>()` helper

### Added

- Per-pair fault isolation in `step_update_market_state()` (try/catch per pair)
- `_unwrap()` helper for QScrollArea-wrapped page widgets in MainWindow

### Changed

- Default `q_max` guidance: must match actual wallet capacity (was 1000, realistic ~10)

## [0.2.2] — 2026-04-02

### Changed

- Pre-commit icons (icon.ico, icon.png) to repo; remove runtime generation
- Desktop shortcut enabled by default in Windows installer
- Single installer exe for Windows releases (standalone binaries removed)
- Release workflow deletes old assets before uploading new ones
- Harden macOS CI with Ninja generator

## [0.2.1] — 2026-04-02

### Fixed

- Fix constructor initializer order in Engine (Werror reorder)
- Add VolatilityEstimator::get_regime_duration_blocks() method
- Fix IndentationError in gui/widgets/chart.py
- Fix IndentationError in gui/widgets/main_window.py

## [0.2.0] — 2026-04-02

### Changed

- Version bump to 0.2.0

## [0.1.9] — 2026-04-01

### Changed

- Version bump to 0.1.9

## [0.1.7] — 2026-03-31

### Changed

- Version bump to 0.1.7

## [0.1.6] — 2026-03-31

### Changed

- Version bump to 0.1.6

## [0.1.5] — 2026-03-31

### Fixed

- Fixed `AttributeError` in `MetricsService`: use `Qt.TimerType.CoarseTimer` instead of `QTimer.TimerType`

### Changed

- Version bump to 0.1.5

## [0.1.4] — 2026-03-31

### Changed

- Version bump to 0.1.4

## [0.1.3] — 2026-03-28

### Changed

- Version bump to 0.1.3

## [0.1.2] — 2026-03-26

### Changed

- Version bump to 0.1.2

## [0.1.1] — 2026-03-26

### Fixed

- Fill-rate feedback loop: replaced hardcoded `fill_rate_24h = 0.30` and `fill_rate_per_block = 0.03` with DB-computed values from offer_log history
- Telegram alert HTML injection: added entity escaping (`&<>"`) in `post_telegram()` including unsafe fallback path
- SQLite diagnostic queries: `trade_count()`, `offer_count()`, `snapshot_count()` now check `sqlite3_step()` return values
- FetchContent supply-chain: pinned nlohmann_json, spdlog, yaml-cpp to commit SHAs instead of mutable tags
- Desktop file `Exec` path corrected for Linux packaging

### Added

- Configurable `offer_fee_mojos` in `StrategyConfig` (was hardcoded 100M mojos across 5 call sites)
- Link-Time Optimization for Release builds via `CheckIPOSupported`
- `ctest` step in CI workflow — tests now gate artifact upload
- Linux `uninstall.sh` with `--purge` option, bundled in release tarball
- `CHANGELOG.md`
- Release workflow triggers on GitHub UI release publish (+ concurrency guard)

### Changed

- TODO.md summary table updated (84/121 items complete)

## [0.1.0] — 2026-03-25

Initial release of the XOPTrader CHIA DEX market-making engine.

### Engine

- 13-step per-block heartbeat orchestration engine with Boost.Asio coroutines
- Avellaneda-Stoikov and GLFT market-making strategy implementations
- 4-component spread optimizer (adverse selection, inventory, cost basis, competition)
- Multi-tier offer ladder (configurable tiers, spacing, size allocation)
- Yang-Zhang volatility estimator with Bayesian PIN adverse-selection model
- HMM + variance-ratio regime detection (mean-reverting, random, momentum)
- Order book tactician with Thompson Sampling strategy selection
- Competitor detection and response (own-offer filtering, spread tracking, alerts)
- Whale trader detection with configurable thresholds
- CHIA structural edge multiplier (settlement speed, no-counterparty-risk, etc.)
- Strategic Loss Manager with EV-based rebalance decisions
- Arbitrage scanner (CEX-DEX, cross-DEX, triangular, cross-bridge)
- 7-layer hedging framework with Natural Hedge Efficiency tracking
- Backtesting framework with walk-forward window support

### Connectivity

- Chia full node RPC client (mTLS, port 8555) for block data and coin records
- Chia wallet RPC client (mTLS, port 9256) for offer lifecycle management
- dexie.space API client with rate-limited coroutine interface
- Per-request CURL handles with RAII wrappers for thread safety
- Configurable SSL verification with Chia CA cert support

### Risk Management

- Inventory tracking with mark-to-market concentration limits
- Soft/hard inventory limits with graduated proportional sizing
- Half-Kelly position sizing with division-by-zero guards
- Max-drawdown global circuit breaker (10% default)
- Flash-crash state machine (Normal → Crash → Recovery → Normal)
- Crowding recovery mechanism with cooldown and geometric decay
- Per-pair strategy instances (no shared mutable state)
- Configurable `max_half_spread_bps` cap preventing market withdrawal
- Configurable `offer_fee_mojos` for on-chain fee management

### Data Integrity

- SQLite persistence for trades, offers, and analytics snapshots
- `sqlite3_step()` return value checking on all diagnostic queries
- Fill-rate feedback loop computing rates from offer_log history
- Trade log timestamps populated from fill data
- Proper SHA-256 coin name computation via OpenSSL EVP
- `std::llround()` for all mojo price conversions (no truncation bias)
- Inventory units converted from mojos to base-asset display units
- Crossed-book data validation before ingestion

### Observability

- 24 Prometheus metrics with cardinality-guarded label sets
- 14 Telegram alert rules with HTML entity escaping
- Structured logging via spdlog with secrets redacted
- PnL attribution (spread / inventory / fee components)
- Tax CSV export with acquisition timestamps

### Build & Packaging

- CMake 3.24+ build system with vcpkg manifest mode
- C++20 with coroutine support (MSVC/GCC/Clang)
- Compiler hardening: `-Wall -Wextra -Werror`, stack protector, FORTIFY_SOURCE, RELRO
- Link-Time Optimization for Release builds via `CheckIPOSupported`
- FetchContent dependencies pinned to commit SHAs (nlohmann_json, spdlog, yaml-cpp)
- GitHub Actions CI/CD with 3-platform builds and artifact upload
- Tests run via `ctest` in CI before artifact creation
- Python GUI via PySide6 + pyqtgraph with PyInstaller packaging
- Windows Inno Setup installer with optional desktop shortcut
- Linux install bundle with `.desktop` file and uninstall script
- `pyproject.toml` with build backend, upper-bounded dependencies, `requires-python >=3.11,<4`

### Configuration

- YAML-based configuration with validation and error messages
- `config.example.yaml` reference with full 64-char asset ID placeholders
- GUI error handling for backend initialization failures

### Tests

- 81 unit tests across 8 test files (Google Test)
- Avellaneda-Stoikov math, spread optimizer, inventory/risk,
  volatility, regime detection, competitor detection, whale detection,
  and advanced trading methods

### Academic Rigor (Counter-Research Validation)

- VPIN validation gate with rolling-window precision tracking
- Exponential-decay tau for Avellaneda-Stoikov/GLFT
- Variance ratio Z-statistic significance gating
- Discounted Thompson Sampling for non-stationary rewards
- Sparse-fill correction for GLFT intensity estimation
- Fill-count dampening for Brock-Hommes heterogeneous agent model

### Known Limitations

- CEX reference prices not yet integrated (Phase 2)
- PreTradeCheck, GLFT, and config parsing test suites incomplete
- vcpkg baseline dated 2024-09-30
- No code signing for release binaries
