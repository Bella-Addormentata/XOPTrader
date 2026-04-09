"""Utilities for the config.yaml / secrets.yaml split.

Secret fields (SSL paths, wallet fingerprint, API keys, Telegram
tokens, database path) live in ``secrets.yaml`` which is gitignored.
Public tuning knobs stay in ``config.yaml``.  Both the C++ engine
and the Python GUI merge secrets on top of the base config at load
time; this module provides the shared Python helpers.
"""

from __future__ import annotations

import copy
import logging
from pathlib import Path
from typing import Any

import yaml

_log = logging.getLogger(__name__)

# Top-level section → set of keys that belong in secrets.yaml.
SECRET_KEYS: dict[str, set[str]] = {
    "chia": {
        "wallet_fingerprint",
        "ssl_cert_path",
        "ssl_key_path",
        "wallet_cert_path",
        "wallet_key_path",
        "ca_cert_path",
    },
    "monitoring": {"telegram_bot_token", "telegram_chat_id"},
    "coingecko": {"api_key"},
    "database": {"path"},
}


def deep_merge(base: dict, overlay: dict) -> None:
    """Recursively merge *overlay* into *base* (mutates *base*)."""
    for key, value in overlay.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            deep_merge(base[key], value)
        else:
            base[key] = value


def load_merged(config_path: Path) -> dict[str, Any]:
    """Load config.yaml and deep-merge secrets.yaml from the same dir."""
    with open(config_path, "r", encoding="utf-8") as fh:
        data: dict[str, Any] = yaml.safe_load(fh) or {}
    secrets_path = config_path.parent / "secrets.yaml"
    if secrets_path.is_file():
        try:
            with open(secrets_path, "r", encoding="utf-8") as fh:
                secrets = yaml.safe_load(fh) or {}
            if isinstance(secrets, dict):
                deep_merge(data, secrets)
        except Exception as exc:
            _log.warning("Failed to merge secrets.yaml: %s", exc)
    return data


def split_and_save(config_path: Path, full: dict[str, Any]) -> None:
    """Write public fields to *config_path*, secrets to sibling secrets.yaml.

    Fields listed in :data:`SECRET_KEYS` are extracted from *full*
    and written to ``secrets.yaml``.  Everything else goes to
    *config_path*.  Existing secrets.yaml entries that are NOT managed
    by :data:`SECRET_KEYS` are preserved.
    """
    public = copy.deepcopy(full)
    secrets: dict[str, Any] = {}

    for section, keys in SECRET_KEYS.items():
        if section not in public:
            continue
        src = public[section]
        for k in list(keys):
            if k in src:
                secrets.setdefault(section, {})[k] = src.pop(k)
        # Remove the section from public if it became empty.
        if not src:
            del public[section]

    # Write public config.
    with open(config_path, "w", encoding="utf-8") as fh:
        yaml.safe_dump(
            public, fh,
            default_flow_style=False,
            sort_keys=False,
            allow_unicode=True,
        )

    # Write secrets — preserve any keys the user added manually.
    secrets_path = config_path.parent / "secrets.yaml"
    if secrets:
        if secrets_path.is_file():
            try:
                with open(secrets_path, "r", encoding="utf-8") as fh:
                    existing = yaml.safe_load(fh) or {}
                if isinstance(existing, dict):
                    deep_merge(existing, secrets)
                    secrets = existing
            except Exception:
                pass
        with open(secrets_path, "w", encoding="utf-8") as fh:
            yaml.safe_dump(
                secrets, fh,
                default_flow_style=False,
                sort_keys=False,
                allow_unicode=True,
            )
