"""Regression tests for the comment-preserving config writer.

The GUI's config writer used to serialise ``config.yaml`` with a plain
``yaml.safe_dump`` of the parsed dict.  On 2026-08-05 a single operator
Save therefore destroyed 115 lines of rationale — including the
2026-08-01 ``adverse_selection_sigma_threshold`` recalibration note.

``gui.services.config_split.dump_preserving`` re-uses the file already on
disk as a ``ruamel.yaml`` round-trip template, so comments, key order and
scalar formatting all survive a save.

Runs under pytest or directly:

    .venv/Scripts/python.exe tests/test_config_writer.py
"""

from __future__ import annotations

import logging
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402
import yaml  # noqa: E402

from gui.services import config_split  # noqa: E402
from gui.services.config_split import (  # noqa: E402
    RUAMEL_AVAILABLE,
    dump_preserving,
    split_and_save,
)

_COMMENTED_YAML = """\
# Leading rationale block.
chia:
  wallet_host: localhost
  wallet_port: 9256   # inline note
strategy:
  # Raised 30 -> 50 bps on 2026-07-30, once realized P&L became measurable.
  min_profit_margin_bps: 50
  gamma: 0.003
  tier_size_pct:
  - 0.10
  - 0.90
pairs:
# A per-pair note.
- name: XCH/wUSDC.b
  enabled: true
"""


requires_ruamel = pytest.mark.skipif(
    not RUAMEL_AVAILABLE, reason="ruamel.yaml not installed"
)


def _write(tmp_path: Path) -> Path:
    path = tmp_path / "config.yaml"
    path.write_text(_COMMENTED_YAML, encoding="utf-8")
    return path


@requires_ruamel
def test_identity_round_trip_is_byte_identical(tmp_path):
    """Load + dump with no edits must not change a single byte."""
    path = _write(tmp_path)
    before = path.read_bytes()
    dump_preserving(path, yaml.safe_load(before.decode("utf-8")))
    assert path.read_bytes() == before


@requires_ruamel
def test_single_edit_changes_only_that_value(tmp_path):
    path = _write(tmp_path)
    before = path.read_text(encoding="utf-8").splitlines()

    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    data["strategy"]["min_profit_margin_bps"] = 75
    dump_preserving(path, data)

    after = path.read_text(encoding="utf-8").splitlines()
    changed = [
        (a, b) for a, b in zip(before, after) if a != b
    ]
    assert len(before) == len(after)
    assert changed == [
        ("  min_profit_margin_bps: 50", "  min_profit_margin_bps: 75")
    ]


@requires_ruamel
def test_every_comment_survives(tmp_path):
    path = _write(tmp_path)
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    data["strategy"]["gamma"] = 0.004
    dump_preserving(path, data)

    after = path.read_text(encoding="utf-8")
    assert "# Leading rationale block." in after
    assert "# Raised 30 -> 50 bps on 2026-07-30" in after
    assert "# inline note" in after
    assert "# A per-pair note." in after
    assert yaml.safe_load(after)["strategy"]["gamma"] == 0.004


@requires_ruamel
def test_unchanged_scalars_keep_source_formatting(tmp_path):
    """`0.10` must not silently become `0.1` when nothing edited it."""
    path = _write(tmp_path)
    dump_preserving(path, yaml.safe_load(path.read_text(encoding="utf-8")))
    assert "- 0.10" in path.read_text(encoding="utf-8")


@requires_ruamel
def test_added_and_removed_keys(tmp_path):
    path = _write(tmp_path)
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    data["strategy"]["new_knob"] = 42
    del data["strategy"]["gamma"]
    dump_preserving(path, data)

    result = yaml.safe_load(path.read_text(encoding="utf-8"))
    assert result["strategy"]["new_knob"] == 42
    assert "gamma" not in result["strategy"]
    # The surviving neighbour keeps its comment.
    assert "# Raised 30 -> 50 bps" in path.read_text(encoding="utf-8")


@requires_ruamel
def test_split_and_save_preserves_comments(tmp_path):
    """The real GUI save path, not just the low-level writer."""
    path = _write(tmp_path)
    before = path.read_bytes()
    from gui.services.config_split import load_merged

    split_and_save(path, load_merged(path))
    assert path.read_bytes() == before


@requires_ruamel
def test_new_file_without_template(tmp_path):
    """No existing file: must still write valid YAML, not crash."""
    path = tmp_path / "brand_new.yaml"
    dump_preserving(path, {"a": {"b": 1}})
    assert yaml.safe_load(path.read_text(encoding="utf-8")) == {"a": {"b": 1}}


def test_falls_back_and_warns_without_ruamel(tmp_path, monkeypatch, caplog):
    """A missing ruamel must WARN about comment loss, never fail the save."""
    path = _write(tmp_path)
    monkeypatch.setattr(config_split, "RUAMEL_AVAILABLE", False)
    monkeypatch.setattr(config_split, "_warned_no_ruamel", False)

    with caplog.at_level(logging.WARNING, logger=config_split.__name__):
        dump_preserving(path, yaml.safe_load(_COMMENTED_YAML))

    # Values still reach disk...
    saved = yaml.safe_load(path.read_text(encoding="utf-8"))
    assert saved["strategy"]["min_profit_margin_bps"] == 50
    # ...and the comment-loss risk is named in the log.
    assert any("COMMENT" in r.message.upper() for r in caplog.records)


@requires_ruamel
def test_broken_template_falls_back(tmp_path, caplog):
    """Unparseable existing file must not lose the save."""
    path = tmp_path / "config.yaml"
    path.write_text("key: [unclosed\n", encoding="utf-8")
    with caplog.at_level(logging.WARNING, logger=config_split.__name__):
        dump_preserving(path, {"key": "value"})
    assert yaml.safe_load(path.read_text(encoding="utf-8")) == {"key": "value"}


@requires_ruamel
def test_line_endings_are_preserved(tmp_path):
    """config.yaml is CRLF on disk; a save must not reflow every line."""
    path = tmp_path / "config.yaml"
    path.write_bytes(_COMMENTED_YAML.replace("\n", "\r\n").encode("utf-8"))
    dump_preserving(path, yaml.safe_load(_COMMENTED_YAML))
    raw = path.read_bytes()
    assert raw.count(b"\r\n") > 0
    assert raw.count(b"\n") == raw.count(b"\r\n"), "stray bare LF introduced"


@requires_ruamel
def test_live_config_round_trips_byte_identical():
    """The repo's real config.yaml, loaded and written back unchanged."""
    live = _REPO / "config.yaml"
    if not live.is_file():
        pytest.skip("config.yaml not present")
    import shutil
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        copy = Path(td) / "config.yaml"
        shutil.copy2(live, copy)
        before = copy.read_bytes()
        dump_preserving(copy, yaml.safe_load(before.decode("utf-8")))
        assert copy.read_bytes() == before


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))


# ---------------------------------------------------------------------------
# Secrets further down a section (options.wallet.cert_path and the like)
# ---------------------------------------------------------------------------

_OPTIONS_YAML = """\
options:
  enabled: true      # the options book
  wallet:
    mode: dedicated
    port: 9356       # its own daemon
    cert_path: /c.crt
    key_path: /k.key
    fingerprint: 42
  node:
    cert_path: /n.crt
    key_path: /n.key
    ca_cert_path: /ca.crt
  database_path: data/options_jobs.db
"""


@requires_ruamel
def test_nested_secrets_go_to_secrets_yaml_and_the_rest_stays(tmp_path):
    """A Settings save loads config.yaml and secrets.yaml merged, then splits
    them again. The options book's certificate paths and fingerprint are two
    levels down; they must land in secrets.yaml, not in the tracked file."""
    path = tmp_path / "config.yaml"
    path.write_text(_OPTIONS_YAML, encoding="utf-8")

    split_and_save(path, yaml.safe_load(_OPTIONS_YAML))

    public = yaml.safe_load(path.read_text(encoding="utf-8"))
    secrets = yaml.safe_load((tmp_path / "secrets.yaml").read_text(encoding="utf-8"))
    assert public["options"]["wallet"] == {"mode": "dedicated", "port": 9356}
    # A mapping left with nothing in it is not left behind as {}.
    assert "node" not in public["options"]
    assert public["options"]["database_path"] == "data/options_jobs.db"
    assert secrets["options"] == {
        "wallet": {"cert_path": "/c.crt", "key_path": "/k.key", "fingerprint": 42},
        "node": {"cert_path": "/n.crt", "key_path": "/n.key", "ca_cert_path": "/ca.crt"},
    }
    # The comments on what stays are kept.
    assert "# its own daemon" in path.read_text(encoding="utf-8")
    # And read back together, nothing was lost.
    assert config_split.load_merged(path) == yaml.safe_load(_OPTIONS_YAML)


@requires_ruamel
def test_secrets_already_in_secrets_yaml_stay_there_through_a_save(tmp_path):
    path = tmp_path / "config.yaml"
    path.write_text("options:\n  enabled: false\n  wallet:\n    port: 9356\n", encoding="utf-8")
    secrets_path = tmp_path / "secrets.yaml"
    secrets_path.write_text(
        "options:\n  wallet:\n    cert_path: /c.crt\n    key_path: /k.key\n"
        "coingecko:\n  api_key: CG\n",
        encoding="utf-8",
    )
    before = path.read_bytes()

    split_and_save(path, config_split.load_merged(path))

    assert path.read_bytes() == before  # nothing copied into the tracked file
    secrets = yaml.safe_load(secrets_path.read_text(encoding="utf-8"))
    assert secrets["options"]["wallet"] == {"cert_path": "/c.crt", "key_path": "/k.key"}
    assert secrets["coingecko"] == {"api_key": "CG"}


@requires_ruamel
def test_an_empty_mapping_the_user_wrote_is_left_alone(tmp_path):
    """Only what a moved secret emptied is removed."""
    text = "options:\n  enabled: false\n  pricing: {}\n  node:\n    port: 8555\n"
    path = tmp_path / "config.yaml"
    path.write_text(text, encoding="utf-8")

    split_and_save(path, yaml.safe_load(text))

    assert yaml.safe_load(path.read_text(encoding="utf-8")) == yaml.safe_load(text)
    assert not (tmp_path / "secrets.yaml").exists()


def test_every_secret_key_names_a_real_place():
    # A dotted key is walked one level per dot; an empty part would never match.
    for section, keys in config_split.SECRET_KEYS.items():
        for key in keys:
            assert all(key.split(".")), (section, key)


@requires_ruamel
def test_a_failed_secrets_write_leaves_config_yaml_as_it_was(tmp_path):
    """An unreadable secrets.yaml is refused. Had config.yaml been written
    first, a secret being moved out of it would be gone from both files."""
    path = tmp_path / "config.yaml"
    path.write_text(_OPTIONS_YAML, encoding="utf-8")
    secrets_path = tmp_path / "secrets.yaml"
    secrets_path.write_text("chia: [unclosed\n", encoding="utf-8")
    before, secrets_before = path.read_bytes(), secrets_path.read_bytes()

    with pytest.raises(ValueError, match="refusing to overwrite"):
        split_and_save(path, yaml.safe_load(_OPTIONS_YAML))

    assert path.read_bytes() == before
    assert secrets_path.read_bytes() == secrets_before


def _options_on_disk(tmp_path, secrets_text: str) -> Path:
    path = tmp_path / "config.yaml"
    path.write_text("options:\n  enabled: false\n  wallet:\n    port: 9356\n", encoding="utf-8")
    (tmp_path / "secrets.yaml").write_text(secrets_text, encoding="utf-8")
    return path


@requires_ruamel
def test_a_save_does_not_undo_an_edit_made_to_secrets_yaml_since_it_loaded(tmp_path):
    """Nothing in the app edits the options secrets, so the copy a caller
    loaded earlier can only be stale. An unrelated save must not write it
    back over a newer value on disk."""
    path = _options_on_disk(
        tmp_path, "options:\n  wallet:\n    cert_path: /old.crt\n    fingerprint: 1\n"
    )
    loaded = config_split.load_merged(path)  # what a Settings page holds
    (tmp_path / "secrets.yaml").write_text(
        "options:\n  wallet:\n    cert_path: /new.crt\n", encoding="utf-8"
    )  # edited by hand since; and the fingerprint removed

    split_and_save(path, loaded)

    secrets = yaml.safe_load((tmp_path / "secrets.yaml").read_text(encoding="utf-8"))
    assert secrets["options"]["wallet"] == {"cert_path": "/new.crt"}  # kept, not put back
    public = yaml.safe_load(path.read_text(encoding="utf-8"))
    assert public["options"]["wallet"] == {"port": 9356}


@requires_ruamel
def test_a_secret_written_into_config_yaml_since_it_loaded_is_still_moved(tmp_path):
    path = _options_on_disk(tmp_path, "coingecko:\n  api_key: CG\n")
    loaded = config_split.load_merged(path)  # knows no cert_path
    path.write_text(
        "options:\n  enabled: false\n  wallet:\n    port: 9356\n    cert_path: /c.crt\n",
        encoding="utf-8",
    )

    split_and_save(path, loaded)

    secrets = yaml.safe_load((tmp_path / "secrets.yaml").read_text(encoding="utf-8"))
    public = yaml.safe_load(path.read_text(encoding="utf-8"))
    assert secrets["options"]["wallet"] == {"cert_path": "/c.crt"}
    assert public["options"]["wallet"] == {"port": 9356}
    assert secrets["coingecko"] == {"api_key": "CG"}


@requires_ruamel
def test_secrets_yaml_wins_when_both_files_carry_a_secret(tmp_path):
    """load_merged reads secrets.yaml over config.yaml; the save agrees."""
    path = tmp_path / "config.yaml"
    path.write_text(
        "options:\n  wallet:\n    port: 9356\n    cert_path: /stale.crt\n", encoding="utf-8"
    )
    (tmp_path / "secrets.yaml").write_text(
        "options:\n  wallet:\n    cert_path: /real.crt\n", encoding="utf-8"
    )
    merged = config_split.load_merged(path)

    split_and_save(path, merged)

    assert config_split.load_merged(path) == merged
    secrets = yaml.safe_load((tmp_path / "secrets.yaml").read_text(encoding="utf-8"))
    assert secrets["options"]["wallet"]["cert_path"] == "/real.crt"
    assert "cert_path" not in yaml.safe_load(path.read_text(encoding="utf-8"))["options"]["wallet"]


def test_every_file_owned_key_is_a_secret_key():
    for section, keys in config_split.FILE_OWNED_KEYS.items():
        assert keys <= config_split.SECRET_KEYS[section], section


@requires_ruamel
def test_an_edited_file_owned_secret_is_the_callers_and_none_removes_it(tmp_path):
    """[OPTIONS] The Settings page's Options tab edits the dedicated wallet's
    secrets: those it names in ``edited`` come from its copy, not the files."""
    path = tmp_path / "config.yaml"
    path.write_text("options:\n  wallet:\n    port: 9356\n", encoding="utf-8")
    (tmp_path / "secrets.yaml").write_text(
        "options:\n  wallet:\n    cert_path: /old.crt\n    key_path: /old.key\n"
        "    fingerprint: 4242\n",
        encoding="utf-8",
    )
    merged = config_split.load_merged(path)
    merged["options"]["wallet"]["cert_path"] = "/new.crt"
    merged["options"]["wallet"]["fingerprint"] = None
    merged["options"]["wallet"]["key_path"] = "/ignored.key"  # not named: the file's stays

    split_and_save(
        path,
        merged,
        edited=frozenset({"options.wallet.cert_path", "options.wallet.fingerprint"}),
    )

    wallet = yaml.safe_load((tmp_path / "secrets.yaml").read_text(encoding="utf-8"))["options"][
        "wallet"
    ]
    assert wallet == {"cert_path": "/new.crt", "key_path": "/old.key"}
    public = yaml.safe_load(path.read_text(encoding="utf-8"))["options"]["wallet"]
    assert "cert_path" not in public and "fingerprint" not in public
