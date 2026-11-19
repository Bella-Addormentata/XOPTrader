# Options: first live run

`gui/services/options` writes option contracts (CHIP-0045) from a Chia reference
wallet. Before this checklist it had been run on a simulated chain, against
chia-blockchain 2.7.5's own signer code, and read-only against mainnet. It had
not been run against a live wallet daemon and had broadcast nothing.

This checklist is that first run. It uses dust amounts on a throwaway key and
stops at the first thing that does not match.

It was first followed on 2026-11-15, on Windows against a 2.7.4 wallet daemon on
mainnet, using the trading wallet with the engine stopped
(`options.wallet.share_trading_wallet`). Stage 2's `live-test` passed: all
three spends confirmed and the wallet lost exactly the fees. The book then ran
live: its mints confirmed and xchoptions.com accepted and listed its offers.
Still to be seen live: another wallet taking an offer, an exercise, and a
clawback after expiry.

## What you need

- chia-blockchain 2.4.3 or newer (the signer RPCs it relies on arrived there).
- A second wallet daemon with its own key. One daemon serves one key at a time,
  so it cannot share the trading wallet's daemon. Give it its own `CHIA_ROOT`
  and its own ports, start only the wallet service, and let it sync.
  - Or, on a machine where a second daemon and key are not wanted, the trading
    wallet itself with `options.wallet.share_trading_wallet: true` and the
    trading engine **stopped** for as long as any contract is open. See the
    comment beside it in `config.example.yaml`. `status` and every step then
    print a warning and whether the engine is up, and every signature and push
    is refused while it is (or while that cannot be told), checked afresh
    each time. The wallet must be on this machine, since that is all the
    check can see, and `options.wallet.fingerprint` and
    `chia.wallet_fingerprint` must both be set and equal. `options.wallet.address` is required then:
    the wallet's current address is where the engine's self-sends land, and
    on the first mainnet run the engine spent the coin there within minutes.
    An address a Dexie trade paid into works, and needs nothing sent first.
    With `options.wallet.fund_from_wallet: true` the book also pays from the
    rest of the wallet when that address runs short, in XCH or the quote
    asset, while everything it owns still lands at the address. On the first
    run the engine's resting offers took the address's coins, and a cancel of
    a call struck in wUSDC.b also needs a wUSDC.b coin to start from, which
    the address had never held.
- That wallet's RPC port, under `options.wallet` in `config.yaml`, and its
  `config/ssl/wallet/private_wallet.crt` and `.key`, under `options.wallet` in
  `secrets.yaml` (see `secrets.example.yaml`; `~` is expanded). `secrets.yaml`
  is laid over `config.yaml` as everywhere else in the app, and a Settings
  save in the GUI keeps these keys there.
  Enter the key's `fingerprint` there too: the wallet is logged in to it before
  every step, and nothing is done while the wallet reports itself still syncing.
  `status` prints whether it is ready.
- A few thousand mojos of XCH sent to the address `status` prints. Only coins
  at that one address are used. With `options.wallet.address` blank the book
  asks the wallet for its current address once and keeps to it from then on,
  whatever the wallet's current address later becomes.
- Optional: a full node of your own. Enter its RPC port under `options.node`
  in `config.yaml`, and its `config/ssl/full_node/private_full_node.crt` and
  `.key` under `options.node` in `secrets.yaml`, and the chain is read from it;
  coinset.org is then never contacted. If the node is on another machine,
  `options.node.ca_cert_path` is required too, in `secrets.yaml` (the node's
  `config/ssl/ca/private_ca.crt`): the node's certificate is then
  checked against it, so nobody between the two can answer in its place. Without
  it the chain is read from coinset.org, sparingly (see "How often the chain is
  asked" below). `status` prints which of the two is in use.

## Stage 1: testnet11

Set `options.network: testnet11` and point `options.wallet` at a testnet11
wallet. Testnet XCH is free, so nothing is at risk.

1. `python -m gui.services.options --config config.yaml status`
   - Expect the options address and the spendable balance you funded.
   - A connection or certificate error here is a setup problem; fix it first.
2. `python -m gui.services.options --config config.yaml live-test`
   - It pauses before each of three spends (mint, withdrawal, cancel) and prints
     what the bundle does to the wallet. Read each line before typing `yes`.
   - **Mint.** Expect the wallet to lose the amount plus 1 mojo, and the same
     amount to appear as locked. Then: "read back from chain: live, owned by us:
     True".
   - **Offer.** An `offer1...` string is printed. Nothing was sent.
   - **Withdrawal.** Expect only the 1-mojo option coin to move. Then: "the
     offer is now: conflict".
   - **Cancel.** Expect the locked amount to return to the wallet.
   - Expect the final spendable balance to equal the starting one minus fees.
   - **If it stops part way** (you answered no, a spend did not confirm in
     time, the terminal closed): run `live-test` again. The contract and every
     bundle the test signed are written to `options.database_path` before they
     are sent, so the next run starts by reading the chain and finishing the
     earlier contract: it re-sends what was signed and never confirmed, cancels
     a contract that is still yours, and claws back one that has expired. Only
     then does it start a new test. `status` lists the contract as `live-test`.
3. Check the wallet itself.
   - Its balance agrees with the last line of the test.
   - Its log has no error or traceback from around the mint. (The wallet's own
     recognition code was run on a mint offline and dismissed it cleanly; the
     full sync loop meeting one on a live node is what this step checks.)
   - It shows no unknown asset, NFT, DID or pool wallet created by the test.

Stop here if anything differed. Write down the stage, the printed line and the
wallet's log.

## Stage 2: mainnet dust

Set `options.network: mainnet` and point `options.wallet` at a throwaway mainnet
key funded with dust. Repeat `status` and `live-test`. Then two things testnet
cannot show:

4. **The site accepts our offer.** At the offer stage, before continuing, paste
   the printed string into "Upload offer" on xchoptions.com.
   - Expect it to be listed with the contract's size, strike and expiry.
   - The test contract is XCH on both legs, so the site shows no per-XCH price
     for it; that is expected.
5. **Another wallet can take it.** Run `live-test` again and this time take the
   offer from another wallet (Sage has option support) before continuing.
   - The withdrawal stage then reports the offer was taken first and the test
     ends. The contract is sold and the sale is on record with what it paid.
     Its collateral goes to the buyer if they exercise. Otherwise run
     `live-test` again after the expiry time it prints (24 hours after the
     mint) and it claws the collateral back.

## Stage 3: the book, in dry run

6. Fill in `expiries`, the strike lists and every entry under `limits`. Leave
   `dry_run: true`.
7. `python -m gui.services.options --config config.yaml step`
   - Expect the market line (spot, volatility, Dexie range) and "dry run: would
     mint ...". A "not quoting" note names what was wrong with the price view.
8. `python -m gui.services.options --config config.yaml monitor`
   - Read-only. Expect the desk's listings with the site's price, ours, and the
     edge after hedging. Leave it running with `--interval 300` to build a
     record of what is listed and what gets taken.

## Going live

Set `dry_run: false` with small limits and run `step` by hand a few times
before `run`. `withdraw-all` takes every open offer off the market by spending
its coin. Stopping `run` is safe: offers already out expire on their own and
collateral waits to be reclaimed on the next `step` after expiry.

The **Options** tab in Settings sets the book up without editing config.yaml:
switching it on, the wallet (sharing the trading wallet needs only the options
address), the fence, the top-up target, the grid and the limits. It refuses to
save anything the book itself would refuse.

In the app, the Options page does the same without a terminal: **Rehearse**
steps the book as a dry run whatever `options.dry_run` says, **Live** steps it
for real after a confirmation, and **Off** stops it after the step in
progress. It reads the app's own `config.yaml`, so the options settings must be
in that file (and `secrets.yaml` beside it). With the trading wallet shared,
the engine will not start while the book has contracts open, and Live will not
switch on while the engine runs. Run the book either from the page or from a
terminal, not both: they share one database, and the second one to act is
refused ("another process is acting").

The **OPTIONS** switch in the toolbar, beside DEXIE, is a shortcut to the same
switch: on is Live, after the same confirmation, and off is Off; Rehearse is
only on the page. Its chip says what the book is doing, and why it is held or
refused. Settings > Advanced > Subsystems can hide it.

With `options.wallet.engine_beside_open_contracts: true` (and
`fund_from_wallet: true`, which it needs) the engine is held only while the
book acts, not for as long as contracts are open. A day with contracts out then
goes:

1. Stop the engine, switch the book to Live, and let it mint, list or cancel.
   A stop that keeps the engine's offers leaves their coins locked: if the
   book reports it cannot find a coin it needs (a cancel of a call struck in
   the quote asset needs one of that asset), stop the engine with its offers
   cancelled instead.
2. Switch the book Off once it has done what you wanted. If it sent anything,
   leave it Live (or on Rehearse) until its record shows the transaction
   settled: the engine waits for every transaction of the book's until a step
   has recorded its verdict from the chain, which a Rehearse step does too.
3. Start the engine. It trades beside the open contracts; collateral stays
   locked in them, and a sale or an exercise only adds to the wallet.
4. After expiry, repeat 1 and 2 to claw the collateral back.

The engine can spend the loose coins at the options address while it runs,
which is why the book must be able to pay from the rest of the wallet. At
launch the engine starts before the Options page takes up its remembered
mode, so a book left Live when the app closed comes back Live only if
something else still holds the engine; otherwise the page says it was not
resumed, and step 1 starts it again.

### Selling while the engine trades: the engine fence

`options.wallet.engine_fence: true` (engine v0.10.52 or later) turns the
options address into a sub-account that nothing but the book spends. The
engine excludes its coins from every wallet call that selects coins and from
its balances; the app's own sends do the same. The book then spends only from
its address, so `fund_from_wallet` must be off; set `options.target` to keep
the address funded. A first run:

1. Install the release with the fence. Set `engine_fence: true`,
   `fund_from_wallet: false` and a small `target` (say `xch: 1.5`,
   `quote: 2`), and restart the app so the engine starts with the fence.
2. Check the engine's metrics answer `xop_options_fence_enabled 1` and
   `xop_options_fence_fresh 1`, and its log says `[OPTIONS-FENCE] ON`.
3. Leave the book Off for a while with the engine trading. Its log should show
   no `[OPTIONS-FENCE]` refusals beyond the first heartbeat, and the coins at
   the options address should stay where they are.
4. Switch the book to Rehearse: it says what it would top up and mint. Then
   Live with one small contract: it tops up, mints and lists while the engine
   keeps trading.

If the engine's fence goes down (an older engine, the full node unreachable,
a stale refresh), the book stops signing beside it and says why, as before.

#### The fence that follows the book: `engine_fence: auto`

This is the default whenever `share_trading_wallet` is on (0.10.53 and later):
an unset `engine_fence` means `auto`, for the app and the engine alike, and
`engine_fence: false` keeps the older rules. Under the fence `fund_from_wallet`
and `engine_beside_open_contracts` are not used and are ignored with a warning,
so a config set up for 0.10.51 works as it is. Without `options.target` the
target comes from the limits: `max_locked_xch` plus one contract in XCH, and 2
units of the quote asset (plus `max_locked_quote` when puts are written); with
no XCH cap there is no target, and `target: false` turns top-ups off.

With `engine_fence: auto` (engine v0.10.53 or later) the fence is up only while
the book is in use: switched on (Rehearse or Live), a contract open, or a
transaction unsettled. The app keeps `options_fence.flag` beside the engine's
database for as long as that lasts (the folder the Dexie switch's `pause.flag`
goes in), and the engine follows it within seconds; its log says
`[OPTIONS-FENCE] UP` and `DOWN`. Once the book is off with nothing open, the
flag goes and the coins at the options address are trading funds again: Dexie
can use spare XCH and premiums there. The next time the book is switched on,
`target` tops the address back up.

Switching the book Live beside a running engine is no longer refused: the
flag goes up first, and each Live step waits until the engine reports the
fence up ("waiting for the trading engine to put its fence up"). If an engine
offer made while the fence was down still holds a coin at the options
address, the book also waits for that offer to be refreshed or to expire
(about 20 minutes at most). A first run is the same as above, with `auto` in
step 1; in step 2 the metrics read `xop_options_fence_enabled 0` until the
book is switched on, then `1`.

The flag names the process that raised it (`pid=` in the file), and every step
writes it again. The app lowers it only when no other live process raised it
last and no step is in progress, so a run from a terminal keeps it up; the
Options page checks again about once a minute while the book is off. If the
flag cannot be written, Live is refused at the switch and each Live step is
held until it can be.

Two things to know once contracts are out:

- **Setting `dry_run: true` again stops all spending**, housekeeping included.
  A dry-run `step` still reads the chain and keeps the record right, and says
  what it would do ("dry run: would claw back ..."), but it sends no clawback
  and re-sends nothing. Expired collateral is safe to leave: only its creator
  can move it. `withdraw-all` in dry run only lists what it would withdraw, so
  turn dry run off to use it. Offers already out stay takeable until they
  expire (15 minutes by default).
- **Run one process per database.** `step`, `run`, `withdraw-all` and
  `live-test` take a lock on `options.database_path`; a second one started
  beside it reports "another process is acting" and does nothing. `status` and
  `monitor` are not affected.

## Reviewing a run

Three records say what the book did and why, so a run can be reviewed without
having kept the terminal output:

- **The log file**, beside the database and named after it
  (`data/options_jobs.log` for `data/options_jobs.db`), with the time on every
  line: the market each step, every action and problem, and engine-guard
  changes. A run from the app's Options page writes it, as does every command
  but `status` and `report`. It is rotated at 2 MB with three old files kept.
  On the command line `--log-file PATH` puts it elsewhere and `--no-log-file`
  turns it off. A log file that would be the database, a file beside it, the
  config file or any other SQLite database is refused.
- **The database's journals.** `quotes` keeps, beside each offer, the spot,
  volatility, model value, carry, markup and minimum premium it was priced
  from, and which of them set the price. `step_log` keeps the market each step
  saw and its verdict: `quoting`, `not quoting: <reason>` (spot price too old,
  Dexie book too thin, crossed or too wide, spot outside the Dexie range,
  market data unavailable), `wallet not ready`, `step failed` (with the
  error), or `refused: trading engine may be running`. It writes a row when
  the verdict changes, when a price moves more than 0.5%, and otherwise every
  15 minutes. The `events` table carries the fee each confirmed transaction
  paid, and a sale names the offer that was taken.
- **`report`**, which reads the two:

  ```
  python -m gui.services.options --config config.yaml report --since 24h
  python -m gui.services.options --config config.yaml report --since 2026-11-15 --csv book.csv
  ```

  It prints how long the book quoted and what held it back (and for how long,
  in how many spells), how long it was not running, the offers made and how
  many sat on the minimum premium, and for each contract the premium a sale
  brought against the model value of the offer that was taken, the fees its
  transactions paid and how long its collateral was locked. A fee counts in
  the period its transaction was sent, once it has confirmed. `--since` takes a
  span (`90m`, `24h`, `7d`, `2w`), a date or time (UTC unless it says
  otherwise) or `all`. It only reads, so it is safe while `run` is going.
  `--csv` will not write over the database, the files beside it, the config
  file, the book's log files or any other SQLite database.
  Periods from before this release have no journal: they show contracts, fees
  and sales, but no quoting time or price breakdown.

`status` also says, for each unsold contract, whether it could be cancelled
now. A cancel of a call pays its strike to the options address inside the same
transaction, so it needs one coin of the quote asset to start from (any amount;
it comes back in full), and XCH for a fee when one is due: at the options
address, or anywhere in the wallet with `wallet.fund_from_wallet`. A fixed
`fee_mojos` is checked against that XCH; with `fee_mojos: auto` the fee is only
known when the cancel is sent. A contract it cannot cancel is clawed back after
expiry instead, and once expired a contract can only be clawed back.

## Announcing offers on Splash (optional)

[Splash!](https://github.com/dexie-space/splash) is Dexie's peer-to-peer
network for Chia offers. With `options.splash.enabled: true`, each offer the
book makes is also handed to a Splash daemon you run on the same machine, which
gossips it to its peers. It is off by default and independent of the upload to
the site.

1. Download the daemon for your system from the project's releases page
   (`splash-amd64.exe` on Windows) and check it against the `.sha256` file
   published beside it.
2. Start it with its offer-submission API on this machine only:

   ```bash
   splash --listen-offer-submission 127.0.0.1:4000
   ```

   Add `--testnet` when `options.network` is `testnet11`: the two chains have
   separate Splash networks, and the book cannot ask the daemon which it is on.
   It prints `Connected to peer: ...` as it finds peers. It needs outbound
   connections only.
3. Set `options.splash.enabled: true`. `status` then shows the address offers
   are handed to and how many of the live ones the daemon has taken.

What to expect:

- **The daemon's "yes" is not delivery.** It answers before it has published
  and says yes with no peers at all, so watch its own output for peers. An
  offer it refuses or cannot be reached for is reported by `step` ("not
  announced on Splash") and tried again on later steps while that offer
  stands. The offer is valid either way.
- **Nothing can be taken back.** As with the site, only spending the option
  coin ends an offer; `withdraw-all` does that.
- **The network has no memory.** A listener hears an offer only if it is
  connected when the offer is sent. Each offer is announced once; the book
  re-makes offers every 12 minutes by default, and each new one is announced.
- **Who shows what they hear is up to them.** Dexie indexes the network but is
  not known to show option offers, and the options site documents only its own
  upload. Treat this as extra reach where someone is listening, not as a
  replacement for the upload.
- The daemon's peers see its network address. The offers are public anyway.

Checked against the real daemon (0.3.0), two nodes on one machine: an offer
made by the book went in through one node and came out of the other's hook
unchanged. Not checked: the public network, and Windows.

## How often the chain is asked

The wallet does not track option coins, so the book reads them from a full
node: yours if `options.node` is set, otherwise coinset.org. It is built to ask
as little as possible.

- What is settled is asked for once. A spent coin and how it was spent are kept
  in `options.database_path` and, once a quarter of an hour old, never
  requested again, by this run or the next.
- What can still change is asked for together. Each contract has two coins
  worth watching (the option and its collateral); all of them, for every
  contract, go in one request.
- The book looks only when something may have happened: the wallet's own coins
  changed (a premium or a strike payment arrived, a transaction of ours
  confirmed), a transaction of ours is waiting (`chain_poll_seconds`), a
  contract just reached expiry, or `chain_refresh_seconds` have passed.
  Listing the wallet's coins is a call to your own wallet, not to the node.

Every `step` ends with a line such as `chain requests this step: 1 (routine)`,
so the cost is visible. For scale: a book of six open contracts stepped once a
minute used to make about 90,000 requests a day. It now makes 24 on a quiet
day (one an hour), about 7 when it mints a contract, and about 5 when one
sells. Each of those is followed, once, by 2 to 4 more at the first look after
the spends have settled: how each coin was spent is read again before it is
kept for good, because a reorganised block can hold a different spend at the
same height. Lowering `chain_refresh_seconds` from 3600 to 900 makes the quiet-day
figure 96 and shortens the wait described below to a quarter of an hour. A
transaction that runs out of time adds a few: from ten minutes after its
deadline the chain's clock is read at most once a minute (counted as three
requests each time, which is the most it can be) until it is seen to be past.

Two things follow from looking less often:

- Between looks the book acts on what the last look found. That cannot lose
  money, because the chain itself judges every spend: an offer for a contract
  already sold cannot be taken, and a cancel of one never confirms and is
  settled as overtaken at the next look. A sale is normally noticed within one
  step, because the premium arriving changes the wallet's coins. That needs the
  wallet to have a wallet for the quote asset (it creates one by itself when
  the first payment arrives, unless that has been switched off); without one a
  sale is noticed at the next routine look instead, up to an hour later, and
  its slot is refilled that much later.
- `run` is the mode that saves requests, because it remembers when it last
  looked. `step` run by hand, or from cron, is a new process every time and
  looks every time: two requests per open contract.

## Transactions that run out, withdrawals and fees

**Deadlines.** Every transaction carries a time after which the chain itself
refuses it. A mint, a cancel and a clawback get `confirm_timeout_seconds` (half
an hour); a withdrawal gets until the last offer it was sent to kill expires.
One that has not confirmed by then never will, so it is given up without
another transaction: its coins are free again, a lapsed mint is made again at
the current strike, a contract whose withdrawal or cancel lapsed goes back on
offer, and a lapsed clawback is signed again. `step` reports "did not confirm
in time". Nothing is given up on this machine's clock, nor on the newest
block's: the chain's clock is read 32 blocks below the peak (about ten
minutes), the depth the engine waits before retiring an expired offer, so a
reorganisation cannot bring the old transaction back beside its replacement.
Expect "did not confirm in time" roughly ten minutes after the deadline.

**Withdrawals.** Raising a price does not remove the cheaper offer already
out; only spending the option coin does, and that is a transaction. So it is
kept for when it matters and capped:

- the ask must have risen by `withdraw_pct` (2%) of the contract's notional;
- the stale offer must have more than `withdraw_min_life_seconds` (3 minutes)
  left, because a withdrawal could not confirm before a shorter one ran out;
- after a withdrawal the contract is not quoted for `withdraw_cooldown_seconds`
  (5 minutes), so a trending market cannot turn one into a run of them;
- the whole book sends at most `max_withdrawals_per_hour` (6). Once that is
  used up, a contract that would need one is not re-quoted until its stale
  offer has run out by itself, and `step` says so.

What is given up is bounded: a stale offer can be taken at its old price for
at most the rest of its life, 15 minutes by default.

**Fees.** `fee_mojos` is a fixed fee per transaction, or `auto`. With `auto`
each transaction is priced from its own cost, never above
`limits.max_fee_mojos`, at the larger of two rates:

- **What the mempool needs.** Nothing while the wallet's node estimates nothing
  (there is room). As soon as it shows any pressure, 5.5 mojos per unit of
  cost: the 5 a full mempool admits, plus a tenth. The node's estimate itself
  is not paid, because it runs far short of that. The engine's fee controller
  measured it 13 times short on this project's own wallet, and found that above
  the admission rate a spend confirms promptly whatever it paid, while below it
  it does not confirm at all.
- **What failure taught.** A mint, cancel or clawback of ours that ran out
  unconfirmed most likely paid too little, so the rate is raised to at least
  double what it paid (5.5 if it paid nothing). The raised rate is kept in the
  database, used for everything after, and halves every six hours without
  another failure. Because a lapsed transaction is always made again, this is
  also how a stuck one gets a better fee. `step` reports each raise, and
  `status` shows the rate. Two lapses teach nothing, because they say little
  about fees: a withdrawal's, which has only as long as the offer it kills,
  and any while the wallet cannot give an estimate at all (it has no node to
  ask). A lapse for some other reason, such as the wallet's node having been
  unreachable for a while, is still read as a low fee; what that costs is
  bounded by `limits.max_fee_mojos` and gone within a day.

A mint costs about 55 to 90 million cost units, a withdrawal about 35 to 45
million, a cancel about 90 to 170 million and a clawback 15 to 40 million. At
5.5 mojos per unit that is roughly 0.0003 to 0.0005 XCH for a mint and 0.0005
to 0.0009 for a cancel, which the example's cap of 0.001 XCH covers; `step`
says so when a fee is held down by the cap. Measured on mainnet on 2026-11-15:
a book mint about 55 million (fee 300,671,718 mojos), and the `live-test`
cancel 89.8 million. The mempool was 93 to 99% full that afternoon, so the
5.5 rate was the one that confirmed promptly. The estimate comes from the wallet
(`get_fee_estimate`, in chia-blockchain 2.7.5); an older wallet without it
gives none, and the book then goes by what failures taught it alone. This is a
small-scale form of what the engine's `fees:` controller does, not a share of
it: the two run in different processes on different wallets. `live-test` pays
one flat fee per spend, sized for its costliest.

## Known limits

- **Dedicated wallet only.** `wallet.mode: trading` is refused until the engine
  keeps option coins out of its own offers and books option events.
  `wallet.share_trading_wallet` is not that: it is the operator undertaking to
  keep the engine stopped, checked before every signature, and nothing more.
  The app will not start the engine while contracts are open (with
  `engine_beside_open_contracts`, while the book acts), but an engine started
  some other way, from a terminal or a service manager, is only reported; the
  engine then counts premiums and returned collateral as its own.
- **The wallet reports no verdict on a broadcast.** Outcomes are read from the
  chain, so a rejected transaction shows up as one that never confirms. It is
  pushed again on every step. While it is unresolved its coins stay set aside
  and are not used for anything new. Every transaction resolves by itself at
  its deadline (above). One signed before deadlines existed, or while the
  chain's time could not be read, is released only when the chain shows it
  confirmed or shows one of its coins spent some other way; to free the coins
  of one that will never confirm, send one of them to yourself from the wallet.
- **The price feed must say when its price was observed.** Spot comes from
  CoinGecko with its `last_updated_at` time, and `market.max_age_seconds` is
  measured from that. If the feed's price is older than the limit, or carries
  no time, the book does not quote. Run against the live feed on 2026-11-15:
  the time is there and the check passes. The price is asked for with
  `precision=full`; without it CoinGecko rounds to the cent (1.15 for 1.1512),
  which moved the book's spot in steps of about 0.9%. If a dry-run `step`
  reports the spot as too old, compare the age it prints with
  `max_age_seconds` before raising the limit.
- **No hedging.** The book steers its asks by the net delta of what it has
  sold. It places no spot trades.
- **No taking.** It writes and sells. It does not buy other makers' offers.
- **A clawed-back contract leaves its 1-mojo option coin behind** with whoever
  bought it.
