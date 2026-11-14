"""Parsing the ``options:`` section: off by default, caps that fail closed."""

from __future__ import annotations

import pytest

from gui.services.options import config

USD = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"


def _section(**overrides) -> dict:
    section = {
        "enabled": True,
        "wallet": {"port": 9356, "cert_path": "/w/private_wallet.crt", "key_path": "/w/k.key"},
        "quote_asset_id": USD,
        "contract_xch": 20,
        "expiries": ["2026-12-01T13:00:00Z"],
        "call_strikes_pct": [100, 110],
        "put_strikes_pct": [90],
        "limits": {
            "max_locked_xch": 100,
            "max_locked_quote": 150.5,
            "max_contracts": 10,
            "max_contracts_per_expiry": 5,
        },
    }
    section.update(overrides)
    return section


def _parse(**overrides) -> config.OptionsConfig:
    parsed = config.parse({"options": _section(**overrides)})
    assert parsed is not None
    return parsed


@pytest.mark.parametrize(
    "root", [None, {}, {"options": None}, {"options": {"enabled": False}}, {"options": {}}, "x"]
)
def test_absent_or_disabled_means_off(root) -> None:
    assert config.parse(root) is None


def test_defaults_are_conservative() -> None:
    parsed = _parse()

    assert parsed.dry_run is True
    assert parsed.network == config.MAINNET and parsed.address_prefix == "xch"
    assert parsed.fee_mojos == 0
    assert parsed.offer_ttl_seconds == 900
    assert parsed.market.require_book is True
    assert parsed.quote_asset_id.hex() == USD
    assert parsed.quote_unit_mojos == 1000
    assert parsed.contract_xch_mojos == 20 * config.XCH_MOJOS
    assert parsed.expiries == (1796130000,)
    assert parsed.database_path == "data/options_jobs.db"


def test_limits_are_scaled_and_floored() -> None:
    limits = _parse().limits
    assert limits.max_locked_xch == 100 * config.XCH_MOJOS
    assert limits.max_locked_quote == 150_500
    assert (limits.max_contracts, limits.max_contracts_per_expiry) == (10, 5)
    # A cap never rounds up past what was written.
    floored = _parse(limits={**_section()["limits"], "max_locked_quote": 1.2349})
    assert floored.limits.max_locked_quote == 1234


def test_a_missing_cap_is_unset_not_unlimited() -> None:
    limits = _parse(limits={"max_contracts": 3}).limits
    assert limits.max_locked_xch == config.UNSET
    assert limits.max_locked_quote == config.UNSET
    assert limits.max_contracts == 3
    explicit = _parse(limits={**_section()["limits"], "max_locked_xch": "unlimited"})
    assert explicit.limits.max_locked_xch == config.UNLIMITED


@pytest.mark.parametrize("bad", [0, -5, "lots", True, float("inf")])
def test_a_bad_cap_is_refused(bad) -> None:
    with pytest.raises(config.OptionsConfigError, match="max_locked_xch"):
        _parse(limits={**_section()["limits"], "max_locked_xch": bad})


def test_trading_wallet_is_refused_with_the_reason() -> None:
    with pytest.raises(config.OptionsConfigError, match="not supported yet"):
        _parse(wallet={**_section()["wallet"], "mode": "trading"})


def test_dedicated_wallet_must_not_be_the_trading_wallet() -> None:
    root = {
        "chia": {"wallet_host": "localhost", "wallet_port": 9256},
        "options": _section(wallet={**_section()["wallet"], "port": 9256, "host": "127.0.0.1"}),
    }
    with pytest.raises(config.OptionsConfigError, match="trading wallet's RPC port"):
        config.parse(root)
    root["options"]["wallet"]["port"] = 9356
    assert config.parse(root) is not None


@pytest.mark.parametrize(
    "wallet",
    [
        {"cert_path": "c", "key_path": "k"},
        {"port": 9356, "key_path": "k"},
        {"port": 9356, "cert_path": "c"},
        {"port": 99999, "cert_path": "c", "key_path": "k"},
        {"port": "x", "cert_path": "c", "key_path": "k"},
    ],
)
def test_wallet_connection_details_are_required(wallet) -> None:
    with pytest.raises(config.OptionsConfigError):
        _parse(wallet=wallet)


def test_expiries_accept_iso_and_unix_and_need_a_zone() -> None:
    parsed = _parse(expiries=["2026-12-31T13:00:00+00:00", 1796130000, "2026-12-01T13:00:00Z"])
    assert parsed.expiries == (1796130000, 1798722000)
    with pytest.raises(config.OptionsConfigError, match="time zone"):
        _parse(expiries=["2026-12-01T13:00:00"])
    with pytest.raises(config.OptionsConfigError, match="ISO-8601"):
        _parse(expiries=["next friday"])


@pytest.mark.parametrize(
    "overrides",
    [
        {"quote_asset_id": "abc"},
        {"quote_asset_id": None},
        {"contract_xch": 0},
        {"contract_xch": None},
        {"network": "devnet"},
        {"call_strikes_pct": [110, 110]},
        {"call_strikes_pct": [0]},
        {"put_strikes_pct": "90"},
        {"strike_step": 0},
        {"offer_ttl_seconds": 30},
        {"offer_ttl_seconds": 300, "refresh_before_seconds": 300},
        {"fee_mojos": 5},
        {"pricing": {"lock_rate": -1}},
        {"pricing": "cheap"},
        {"market": {"max_spread_pct": 0}},
    ],
)
def test_bad_values_are_refused(overrides) -> None:
    with pytest.raises(config.OptionsConfigError):
        _parse(**overrides)


def test_fee_needs_an_allowance() -> None:
    parsed = _parse(fee_mojos=5, limits={**_section()["limits"], "max_fee_mojos": 10})
    assert parsed.fee_mojos == 5 and parsed.limits.max_fee_mojos == 10


def test_the_chain_is_read_from_coinset_unless_a_node_is_given() -> None:
    parsed = _parse()
    assert not parsed.node.enabled and (parsed.node.host, parsed.node.port) == ("localhost", 8555)
    # Look every half minute while something is waiting; otherwise once an hour.
    assert (parsed.chain_poll_seconds, parsed.chain_refresh_seconds) == (30, 3600)

    own = _parse(
        node={"host": "10.0.0.5", "port": 8666, "cert_path": "/n/c.crt", "key_path": "/n/k"}
    )
    assert own.node.enabled
    assert (own.node.host, own.node.port, own.node.cert_path) == ("10.0.0.5", 8666, "/n/c.crt")


@pytest.mark.parametrize(
    "overrides, message",
    [
        ({"node": {"cert_path": "/n/c.crt"}}, "both cert_path and key_path"),
        ({"node": {"key_path": "/n/k.key"}}, "both cert_path and key_path"),
        ({"node": "localhost:8555"}, "options.node must be a mapping"),
        ({"node": {"port": 0, "cert_path": "c", "key_path": "k"}}, "port"),
        ({"chain_poll_seconds": 600, "chain_refresh_seconds": 60}, "must not be shorter"),
        ({"chain_poll_seconds": -1}, "chain_poll_seconds"),
    ],
)
def test_bad_chain_settings_are_refused(overrides, message) -> None:
    with pytest.raises(config.OptionsConfigError, match=message):
        _parse(**overrides)


def test_fees_are_fixed_or_follow_the_wallet_s_estimate_under_the_cap() -> None:
    assert (_parse().fee_mojos, _parse().fee_auto) == (0, False)

    fixed = _parse(fee_mojos=500, limits={**_section()["limits"], "max_fee_mojos": 1000})
    assert (fixed.fee_mojos, fixed.fee_auto) == (500, False)

    auto = _parse(fee_mojos="auto", limits={**_section()["limits"], "max_fee_mojos": 1000})
    assert (auto.fee_mojos, auto.fee_auto, auto.limits.max_fee_mojos) == (0, True, 1000)
    # With no cap, "auto" can only ever mean no fee, which is allowed.
    assert _parse(fee_mojos=" Auto ").fee_auto


def test_withdrawals_are_reined_in_by_default() -> None:
    parsed = _parse()
    assert parsed.withdraw_pct == 2.0
    assert parsed.withdraw_min_life_seconds == 180
    assert parsed.withdraw_cooldown_seconds == 300
    assert parsed.max_withdrawals_per_hour == 6
    assert _parse(max_withdrawals_per_hour=0).max_withdrawals_per_hour == 0
    with pytest.raises(config.OptionsConfigError, match="max_withdrawals_per_hour"):
        _parse(max_withdrawals_per_hour=-1)


def test_testnet_uses_testnet_addresses() -> None:
    assert _parse(network="testnet11").address_prefix == "txch"


@pytest.mark.parametrize("raw", ["false", "no", "off", "true", 0, 1, "0"])
@pytest.mark.parametrize("key", ["enabled", "dry_run", "upload_offers"])
def test_a_yes_or_no_setting_must_be_a_real_true_or_false(key, raw) -> None:
    # bool("false") is True: a quoted "false" used to switch the book ON.
    with pytest.raises(config.OptionsConfigError, match=f"{key} must be true or false"):
        config.parse({"options": _section(**{key: raw})})


def test_a_quoted_false_does_not_turn_the_order_book_check_off_or_on() -> None:
    with pytest.raises(config.OptionsConfigError, match="require_book must be true or false"):
        _parse(market={"require_book": "false"})
    assert _parse(market={"require_book": False}).market.require_book is False
    assert _parse().market.require_book is True


def test_yes_or_no_settings_left_empty_take_their_defaults() -> None:
    parsed = _parse(dry_run=None, upload_offers=None)
    assert parsed.dry_run is True and parsed.upload_offers is True
    assert config.parse({"options": {"enabled": None}}) is None


def test_the_app_s_coingecko_key_is_carried_but_never_printed() -> None:
    root = {"options": _section(), "coingecko": {"api_key": " CG-secret "}}
    parsed = config.parse(root)
    assert parsed is not None and parsed.coingecko_api_key == "CG-secret"
    assert "CG-secret" not in repr(parsed)
    assert _parse().coingecko_api_key == ""
