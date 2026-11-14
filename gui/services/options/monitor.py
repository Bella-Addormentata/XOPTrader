"""Read-only desk monitor: what other makers list, what it is worth, how it ends.

Nothing here signs or spends. Each :meth:`DeskMonitor.scan` reads the desk's
listing, and for every offer it has not seen before:

* reads the offer from its own spends and re-validates the option on chain, so
  a listing that names an unsound contract is recorded as a problem, not priced;
* asks the site for its suggested price;
* values the contract at our own spot and volatility, and again at the Dexie
  price a hedge would actually get -- selling XCH at the bid against a bought
  call, buying it at the ask against a bought put. The second figure minus the
  ask is what taking the offer would really be worth.

Offers that have left the listing are closed with what became of them, read
from the chain: taken, withdrawn (the option coin went elsewhere) or expired.

Over days this answers the two questions the business case rests on: how often
does a takeable price appear, and how much actually trades.
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
# An offer missing from the listing with no expiry of its own is given this long
# to turn out taken or withdrawn before it is written off as gone.
_UNDATED_GRACE_SECONDS = 3600


@dataclasses.dataclass
class ScanReport:
    listed: int = 0
    new: list[store.Listing] = dataclasses.field(default_factory=list)
    closed: dict[str, str] = dataclasses.field(default_factory=dict)  # offer id -> outcome
    problems: list[str] = dataclasses.field(default_factory=list)
    market_error: str = ""


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
    ) -> None:
        self._desk = desk_client
        self._chain = chain_reader
        self._market = market
        self._store = book_store
        self._quote = bytes(quote_asset_id)
        self._unit = int(quote_unit_mojos)
        self._rate = float(rate)
        self._additional_data = bytes(agg_sig_me_additional_data)
        self._clock = clock
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

    def _record(self, offer_id: str, now: int, view: Market | None, report: ScanReport) -> None:
        text = self._desk.fetch_offer(offer_id)
        if text is None:
            return  # gone between the listing and the fetch
        seen = desk.inspect_offer(text, agg_sig_me_additional_data=self._additional_data)
        if not seen.signature_valid:
            report.problems.append(f"{offer_id[:8]}: the maker's signature does not verify")
            return
        loaded = chain.load_option(self._chain, seen.launcher_id, now_seconds=now)
        if loaded.status is not chain.OptionStatus.LIVE:
            report.problems.append(f"{offer_id[:8]}: lists an option that is {loaded.status.value}")
            return
        terms = self._terms(loaded)
        side = size = strike = ask = our_value = hedged = site = None
        if terms is not None and seen.requested_asset_id == self._quote:
            side, size, strike = terms
            ask = seen.requested_amount / self._unit
            if view is not None:
                our_value, hedged = self._value(
                    side, size, strike, loaded.terms.expiration_seconds, now, view
                )
            try:
                site = self._desk.suggested_price(seen.launcher_id).premium
            except desk.DeskError as exc:
                report.problems.append(f"{offer_id[:8]}: no suggested price: {exc}")
        sdk = _sdk()
        bundle = sdk.Offer.from_spend_bundle(sdk.Clvm(), sdk.decode_offer(text)).spend_bundle()
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
            outcome=None,
            closed_at=None,
        )
        if self._store.record_listing(listing):
            report.new.append(listing)

    def _close(self, listing: store.Listing, now: int, report: ScanReport) -> None:
        coin_spends = list(_sdk().SpendBundle.from_bytes(listing.bundle).coin_spends)
        status = chain.bundle_status(self._chain, coin_spends)
        if status is chain.BundleStatus.CONFIRMED:
            outcome = TAKEN
        elif status is chain.BundleStatus.CONFLICT:
            outcome = WITHDRAWN
        elif listing.offer_expires is not None and listing.offer_expires <= now:
            outcome = EXPIRED
        elif listing.offer_expires is None and now - listing.last_seen > _UNDATED_GRACE_SECONDS:
            outcome = EXPIRED
        else:
            return  # delisted but still takeable; look again next scan
        self._store.close_listing(listing.offer_id, outcome, now)
        report.closed[listing.offer_id] = outcome

    def scan(self) -> ScanReport:
        now = int(self._clock())
        report = ScanReport()
        try:
            listing = self._desk.list_offers()
        except desk.DeskError as exc:
            # No listing means no conclusions: nothing is recorded and nothing closed.
            report.problems.append(f"could not read the desk: {exc}")
            return report
        report.listed = len(listing.offer_ids)

        view: Market | None
        try:
            view = self._market()
        except MarketError as exc:
            view = None
            report.market_error = str(exc)

        known = {item.offer_id: item for item in self._store.listings(open_only=True)}
        fresh = 0
        for offer_id in listing.offer_ids:
            if offer_id in known:
                self._store.record_listing(dataclasses.replace(known[offer_id], last_seen=now))
                continue
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
        return report
