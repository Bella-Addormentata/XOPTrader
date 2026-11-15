"""The xchoptions.com client against a scripted site, and offer inspection."""

from __future__ import annotations

import json

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder, desk  # noqa: E402
from gui.services.options.tests.sim_world import ADDITIONAL_DATA, CAT, XCH, World  # noqa: E402

OFFER_A = "6RRvdQrieg96VbMv5BRxgYGCD1baJX3dm3pANdZdZBq2"
OFFER_B = "B8Zb6yMH1zSTu3HtvZA8WJseFHQFyCsrGQMmsRwos39n"
LAUNCHER = "45d11aa62ba33c166ef173963d6e448bb00de4b92fa98fc1bb84daff28330401"


def _page(first: int, last: int, total: int, links: list[str]) -> str:
    rows = "".join(
        f'<tr><td><a href="https://xchoptions.com{link}">View</a></td></tr>' for link in links
    )
    return (
        "<html><body><h1>XCH Options</h1><p>Active options <span>"
        f"{first}–{last}</span> of <b>{total}</b></p><table>{rows}</table></body></html>"
    )


class Site:
    def __init__(self) -> None:
        self.pages: dict[str, tuple[int, dict, str]] = {}
        self.calls: list[tuple[str, str, dict | None]] = []

    def __call__(self, method: str, url: str, body: dict | None) -> tuple[int, dict, str]:
        self.calls.append((method, url, body))
        path = url.replace("https://xchoptions.com", "")
        return self.pages.get(
            (method + " " + path), (404, {}, json.dumps({"message": "Not found"}))
        )


class Clock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now


def _client(site: Site, clock: Clock | None = None, **kwargs) -> desk.DeskClient:
    return desk.DeskClient(site, clock=clock or Clock(), **kwargs)


def test_listing_reads_offers_and_unoffered_options() -> None:
    site = Site()
    site.pages["GET /options"] = (
        200,
        {},
        _page(1, 3, 3, [f"/offers/{OFFER_A}", f"/offers/{OFFER_B}", f"/option/{LAUNCHER}"]),
    )

    listing = _client(site).list_offers()

    assert listing.offer_ids == (OFFER_A, OFFER_B)
    assert listing.unoffered_launcher_ids == (bytes.fromhex(LAUNCHER),)
    assert listing.total == 3


def test_listing_follows_pages() -> None:
    site = Site()
    site.pages["GET /options"] = (200, {}, _page(1, 1, 2, [f"/offers/{OFFER_A}"]))
    site.pages["GET /options?page=2"] = (200, {}, _page(2, 2, 2, [f"/offers/{OFFER_B}"]))

    assert _client(site).list_offers().offer_ids == (OFFER_A, OFFER_B)


def test_an_unrecognised_page_is_an_error_not_an_empty_market() -> None:
    site = Site()
    site.pages["GET /options"] = (200, {}, "<html><body>We redesigned everything</body></html>")
    with pytest.raises(desk.DeskLayoutError):
        _client(site).list_offers()


def test_rows_without_links_are_a_layout_error() -> None:
    site = Site()
    site.pages["GET /options"] = (200, {}, _page(1, 2, 2, []))
    with pytest.raises(desk.DeskLayoutError):
        _client(site).list_offers()


def test_a_page_that_adds_nothing_is_a_layout_error() -> None:
    site = Site()
    site.pages["GET /options"] = (200, {}, _page(1, 1, 2, [f"/offers/{OFFER_A}"]))
    site.pages["GET /options?page=2"] = (200, {}, _page(1, 1, 2, [f"/offers/{OFFER_A}"]))
    with pytest.raises(desk.DeskLayoutError):
        _client(site).list_offers()


def test_an_explicitly_empty_desk_is_empty() -> None:
    site = Site()
    site.pages["GET /options"] = (200, {}, "<html><body><p>No options yet.</p></body></html>")
    assert _client(site).list_offers() == desk.Listing((), (), 0)


def test_fetch_offer_returns_the_string_or_none_when_gone() -> None:
    site = Site()
    site.pages[f"GET /api/offers/{OFFER_A}"] = (200, {}, json.dumps({"offer": "offer1abc"}))
    client = _client(site)

    assert client.fetch_offer(OFFER_A) == "offer1abc"
    assert client.fetch_offer(OFFER_B) is None
    with pytest.raises(desk.DeskError, match="malformed"):
        client.fetch_offer("../admin")


def test_upload_returns_the_sites_record() -> None:
    site = Site()
    site.pages["POST /api/offers"] = (
        200,
        {},
        json.dumps(
            {"offer": {"offerId": OFFER_A, "launcherId": LAUNCHER}, "option": {"created": True}}
        ),
    )

    upload = _client(site).upload_offer("offer1abc")

    assert upload == desk.Upload(OFFER_A, bytes.fromhex(LAUNCHER), True)
    assert site.calls[-1] == ("POST", "https://xchoptions.com/api/offers", {"offer": "offer1abc"})


def test_upload_refusal_carries_the_sites_reason() -> None:
    site = Site()
    site.pages["POST /api/offers"] = (
        422,
        {},
        json.dumps({"message": "That offer has already expired."}),
    )
    with pytest.raises(desk.DeskError, match="already expired"):
        _client(site).upload_offer("offer1abc")


def test_suggested_price_parses_basis_and_premium() -> None:
    site = Site()
    site.pages["POST /api/options/price"] = (
        200,
        {},
        json.dumps(
            {
                "basis": {"spot": 1.11, "sigma": 0.86, "riskFreeRate": 0.05},
                "suggested": {"primary": {"amount": "1.76", "ticker": "wUSDC.b"}},
            }
        ),
    )

    suggestion = _client(site).suggested_price(bytes.fromhex(LAUNCHER))

    assert suggestion == desk.Suggestion(True, 1.11, 0.86, 0.05, 1.76)
    assert site.calls[-1][2] == {"option_id": LAUNCHER}


def test_unpriceable_option_has_no_premium() -> None:
    site = Site()
    site.pages["POST /api/options/price"] = (
        200,
        {},
        json.dumps({"priceable": False, "basis": {"spot": 1.11}, "suggested": None}),
    )
    suggestion = _client(site).suggested_price(bytes.fromhex(LAUNCHER))
    assert suggestion.priceable is False and suggestion.premium is None


def test_own_budget_stops_calls_before_the_site_would() -> None:
    site = Site()
    site.pages[f"GET /api/offers/{OFFER_A}"] = (200, {}, json.dumps({"offer": "offer1abc"}))
    clock = Clock()
    client = _client(site, clock, reads_per_minute=2)
    client.fetch_offer(OFFER_A)
    client.fetch_offer(OFFER_A)

    with pytest.raises(desk.DeskRateLimitedError) as caught:
        client.fetch_offer(OFFER_A)

    assert 0 < caught.value.retry_after <= 60
    assert len(site.calls) == 2  # the third never left
    clock.now += 61
    assert client.fetch_offer(OFFER_A) == "offer1abc"


def test_a_429_backs_off_for_the_time_the_site_asks() -> None:
    site = Site()
    site.pages[f"GET /api/offers/{OFFER_A}"] = (429, {"Retry-After": "30"}, "{}")
    clock = Clock()
    client = _client(site, clock)
    with pytest.raises(desk.DeskRateLimitedError):
        client.fetch_offer(OFFER_A)
    site.pages[f"GET /api/offers/{OFFER_A}"] = (200, {}, json.dumps({"offer": "offer1abc"}))

    with pytest.raises(desk.DeskRateLimitedError):
        client.fetch_offer(OFFER_A)  # still inside the 30 seconds
    assert len(site.calls) == 1
    clock.now += 31
    assert client.fetch_offer(OFFER_A) == "offer1abc"


def test_nearly_spent_site_allowance_pauses_the_client() -> None:
    site = Site()
    site.pages[f"GET /api/offers/{OFFER_A}"] = (
        200,
        {"X-RateLimit-Remaining": "1"},
        json.dumps({"offer": "offer1abc"}),
    )
    client = _client(site)
    client.fetch_offer(OFFER_A)
    with pytest.raises(desk.DeskRateLimitedError):
        client.fetch_offer(OFFER_A)


def test_transport_failure_and_bad_json_are_desk_errors() -> None:
    def broken(method: str, url: str, body: dict | None) -> tuple[int, dict, str]:
        raise TimeoutError("timed out")

    with pytest.raises(desk.DeskError, match="timed out"):
        desk.DeskClient(broken).fetch_offer(OFFER_A)
    site = Site()
    site.pages[f"GET /api/offers/{OFFER_A}"] = (200, {}, "<html>maintenance</html>")
    with pytest.raises(desk.DeskError, match="non-JSON"):
        _client(site).fetch_offer(OFFER_A)


# ---------------------------------------------------------------------------
# inspect_offer
# ---------------------------------------------------------------------------


def _minted(world: World):
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH),
        builder.Asset(usd, 120 * CAT),
        world.now() + 3600,
        maker.puzzle_hash,
    )
    minted = builder.build_mint(terms, maker.funding(), maker.custody())
    world.push(minted.coin_spends)
    return maker, taker, usd, minted


def test_inspect_reads_an_offer_from_its_own_spends() -> None:
    world = World()
    maker, _, usd, minted = _minted(world)
    expires = world.now() + 900
    draft = builder.build_offer(
        minted.option,
        builder.Asset(usd, 2100),
        maker.puzzle_hash,
        maker.custody(),
        expires_at_seconds=expires,
    )
    offer = draft.encode(world.signer().sign(draft.coin_spends))

    view = desk.inspect_offer(offer, agg_sig_me_additional_data=ADDITIONAL_DATA)

    assert view.launcher_id == minted.launcher_id
    assert view.option_coin_id == bytes(minted.option.coin.coin_id())
    assert (view.requested_asset_id, view.requested_amount) == (usd, 2100)
    assert view.expires_at_seconds == expires
    assert view.signature_valid


def test_inspect_reports_an_xch_ask_and_no_expiry() -> None:
    world = World()
    maker, _, _, minted = _minted(world)
    draft = builder.build_offer(
        minted.option, builder.Asset(None, 2 * XCH), maker.puzzle_hash, maker.custody()
    )

    view = desk.inspect_offer(
        draft.encode(world.signer().sign(draft.coin_spends)),
        agg_sig_me_additional_data=ADDITIONAL_DATA,
    )

    assert (view.requested_asset_id, view.requested_amount) == (None, 2 * XCH)
    assert view.expires_at_seconds is None


def test_inspect_flags_an_offer_signed_for_another_network() -> None:
    world = World()
    maker, _, usd, minted = _minted(world)
    draft = builder.build_offer(
        minted.option, builder.Asset(usd, 2100), maker.puzzle_hash, maker.custody()
    )
    offer = draft.encode(world.signer().sign(draft.coin_spends))

    assert desk.inspect_offer(offer).signature_valid is False  # checked against mainnet


def test_inspect_refuses_what_is_not_an_option_offer() -> None:
    with pytest.raises(desk.DeskError):
        desk.inspect_offer("offer1notanoffer")
    with pytest.raises(desk.DeskError):
        desk.inspect_offer("hello")
