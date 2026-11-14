# Options: first live run

`gui/services/options` writes option contracts (CHIP-0045) from a Chia reference
wallet. Before this checklist it had been run on a simulated chain, against
chia-blockchain 2.7.5's own signer code, and read-only against mainnet. It had
not been run against a live wallet daemon and had broadcast nothing.

This checklist is that first run. It uses dust amounts on a throwaway key and
stops at the first thing that does not match.

## What you need

- chia-blockchain 2.4.3 or newer (the signer RPCs it relies on arrived there).
- A second wallet daemon with its own key. One daemon serves one key at a time,
  so it cannot share the trading wallet's daemon. Give it its own `CHIA_ROOT`
  and its own ports, start only the wallet service, and let it sync.
- That wallet's RPC port and its `config/ssl/wallet/private_wallet.crt` and
  `.key`, entered under `options.wallet` in `config.yaml`.
- A few thousand mojos of XCH sent to the address `status` prints. Only coins
  at that one address are used.
- Optional: a full node of your own. Enter its RPC port and its
  `config/ssl/full_node/private_full_node.crt` and `.key` under `options.node`
  and the chain is read from it; coinset.org is then never contacted. Without
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
minute used to make about 90,000 requests a day. It now makes about 100 on a
quiet day (one every fifteen minutes), about 7 when it mints a contract, and
about 5 when one sells. Raising `chain_refresh_seconds` to 3600 brings the
quiet-day figure to 24.

Two things follow from looking less often:

- Between looks the book acts on what the last look found. That cannot lose
  money, because the chain itself judges every spend: an offer for a contract
  already sold cannot be taken, and a cancel of one never confirms and is
  settled as overtaken at the next look. A sale is normally noticed within one
  step, because the premium arriving changes the wallet's coins. That needs the
  wallet to have a wallet for the quote asset (it creates one by itself when
  the first payment arrives, unless that has been switched off); without one a
  sale is noticed at the next routine look instead.
- `run` is the mode that saves requests, because it remembers when it last
  looked. `step` run by hand, or from cron, is a new process every time and
  looks every time: two requests per open contract.

## Known limits

- **Dedicated wallet only.** `wallet.mode: trading` is refused until the engine
  keeps option coins out of its own offers and books option events.
- **The wallet reports no verdict on a broadcast.** Outcomes are read from the
  chain, so a rejected transaction shows up as one that never confirms. It is
  pushed again on every step and flagged after `confirm_timeout_seconds`. The
  flag changes nothing else: the transaction is still signed and could still
  confirm, so its coins stay set aside and are not used for anything new. They
  are released when the chain shows it confirmed, or shows one of its coins
  spent some other way. To free the coins of a transaction that will never
  confirm, send one of them to yourself from the wallet.
- **The price feed must say when its price was observed.** Spot comes from
  CoinGecko with its `last_updated_at` time, and `market.max_age_seconds` is
  measured from that. If the feed's price is older than the limit, or carries
  no time, the book does not quote. This has only been run against scripted
  answers; if the first dry-run `step` reports the spot as too old, compare the
  age it prints with `max_age_seconds` before raising the limit.
- **No hedging.** The book steers its asks by the net delta of what it has
  sold. It places no spot trades.
- **No taking.** It writes and sells. It does not buy other makers' offers.
- **A clawed-back contract leaves its 1-mojo option coin behind** with whoever
  bought it.
