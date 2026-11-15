"""Read-only desk monitor: what other makers list, what it is worth, how it ends.

Nothing here signs or spends. Each :meth:`DeskMonitor.scan` reads the desk's
listing, and for every offer it has not seen before:

* reads the offer from its own spends and re-validates the option on chain, so
  a listing that names an unsound contract is recorded as a problem, not priced;
* checks the offer can still be taken: it has not run out, the contract is
  live, and the coin it spends is the option's current coin. A listing that
  fails is noted with what became of it and never valued. A signature only
  proves who made an offer, not that it is still good;
* asks the site for its suggested price;
* values the contract at our own spot and volatility, and again at the Dexie
  price a hedge would actually get -- selling XCH at the bid against a bought
  call, buying it at the ask against a bought put. The second figure minus the
  ask is what taking the offer would really be worth.

Offers that have left the listing are closed with what became of them, read
from the chain: taken, withdrawn (the option coin went elsewhere) or expired.
Leaving the listing ends nothing by itself. An offer with no expiry of its own
stays takeable by anyone who kept the file, so it is followed until the chain
shows an outcome or the contract itself expires.

Over days this answers the two questions the business case rests on: how often
does a takeable price appear, and how much actually trades.

The chain is read through :class:`chain.CachedChain`, one look per scan. A new
listing costs the handful of requests it takes to validate its option, once.
After that a scan costs one request for all the delisted offers still being
followed, and none when there are none.
"""

from __future__ import annotations

import dataclasses
import logging
import time
from collections.abc import Callable
from typing import Any

from gui.services.options import chain, desk, pricing, signing, store
from gui.services.options.market import Market, MarketError

log = logging.getLogger(__name__)

TAKEN = "taken"
WITHDRAWN = "withdrawn"
EXPIRED = "expired"

#: An offer is called expired once the chain's clock, read this many blocks
#: below the peak, has passed its end (the depth the book uses for its own).
_EXPIRY_DEPTH_BLOCKS = 32
#: About how long that depth takes; the clock is not asked sooner than this
#: after an offer's end by our own clock.
_EXPIRY_AFTER_SECONDS = 600
#: ...nor more often than this. Each read costs the node up to three requests.
_EXPIRY_CLOCK_SECONDS = 900


@dataclasses.dataclass
class ScanReport:
    listed: int = 0
    new: list[store.Listing] = dataclasses.field(default_factory=list)
    closed: dict[str, str] = dataclasses.field(default_factory=dict)  # offer id -> outcome
    problems: list[str] = dataclasses.field(default_factory=list)
    market_error: str = ""
    chain_requests: int = 0  # requests this scan made of the chain's node


def _sdk() -> Any:
    import chia_wallet_sdk

    return chia_wallet_sdk


class DeskMonitor:
    def __init__(
        self,
        *,
        desk_client: Any,
        chain_reader: chain.ChainReader,
        market: Callable[[], Market],
        book_store: store.OptionStore,
        quote_asset_id: bytes,
        quote_unit_mojos: int,
        rate: float = 0.0,
        agg_sig_me_additional_data: bytes = signing.MAINNET_AGG_SIG_ME_ADDITIONAL_DATA,
        clock: Callable[[], float] = time.time,
        max_new_per_scan: int = 40,
        max_spot_age_seconds: float | None = None,
    ) -> None:
        self._desk = desk_client
        self._chain = chain.CachedChain.over(chain_reader, book_store, clock=clock)
        self._market = market
        self._store = book_store
        self._quote = bytes(quote_asset_id)
        self._unit = int(quote_unit_mojos)
        self._rate = float(rate)
        self._additional_data = bytes(agg_sig_me_additional_data)
        self._clock = clock
        self._max_spot_age = max_spot_age_seconds
        self._expiry_proof: int | None = None  # this scan: the time expiry is judged by
        self._clock_asked_at: int | None = None
        self._max_new = int(max_new_per_scan)

    def _terms(self, loaded: chain.LoadedOption) -> tuple[str, float, float] | None:
        """(side, size in XCH, strike per XCH) for an XCH/quote contract, else None."""
        underlying, strike = loaded.terms.underlying, loaded.terms.strike
        xch = 10**12
        if underlying.is_xch and strike.asset_id == self._quote:
            size = underlying.amount / xch
            return "call", size, (strike.amount / self._unit) / size
        if underlying.asset_id == self._quote and strike.is_xch:
            size = strike.amount / xch
            return "put", size, (underlying.amount / self._unit) / size
        return None

    def _value(
        self, side: str, size: float, strike: float, expiration: int, now: int, view: Market
    ) -> tuple[float | None, float | None]:
        if expiration <= now:
            return None, None
        years = (expiration - now) / pricing.SECONDS_PER_YEAR
        call = side == "call"

        def at(spot: float | None) -> float | None:
            if spot is None or spot <= 0:
                return None
            return pricing.american(spot, strike, years, self._rate, view.sigma, call=call) * size

        # A bought call is hedged by selling XCH (at the bid); a put by buying (at the ask).
        return at(view.spot), at(view.bid if call else view.ask)

    @staticmethod
    def _not_takeable(seen: desk.OfferView, loaded: chain.LoadedOption, now: int) -> str:
        """Why an offer can no longer be taken, or "" if it still can."""
        if loaded.status is not chain.OptionStatus.LIVE:
            return f"lists an option that is {loaded.status.value}"
        if seen.expires_at_seconds is not None and seen.expires_at_seconds <= now:
            return "the offer has run out"
        current = None if loaded.option is None else bytes(loaded.option.coin.coin_id())
        if current != bytes(seen.option_coin_id):
            return "offers an option coin that has already been spent"
        return ""

    def _outcome(self, coin_spends: list[Any]) -> str | None:
        """What the chain says became of an offer, or None while it is undecided."""
        status = chain.bundle_status(self._chain, coin_spends)
        if status is chain.BundleStatus.CONFIRMED:
            return TAKEN
        if status is chain.BundleStatus.CONFLICT:
            return WITHDRAWN
        return None

    def _record(self, offer_id: str, now: int, view: Market | None, report: ScanReport) -> None:
        text = self._desk.fetch_offer(offer_id)
        if text is None:
            return  # gone between the listing and the fetch
        seen = desk.inspect_offer(text, agg_sig_me_additional_data=self._additional_data)
        if not seen.signature_valid:
            report.problems.append(f"{offer_id[:8]}: the maker's signature does not verify")
            return
        loaded = chain.load_option(self._chain, seen.launcher_id, now_seconds=now)
        sdk = _sdk()
        bundle = sdk.Offer.from_spend_bundle(sdk.Clvm(), sdk.decode_offer(text)).spend_bundle()
        dead = self._not_takeable(seen, loaded, now)
        terms = self._terms(loaded)
        side = size = strike = ask = our_value = hedged = site = None
        if terms is not None and seen.requested_asset_id == self._quote:
            side, size, strike = terms
            ask = seen.requested_amount / self._unit
            if view is not None and not dead:
                our_value, hedged = self._value(
                    side, size, strike, loaded.terms.expiration_seconds, now, view
                )
            if not dead:
                try:
                    site = self._desk.suggested_price(seen.launcher_id).premium
                except desk.DeskError as exc:
                    report.problems.append(f"{offer_id[:8]}: no suggested price: {exc}")
        outcome = None
        if dead:
            # Kept, so it is not fetched again every scan, but closed at once
            # and never valued: it is not a price anybody can trade at.
            outcome = self._outcome(list(bundle.coin_spends))
            if outcome is None and self._expired(
                seen.expires_at_seconds, loaded.terms.expiration_seconds
            ):
                outcome = EXPIRED
            # Otherwise it is by this machine's clock only that it has run out.
            # It stays open, unvalued, and is judged again once it is delisted.
            report.problems.append(f"{offer_id[:8]}: {dead}")
        listing = store.Listing(
            offer_id=offer_id,
            launcher_id=seen.launcher_id,
            side=side,
            size_xch=size,
            strike=strike,
            expiration=loaded.terms.expiration_seconds,
            ask=ask,
            offer_expires=seen.expires_at_seconds,
            site_premium=site,
            our_value=our_value,
            hedged_value=hedged,
            bundle=bytes(bundle.to_bytes()),
            first_seen=now,
            last_seen=now,
            outcome=outcome,
            closed_at=None if outcome is None else now,
        )
        if self._store.record_listing(listing) and outcome is None:
            report.new.append(listing)

    def _close(self, listing: store.Listing, now: int, report: ScanReport) -> None:
        coin_spends = list(_sdk().SpendBundle.from_bytes(listing.bundle).coin_spends)
        outcome = self._outcome(coin_spends)
        if outcome is None:
            # The contract's own expiry ends an offer that carries none.
            if not self._expired(listing.offer_expires, listing.expiration):
                return  # delisted but still takeable; look again next scan
            outcome = EXPIRED
        self._store.close_listing(listing.offer_id, outcome, now)
        report.closed[listing.offer_id] = outcome

    def _expired(self, offer_expires: int | None, expiration: int | None) -> bool:
        """Whether an offer is out of time by the clock that decides it.

        An offer's end is enforced by the chain against the chain's own clock,
        which runs behind this machine's. Calling one expired by local time
        closes it for good while it can still be taken, and its fill is then
        never seen. So the verdict waits for :meth:`_prove_expiry`.
        """
        proof = self._expiry_proof
        if proof is None:
            return False
        ends = [end for end in (offer_expires, expiration) if end is not None]
        return any(end <= proof for end in ends)

    def _prove_expiry(self, now: int, delisted: list[store.Listing]) -> None:
        """Read the chain's clock, if some delisted offer may have run out.

        Read 32 blocks below the peak, as the book does for its own
        transactions, and BEFORE the look that finds the offer's coin unspent:
        only in that order is "unspent" proof that it expired untaken. Asked
        no more than once a quarter of an hour, and not before an offer is ten
        minutes past its end by our clock, which is about when that deep clock
        gets there. A reader with no clock to ask leaves this machine's.
        """
        self._expiry_proof = None
        if not self._chain.tells_time:
            self._expiry_proof = now
            return
        ends = [
            min(end for end in (item.offer_expires, item.expiration) if end is not None)
            for item in delisted
            if item.offer_expires is not None or item.expiration is not None
        ]
        due = any(end + _EXPIRY_AFTER_SECONDS <= now for end in ends)
        asked = self._clock_asked_at
        if not due or (asked is not None and now - asked < _EXPIRY_CLOCK_SECONDS):
            return
        self._clock_asked_at = now
        try:
            self._expiry_proof = self._chain.chain_time(_EXPIRY_DEPTH_BLOCKS)
        except chain.ChainError:
            self._expiry_proof = None  # undecided this scan; asked again later

    def scan(self) -> ScanReport:
        report = ScanReport()
        asked = self._chain.requests
        self._scan(report)
        report.chain_requests = self._chain.requests - asked
        return report

    def _scan(self, report: ScanReport) -> None:
        now = int(self._clock())
        try:
            listing = self._desk.list_offers()
        except desk.DeskError as exc:
            # No listing means no conclusions: nothing is recorded and nothing closed.
            report.problems.append(f"could not read the desk: {exc}")
            return
        report.listed = len(listing.offer_ids)

        view: Market | None
        try:
            view = self._market()
        except MarketError as exc:
            view = None
            report.market_error = str(exc)
        else:
            # A feed can hand over, freshly fetched, a price observed hours
            # ago. Valuing offers from it would record them as mispriced.
            age = now - view.as_of
            if self._max_spot_age is not None and age > self._max_spot_age:
                view = None
                report.market_error = f"the spot price is {int(age)} s old"

        known = {item.offer_id: item for item in self._store.listings(open_only=True)}
        judged = self._store.closed_listing_ids()
        gone = set(listing.offer_ids)
        self._prove_expiry(now, [item for key, item in known.items() if key not in gone])
        self._chain.begin()  # after the clock: see _prove_expiry
        fresh = 0
        for offer_id in listing.offer_ids:
            if offer_id in known:
                self._store.record_listing(dataclasses.replace(known[offer_id], last_seen=now))
                continue
            if offer_id in judged:
                continue  # still listed, already closed with an outcome
            if fresh >= self._max_new:
                break
            fresh += 1
            try:
                self._record(offer_id, now, view, report)
            except desk.DeskRateLimitedError as exc:
                report.problems.append(f"paused: {exc}")
                break
            except (desk.DeskError, chain.ChainError, chain.InvalidOptionError) as exc:
                report.problems.append(f"{offer_id[:8]}: {exc}")

        listed = set(listing.offer_ids)
        for offer_id, item in known.items():
            if offer_id in listed:
                continue
            try:
                self._close(item, now, report)
            except chain.ChainError as exc:
                report.problems.append(f"{offer_id[:8]}: {exc}")
