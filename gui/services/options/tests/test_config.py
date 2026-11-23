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
        {"contract_xch": -1},
        {"contract_xch": "big"},
        {"contract_xch": ""},
        {"contract_xch": True},
        {"contract_xch": float("inf")},
        {
            "limits": {
                "max_locked_xch": 1e300,
                "max_locked_quote": 1,
                "max_contracts": 1,
                "max_contracts_per_expiry": 1,
            }
        },
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
        node={
            "host": "10.0.0.5",
            "port": 8666,
            "cert_path": "/n/c.crt",
            "key_path": "/n/k",
            "ca_cert_path": "/n/ca.crt",
        }
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


def test_splash_is_off_unless_switched_on() -> None:
    assert _parse().splash == config.SplashConfig(False, "http://127.0.0.1:4000")
    on = _parse(splash={"enabled": True}).splash
    assert on.enabled and on.submit_url == "http://127.0.0.1:4000"
    there = _parse(splash={"enabled": True, "submit_url": " http://10.0.0.5:4000 "}).splash
    assert there.submit_url == "http://10.0.0.5:4000"


@pytest.mark.parametrize(
    ("raw", "why"),
    [
        ({"enabled": "true"}, "enabled must be true or false"),
        ({"enabled": True, "submit_url": "127.0.0.1:4000"}, "submit_url is not an http address"),
        ({"submit_url": "http://user:pw@host:4000"}, "must not carry a user name"),
        ("on", "options.splash must be a mapping"),
    ],
)
def test_a_bad_splash_section_is_refused(raw, why) -> None:
    with pytest.raises(config.OptionsConfigError, match=why):
        _parse(splash=raw)


def test_the_dedicated_wallet_must_not_be_the_trading_wallet_s_key() -> None:
    # A second daemon on another port passes the port check, but on the same
    # key it spends the very coins the engine's offers are made of.
    section = _section(wallet={"port": 9356, "cert_path": "c", "key_path": "k", "fingerprint": 77})
    for trading in (77, "77"):
        with pytest.raises(config.OptionsConfigError, match="the trading wallet's key"):
            config.parse({"options": section, "chia": {"wallet_fingerprint": trading}})
    parsed = config.parse({"options": section, "chia": {"wallet_fingerprint": 78}})
    assert parsed is not None and parsed.wallet.fingerprint == 77


def test_a_node_on_another_machine_needs_its_certificate_authority() -> None:
    pair = {"cert_path": "c", "key_path": "k"}
    for host in ("localhost", "127.0.0.1", "::1", "LOCALHOST"):
        assert _parse(node={"host": host, **pair}).node.ca_cert_path == ""
    with pytest.raises(config.OptionsConfigError, match="ca_cert_path is required"):
        _parse(node={"host": "10.0.0.9", **pair})
    far = _parse(node={"host": "10.0.0.9", "ca_cert_path": " /n/private_ca.crt ", **pair}).node
    assert far.ca_cert_path == "/n/private_ca.crt" and not far.local
    # With no node configured there is nothing to check.
    assert not _parse(node={"host": "10.0.0.9"}).node.enabled


def test_an_ipv6_host_in_brackets_is_still_this_machine() -> None:
    pair = {"cert_path": "c", "key_path": "k"}
    assert _parse(node={"host": "[::1]", **pair}).node.local
    assert _parse(node={"host": "::1", **pair}).node.local
    # ...and still the trading wallet's machine, for the port check.
    section = _section(wallet={"host": "[::1]", "port": 9256, "cert_path": "c", "key_path": "k"})
    with pytest.raises(config.OptionsConfigError, match="trading wallet's RPC port"):
        config.parse({"options": section, "chia": {"wallet_host": "::1", "wallet_port": 9256}})


def test_certificate_paths_have_their_home_expanded(monkeypatch) -> None:
    # The TLS libraries the paths go to do not expand ~ themselves.
    monkeypatch.setenv("HOME", "/home/someone")
    monkeypatch.setenv("USERPROFILE", "/home/someone")  # what Windows expands ~ from
    wallet = {"port": 9356, "cert_path": "~/w.crt", "key_path": " ~/w.key "}
    node = {"cert_path": "~/n.crt", "key_path": "/abs/n.key", "ca_cert_path": "~/ca.crt"}
    parsed = _parse(wallet=wallet, node=node)
    assert (parsed.wallet.cert_path, parsed.wallet.key_path) == (
        "/home/someone/w.crt",
        "/home/someone/w.key",
    )
    assert (parsed.node.cert_path, parsed.node.key_path, parsed.node.ca_cert_path) == (
        "/home/someone/n.crt",
        "/abs/n.key",
        "/home/someone/ca.crt",
    )
    assert _parse().node.ca_cert_path == ""


# -- [ROLLING] expiries that roll forward on their own -------------------------------

_FRI_0330 = 1795145400  # 2026-11-20T03:30:00Z, a Friday


def _utc(stamp: int) -> str:
    import datetime

    return datetime.datetime.fromtimestamp(stamp, tz=datetime.UTC).strftime("%a %Y-%m-%d %H:%M")


def test_weekly_is_the_next_two_fridays_at_eight_utc() -> None:
    parsed = _parse(expiries="weekly")
    assert parsed.expiries == () and parsed.expiry_rule == config.ExpiryRule(2, 0)
    assert [_utc(x) for x in parsed.expiry_rule.at(_FRI_0330)] == [
        "Fri 2026-11-20 08:00",
        "Fri 2026-11-27 08:00",
    ]
    # From the end of the stop window (six hours by default): two still quotable.
    assert [_utc(x) for x in parsed.expiries_at(_FRI_0330)] == [
        "Fri 2026-11-27 08:00",
        "Fri 2026-12-04 08:00",
    ]


def test_monthly_is_the_last_friday_of_the_month() -> None:
    parsed = _parse(expiries="Monthly")
    assert [_utc(x) for x in parsed.expiry_rule.at(_FRI_0330)] == [
        "Fri 2026-11-27 08:00",
        "Fri 2026-12-25 08:00",
    ]
    both = _parse(expiries={"weekly": 3, "monthly": 2})
    assert [_utc(x) for x in both.expiry_rule.at(_FRI_0330)] == [
        "Fri 2026-11-20 08:00",
        "Fri 2026-11-27 08:00",
        "Fri 2026-12-04 08:00",
        "Fri 2026-12-25 08:00",
    ]


@pytest.mark.parametrize(
    "raw, message",
    [
        ("daily", "is not a list of dates"),
        ({"weekly": 0, "monthly": 0}, "must be above 0"),
        ({"weekly": 13}, "0 to 12"),
        ({"weekly": True}, "whole number"),
        ({"fortnightly": 1}, "unknown key"),
    ],
)
def test_a_rule_that_makes_no_sense_is_refused(raw, message) -> None:
    with pytest.raises(config.OptionsConfigError, match=message):
        _parse(expiries=raw)


def test_a_list_of_dates_still_works_and_has_no_rule() -> None:
    parsed = _parse(expiries=["2026-12-01T13:00:00Z"])
    assert parsed.expiry_rule is None
    assert parsed.expiries_at(_FRI_0330) == parsed.expiries == (1796130000,)


def test_no_expiries_written_is_weekly_and_none_winds_down() -> None:
    """[DEFAULTS] A book with no expiries used to sit idle without saying why."""
    section = _section()
    section.pop("expiries")
    absent = config.parse({"options": section})
    assert absent is not None and absent.expiry_rule == config.ExpiryRule(2, 0)
    assert _parse(expiries=[]).expiry_rule == config.ExpiryRule(2, 0)
    for word in ("none", "None", " NONE "):
        winding = _parse(expiries=word)
        assert winding.expiry_rule is None and winding.expiries_at(_FRI_0330) == ()


def test_no_strikes_written_means_calls_and_puts() -> None:
    """[DEFAULTS] Both sides, out of the money, when neither list is written."""
    section = _section()
    del section["call_strikes_pct"], section["put_strikes_pct"]
    parsed = config.parse({"options": section})
    assert parsed is not None
    assert parsed.call_strikes_pct == config.DEFAULT_CALL_STRIKES_PCT == (110, 120)
    assert parsed.put_strikes_pct == config.DEFAULT_PUT_STRIKES_PCT == (90, 80)


def test_a_side_written_alone_keeps_the_other_off() -> None:
    """An existing grid gains no puts (or calls) nobody chose."""
    calls_only = _section()
    del calls_only["put_strikes_pct"]
    assert config.parse({"options": calls_only}).put_strikes_pct == ()
    puts_only = _section()
    del puts_only["call_strikes_pct"]
    assert config.parse({"options": puts_only}).call_strikes_pct == ()
    assert _parse(call_strikes_pct=[], put_strikes_pct=[]).call_strikes_pct == ()
    # [review] A list written null is written: empty, as before, not the defaults.
    nulls = _parse(call_strikes_pct=None, put_strikes_pct=None)
    assert (nulls.call_strikes_pct, nulls.put_strikes_pct) == ((), ())
    null_calls = _section(call_strikes_pct=None)
    del null_calls["put_strikes_pct"]
    assert config.parse({"options": null_calls}).put_strikes_pct == ()


def test_the_example_config_writes_both_sides() -> None:
    from pathlib import Path

    yaml = pytest.importorskip("yaml")
    example = Path(__file__).resolve().parents[4] / "config.example.yaml"
    section = yaml.safe_load(example.read_text(encoding="utf-8"))["options"]
    assert section["call_strikes_pct"] == list(config.DEFAULT_CALL_STRIKES_PCT)
    assert section["put_strikes_pct"] == list(config.DEFAULT_PUT_STRIKES_PCT)


# -- [AUTO-SIZE] options.contract_xch: auto ---------------------------------------


def _caps(xch, contracts=config.UNLIMITED, per_expiry=config.UNLIMITED) -> config.Limits:
    """Limits with ``xch`` XCH lockable (or UNSET / UNLIMITED) and the two counts."""
    locked = xch if xch in (config.UNSET, config.UNLIMITED) else int(xch * config.XCH_MOJOS)
    return config.Limits(
        max_locked_xch=locked,
        max_locked_quote=config.UNSET,
        max_contracts=contracts,
        max_contracts_per_expiry=per_expiry,
        max_fee_mojos=0,
    )


@pytest.mark.parametrize(
    ("limits", "calls", "expiries", "size"),
    [
        # Two strikes on two Fridays: four call slots.
        (_caps(4, 4, 2), 2, 2, 1),
        (_caps(39.99, 4, 2), 2, 2, 1),
        (_caps(40, 4, 2), 2, 2, 10),
        (_caps(400, 4, 2), 2, 2, 100),
        (_caps(4000, 4, 2), 2, 2, 1000),
        # The trading PC's limits on 2026-11-22: one 1 XCH contract fits.
        (_caps(1.0, 2, 2), 2, 2, 1),
        # The counts hold the grid to two contracts, so two 10 XCH ones fit in 20.
        (_caps(20, 2, 2), 2, 2, 10),
        (_caps(20, config.UNLIMITED, 1), 2, 2, 10),
        # No count caps: all four slots, and 10 XCH each would be 40.
        (_caps(20), 2, 2, 1),
        (_caps(20, config.UNSET, config.UNSET), 2, 2, 1),
        # More variety, smaller contracts.
        (_caps(100), 2, 2, 10),
        (_caps(100), 3, 4, 1),
        # Nothing to size from: the smallest.
        (_caps(config.UNSET), 2, 2, 1),
        (_caps(config.UNLIMITED), 2, 2, 1),
        (_caps(1000), 0, 2, 1),
        (_caps(1000), 2, 0, 1),
        # Under one contract: still 1 XCH, and the cap then lets none be written.
        (_caps(0.5, 4, 2), 2, 2, 1),
        # Bounded, whatever the cap.
        (_caps(10**12), 1, 1, config.AUTO_CONTRACT_MAX_XCH),
    ],
)
def test_auto_is_the_largest_power_of_ten_that_covers_every_call_slot(
    limits, calls, expiries, size
) -> None:
    """[AUTO-SIZE] Variety first, then size; never under 1 XCH."""
    assert config.auto_contract_xch(limits, calls, expiries) == size


def test_the_call_slots_the_grid_can_hold() -> None:
    assert config.held_call_slots(_caps(1), 2, 2) == 4
    assert config.held_call_slots(_caps(1, contracts=3), 2, 2) == 3
    assert config.held_call_slots(_caps(1, per_expiry=1), 2, 2) == 2
    assert config.held_call_slots(_caps(1, contracts=1, per_expiry=1), 2, 2) == 1
    assert config.held_call_slots(_caps(1, config.UNSET, config.UNSET), 2, 3) == 6
    assert config.held_call_slots(_caps(1), 0, 3) == 0


@pytest.mark.parametrize("written", ["auto", " AUTO ", "Auto", None])
def test_a_size_written_auto_or_null_is_worked_out(written) -> None:
    # Two call strikes on one date, at most 5 an expiry and 10 in all: two
    # slots, and 100 XCH lockable covers them at 10 XCH each, not at 100.
    parsed = _parse(contract_xch=written)
    assert parsed.contract_auto is True
    assert parsed.contract_xch_mojos == 10 * config.XCH_MOJOS
    assert parsed.market.hedge_size_xch == 10.0, "the price is read at the size written"


def test_a_size_left_out_is_auto_too() -> None:
    """[AUTO-SIZE] As an unwritten expiries is weekly: nothing more to set."""
    section = _section()
    del section["contract_xch"]
    parsed = config.parse({"options": section})
    assert parsed is not None and parsed.contract_auto is True
    assert parsed.contract_xch_mojos == 10 * config.XCH_MOJOS


def test_a_written_size_is_kept_as_written() -> None:
    parsed = _parse(contract_xch=0.5)
    assert parsed.contract_auto is False
    assert parsed.contract_xch_mojos == config.XCH_MOJOS // 2
    assert _parse(contract_xch="20").contract_xch_mojos == 20 * config.XCH_MOJOS


def test_auto_counts_the_expiries_of_a_rule() -> None:
    limits = {
        "max_locked_xch": 40,
        "max_locked_quote": 1,
        "max_contracts": 4,
        "max_contracts_per_expiry": 2,
    }
    weekly = _parse(
        contract_xch="auto", expiries="weekly", call_strikes_pct=[110, 120], limits=limits
    )
    assert weekly.contract_xch_mojos == 10 * config.XCH_MOJOS, "four slots of 10 XCH in 40"
    # Three weeklies and a month-end, two strikes each, eight contracts allowed.
    more = _parse(
        contract_xch="auto",
        expiries={"weekly": 3, "monthly": 1},
        call_strikes_pct=[110, 120],
        limits={**limits, "max_contracts": 8},
    )
    assert more.contract_xch_mojos == config.XCH_MOJOS, "eight slots: 10 XCH each would be 80"
    none = _parse(contract_xch="auto", expiries="none", limits=limits)
    assert none.contract_xch_mojos == config.XCH_MOJOS, "no new contracts: nothing to size"


def test_a_malformed_size_says_auto_is_a_choice() -> None:
    with pytest.raises(config.OptionsConfigError, match="a number of XCH, or auto"):
        _parse(contract_xch="big")
    with pytest.raises(config.OptionsConfigError, match="must be positive"):
        _parse(contract_xch=0)


def test_the_example_config_sizes_its_contracts_from_the_limits() -> None:
    from pathlib import Path

    yaml = pytest.importorskip("yaml")
    example = Path(__file__).resolve().parents[4] / "config.example.yaml"
    section = yaml.safe_load(example.read_text(encoding="utf-8"))["options"]
    assert section["contract_xch"] == "auto"


def test_the_same_expiry_written_twice_is_one_expiry() -> None:
    """[review #254] What auto sizes from, and what the Settings preview must match."""
    same = ["2026-12-01T13:00:00Z", "2026-12-01T14:00:00+01:00", 1796130000]
    assert config.distinct_expiries(same) == 1
    assert config.distinct_expiries([*same, "2026-12-08T13:00:00Z"]) == 2
    assert config.distinct_expiries([]) == 0
    with pytest.raises(config.OptionsConfigError):
        config.distinct_expiries(["next friday"])

    # Two call strikes on that one expiry, 20 XCH lockable: two slots of 10 XCH.
    parsed = _parse(
        contract_xch="auto",
        expiries=same,
        call_strikes_pct=[110, 120],
        limits={**_section()["limits"], "max_locked_xch": 20},
    )
    assert parsed.expiries == (1796130000,)
    assert parsed.contract_xch_mojos == 10 * config.XCH_MOJOS


def test_a_cap_too_large_to_count_is_refused_as_a_cap() -> None:
    """[review #254] 1e300 XCH is finite, but not in mojos: the book's own error."""
    limits = {**_section()["limits"], "max_locked_xch": 1e300}
    with pytest.raises(config.OptionsConfigError, match="max_locked_xch is too large"):
        _parse(limits=limits)
    limits = {**_section()["limits"], "max_locked_quote": "1e306"}
    with pytest.raises(config.OptionsConfigError, match="max_locked_quote is too large"):
        _parse(limits=limits)
