"""Utilities for the config.yaml / secrets.yaml split.

Secret fields (SSL paths, wallet fingerprint, API keys, Telegram
tokens, database path) live in ``secrets.yaml`` which is gitignored.
Public tuning knobs stay in ``config.yaml``.  Both the C++ engine
and the Python GUI merge secrets on top of the base config at load
time; this module provides the shared Python helpers.

Writing is **comment-preserving**.  ``config.yaml`` carries a large
amount of operator rationale in comments (why a threshold was
recalibrated, why a strategy is disabled).  A plain ``yaml.safe_dump``
of a parsed dict discards every one of them -- on 2026-08-05 a single
GUI Save destroyed 115 lines of that rationale.  :func:`dump_preserving`
therefore re-uses the *existing* file as a round-trip template
(``ruamel.yaml``) and writes only the values that actually changed.
"""

from __future__ import annotations

import copy
import logging
import os
import tempfile
import time
import threading
from contextlib import contextmanager
from pathlib import Path
from typing import Any

import yaml

_log = logging.getLogger(__name__)


# --------------------------------------------------------------------------- #
# Per-file write serialization.
#
# secrets.yaml is a read-modify-write store touched from more than one thread:
# the GUI settings page (preserving on-disk secrets it did not change) and the
# warp worker (BaseWallet create/rotate/backup). Without a shared lock, a
# settings save that read the file BEFORE a rotation wrote the new key can
# merge onto its stale snapshot and write the OLD key back, stranding the
# swept funds. A single re-entrant lock per resolved path makes each side's
# whole read-modify-write atomic against the other.
# --------------------------------------------------------------------------- #

_file_locks: dict[str, threading.RLock] = {}
_file_locks_guard = threading.Lock()


def file_lock(path: Path) -> threading.RLock:
    """The shared re-entrant lock for a config/secrets file, keyed by real path."""
    key = str(Path(path).resolve())
    with _file_locks_guard:
        lock = _file_locks.get(key)
        if lock is None:
            lock = threading.RLock()
            _file_locks[key] = lock
        return lock


@contextmanager
def file_transaction(path: Path):
    """Hold the per-file lock for a whole read-modify-write over *path*.

    Both the settings-save path and BaseWallet's key mutations enter this
    around their entire read...write span, so they can never interleave and
    clobber each other's writes.
    """
    lock = file_lock(path)
    lock.acquire()
    try:
        yield
    finally:
        lock.release()


def _atomic_write_text(path: Path, text: str, *, newline: str) -> None:
    """Write *text* to *path* atomically (unique temp + fsync + os.replace).

    ``open(path, "w")`` truncates in place, so a crash or ENOSPC mid-write
    leaves the file empty or partial. For secrets.yaml that window can erase
    the ONLY at-rest copy of a hot-wallet key -- the exact loss rotate()'s
    persist-before-broadcast ordering exists to prevent. os.replace is atomic
    on the same filesystem, so a reader ever sees only the whole old file or
    the whole new one, and a crash leaves the old file intact.

    The temp file gets a unique name (``mkstemp``), so two writers never
    collide on one staging path; and the destination ``os.replace`` runs
    under the per-file lock, because on Windows a concurrent replace of the
    same destination raises ``PermissionError``. The lock is re-entrant, so
    callers already inside :func:`file_transaction` nest harmlessly.
    """
    path = Path(path)
    with file_transaction(path):
        fd, tmp_name = tempfile.mkstemp(
            dir=str(path.parent), prefix=f".{path.name}.", suffix=".tmp"
        )
        tmp = Path(tmp_name)
        try:
            with os.fdopen(fd, "w", encoding="utf-8", newline=newline) as fh:
                fh.write(text)
                fh.flush()
                os.fsync(fh.fileno())
            # [RELOAD] The engine re-reads config.yaml within a heartbeat
            # of every save; on Windows its open read handle makes this
            # os.replace raise PermissionError. The read lasts
            # milliseconds -- absorb it instead of failing the save.
            for _attempt in range(3):
                try:
                    os.replace(tmp, path)
                    break
                except PermissionError:
                    if _attempt == 2:
                        raise
                    time.sleep(0.1)
        finally:
            try:
                if tmp.exists():
                    tmp.unlink()
            except OSError:  # pragma: no cover -- best-effort temp cleanup
                pass

# ---------------------------------------------------------------------------
# ruamel.yaml round-trip support (optional dependency)
# ---------------------------------------------------------------------------

try:  # pragma: no cover - import guard
    from ruamel.yaml import YAML as _RuamelYAML

    RUAMEL_AVAILABLE: bool = True
except ImportError:  # pragma: no cover - exercised only without ruamel
    _RuamelYAML = None  # type: ignore[assignment]
    RUAMEL_AVAILABLE = False

# Emitted once per process so a missing dependency is loud but not noisy.
_warned_no_ruamel: bool = False

# Sentinel for "key absent" (``None`` is a legitimate YAML value).
_MISSING: Any = object()


def _round_trip_yaml() -> Any:
    """Return a configured ``ruamel.yaml.YAML`` round-trip instance.

    Indentation is matched to the historical ``yaml.safe_dump`` output
    (block sequences dedented under their parent key) so switching the
    writer does not reflow the whole file.
    """
    y = _RuamelYAML(typ="rt")
    y.preserve_quotes = True
    # Never re-wrap long scalars or comment lines.
    y.width = 4096
    # safe_dump style:  "pairs:\n- base_asset_id: ..."
    y.indent(mapping=2, sequence=2, offset=0)
    return y


def _detect_newline(path: Path) -> str | None:
    """Return the dominant line terminator already used in *path*.

    Keeps a save from rewriting every line of the file just because the
    platform default differs from what is on disk.  ``None`` means "no
    existing file — use the platform default".
    """
    try:
        raw = path.read_bytes()
    except OSError:
        return None
    crlf = raw.count(b"\r\n")
    lf = raw.count(b"\n") - crlf
    if crlf == 0 and lf == 0:
        return None
    return "\r\n" if crlf >= lf else "\n"


def _unchanged(old: Any, new: Any) -> bool:
    """Return ``True`` when *new* is the same scalar value as *old*.

    Used to leave an existing node — and therefore its source formatting
    and trailing comment — completely untouched when the GUI round-trips
    a value it never edited.  ``bool`` is checked before the numeric
    branch because ``True == 1`` in Python.
    """
    if isinstance(old, bool) or isinstance(new, bool):
        return isinstance(old, bool) and isinstance(new, bool) and old == new
    if isinstance(old, (int, float)) and isinstance(new, (int, float)):
        return old == new
    if isinstance(old, str) and isinstance(new, str):
        return old == new
    return old is None and new is None


def _apply_map(target: Any, source: dict) -> None:
    """Copy *source* values into the round-trip mapping *target* in place.

    Existing keys keep their position and attached comments; keys absent
    from *source* are deleted; new keys are appended.
    """
    for key, value in source.items():
        current = target.get(key, _MISSING) if hasattr(target, "get") else _MISSING
        if isinstance(value, dict) and isinstance(current, dict):
            _apply_map(current, value)
        elif isinstance(value, list) and isinstance(current, list):
            _apply_seq(current, value)
        elif current is not _MISSING and _unchanged(current, value):
            # Identical scalar — keep the original node so its literal
            # formatting (0.10 vs 0.1) and inline comment survive.
            continue
        else:
            target[key] = value

    for key in [k for k in list(target.keys()) if k not in source]:
        del target[key]


def _apply_seq(target: list, source: list) -> None:
    """Copy *source* into the round-trip sequence *target* in place.

    Element-wise so that comments attached to individual list items
    (e.g. per-pair notes) are preserved across a save.
    """
    common = min(len(target), len(source))
    for idx in range(common):
        current, value = target[idx], source[idx]
        if isinstance(value, dict) and isinstance(current, dict):
            _apply_map(current, value)
        elif isinstance(value, list) and isinstance(current, list):
            _apply_seq(current, value)
        elif not _unchanged(current, value):
            target[idx] = value

    if len(source) > common:
        target.extend(source[common:])
    elif len(target) > common:
        del target[common:]


def dump_preserving(path: Path, data: dict[str, Any]) -> None:
    """Write *data* to *path*, preserving the existing file's comments.

    The file already at *path* (if any) is loaded with the ``ruamel.yaml``
    round-trip parser and used as a template: *data*'s values are applied
    onto it, so comments, key order, blank lines and scalar formatting all
    survive.  Keys removed from *data* are dropped; new keys are appended.

    Degrades gracefully: if ``ruamel.yaml`` is not installed, or the
    existing file cannot be round-tripped, this falls back to
    ``yaml.safe_dump`` and logs a WARNING naming the comment-loss risk.
    The save itself never fails because of comment handling.

    Parameters
    ----------
    path : Path
        Destination file.  Its current contents are the comment template.
    data : dict
        Fully-resolved configuration mapping to serialise.
    """
    global _warned_no_ruamel

    if not RUAMEL_AVAILABLE:
        if not _warned_no_ruamel:
            _warned_no_ruamel = True
            _log.warning(
                "ruamel.yaml is not installed: writing %s with PyYAML, which "
                "DISCARDS ALL COMMENTS in the file. Install ruamel.yaml "
                "(pip install 'ruamel.yaml>=0.18,<0.19') to preserve the "
                "operator rationale stored in config.yaml.",
                path,
            )
        _dump_plain(path, data)
        return

    try:
        yaml_rt = _round_trip_yaml()
        newline = _detect_newline(path)
        template: Any = None
        if path.is_file():
            with open(path, "r", encoding="utf-8") as fh:
                template = yaml_rt.load(fh)

        if not isinstance(template, dict):
            # No usable template (new file, empty file, or a non-mapping
            # root).  Emit through ruamel anyway so future saves have one.
            template = data

        else:
            _apply_map(template, data)

        import io

        buf = io.StringIO()
        yaml_rt.dump(template, buf)
        text = buf.getvalue()
    except Exception as exc:  # noqa: BLE001 - a save must never be lost
        _log.warning(
            "Comment-preserving write of %s failed (%s); falling back to "
            "PyYAML, WHICH DISCARDS ALL COMMENTS in this file.",
            path, exc,
        )
        _dump_plain(path, data)
        return

    # [RELOAD] The DISK write happens outside the fallback try/except: a
    # transient OSError (e.g. os.replace losing a sharing-violation race
    # against the engine's reload re-read of this same file) must surface
    # as a Save Error with comments intact -- not silently downgrade to the
    # comment-destroying plain dumper. The fallback above is reserved for
    # genuine round-trip (ruamel) failures.
    _atomic_write_text(path, text, newline=newline)


def _dump_plain(path: Path, data: dict[str, Any]) -> None:
    """Last-resort PyYAML writer (comment-destroying), still atomic."""
    text = yaml.safe_dump(
        data,
        default_flow_style=False,
        sort_keys=False,
        allow_unicode=True,
    )
    _atomic_write_text(path, text, newline=_detect_newline(path))

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
    # warp bridge hot-wallet key material, DPAPI-encrypted
    # (gui/services/warp/keystore.py). retired_keys carries the archived
    # blobs from every rotation and evm_key_backup_confirmed rides with the
    # key it describes; ALL of these must live here -- a settings save of the
    # merged config would otherwise write them into git-tracked config.yaml.
    "warp": {
        "evm_private_key_dpapi",
        "relay_private_key_dpapi",
        "retired_keys",
        "evm_key_backup_confirmed",
    },
    # Permuto trading identity (gui/services/permuto/identity.py).  The
    # wrapped BLS key IS the exchange account: same rule as the warp block
    # above, and for the same reason -- without this a Settings save writes
    # the blob straight into git-tracked config.yaml.  Reproduced before
    # fixing.  The public key, address and registration flags ride with the
    # key they describe so a stale public copy cannot resurrect retired
    # identity state alongside a fresh secret.
    "permuto": {
        "bls_private_key_dpapi",
        "bls_public_key",
        "backup_confirmed",
        "registered",
        "registered_at",
        "listing_verified",
        "link_attempted_at",
        "user_id",
        "trading_address",
        "created_at",
    },
}

# The subset of SECRET_KEYS written EXCLUSIVELY by BaseWallet (create/rotate/
# confirm-backup), never by the Settings UI. split_and_save must strip these
# from the public config (so they never leak to config.yaml) but must NOT
# copy them from the incoming snapshot into secrets.yaml: the settings page
# and ConfigService both cache secrets.yaml merged into memory at load, so a
# later Save would otherwise round-trip a STALE pre-rotation key blob back
# over the fresh one on disk -- destroying the only persisted copy of the
# rotated funds key. The on-disk secrets.yaml is authoritative for these.
WALLET_MANAGED_KEYS: dict[str, set[str]] = {
    "warp": {
        "evm_private_key_dpapi",
        "relay_private_key_dpapi",
        "retired_keys",
        "evm_key_backup_confirmed",
    },
    # Written EXCLUSIVELY by PermutoIdentity (create / restore / mark_*),
    # never by the Settings UI -- so Settings must strip them from the public
    # file AND must not copy its cached snapshot back over secrets.yaml. A
    # stale pre-restore blob round-tripping over a fresh one would destroy
    # the only persisted copy of the account key.
    "permuto": {
        "bls_private_key_dpapi",
        "bls_public_key",
        "backup_confirmed",
        "registered",
        "registered_at",
        "listing_verified",
        "link_attempted_at",
        "user_id",
        "trading_address",
        "created_at",
    },
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

    Both files are written through :func:`dump_preserving`, so comments
    already in them survive the save.
    """
    public = copy.deepcopy(full)
    secrets: dict[str, Any] = {}

    for section, keys in SECRET_KEYS.items():
        if section not in public:
            continue
        src = public[section]
        managed = WALLET_MANAGED_KEYS.get(section, set())
        for k in list(keys):
            if k not in src:
                continue
            if k in managed:
                # Strip from public (never leak to config.yaml) but do NOT
                # carry the possibly-stale snapshot value into the secrets
                # overlay: the on-disk secrets.yaml already holds the truth,
                # written by BaseWallet, and must win.
                src.pop(k)
                continue
            secrets.setdefault(section, {})[k] = src.pop(k)
        # Remove the section from public if it became empty.
        if not src:
            del public[section]

    # Write public config (comments in the existing file are preserved).
    dump_preserving(config_path, public)

    # Write secrets — preserve any keys the user added manually. The whole
    # read-existing / merge / write runs under the per-file lock so a
    # concurrent BaseWallet key write cannot land between our read and write
    # and be silently overwritten by our stale snapshot.
    #
    # Fail CLOSED on an existing file we cannot read as a mapping: the
    # overlay deliberately excludes wallet-managed keys, so overwriting a
    # present-but-corrupt secrets.yaml would erase the active/retired DPAPI
    # blobs it still holds as text. A save must never destroy what it could
    # not read; the operator fixes the file, then saves.
    secrets_path = config_path.parent / "secrets.yaml"
    if secrets:
        with file_transaction(secrets_path):
            if secrets_path.is_file():
                existing = None
                try:
                    with open(secrets_path, "r", encoding="utf-8") as fh:
                        existing = yaml.safe_load(fh)
                except Exception as exc:
                    raise ValueError(
                        f"{secrets_path} exists but cannot be read/parsed "
                        f"({exc}); refusing to overwrite it -- it may hold "
                        "key material. Fix or move the file, then save again."
                    ) from exc
                if existing is None:
                    existing = {}
                if not isinstance(existing, dict):
                    raise ValueError(
                        f"{secrets_path} does not parse to a mapping (got "
                        f"{type(existing).__name__}); refusing to overwrite "
                        "it -- it may hold key material."
                    )
                deep_merge(existing, secrets)
                secrets = existing
            dump_preserving(secrets_path, secrets)
