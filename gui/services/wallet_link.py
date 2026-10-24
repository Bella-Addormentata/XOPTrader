"""Wallet link: find and test the local Chia wallet software.

Desktop Chia tools reach a wallet the same way: a local HTTPS RPC server
that only answers clients presenting the wallet's own TLS certificate
(mutual TLS).  Every call is ``POST https://<host>:<port>/<endpoint>``
with a JSON body.  Two wallets are supported here:

* **Chia reference wallet** (the official GUI / ``chia start wallet``).
  RPC on port 9256 (``wallet.rpc_port`` in Chia's config.yaml), client
  cert ``<CHIA_ROOT>/config/ssl/wallet/private_wallet.crt`` + ``.key``.
  Chia's certificates come from a private CA, so the server cert is not
  verified on localhost (see ``chia.verify_ssl`` in config.example.yaml).
  This is the wallet XOPTrader trades through.

* **Sage** (xch-dev/sage).  RPC server is OFF until the user turns it on
  in Sage's settings; then it listens on 127.0.0.1:9257 (``[rpc]`` in
  Sage's config.toml).  It accepts exactly one client certificate: the
  self-signed ``ssl/wallet.crt`` + ``wallet.key`` in Sage's data folder,
  which is also its server certificate.  So the probe pins the server to
  that same certificate: while Sage's RPC is off, any local program could
  bind 9257, and it must not pass for Sage.  Errors come back as
  plain-text bodies with HTTP 400/401/404/500.

Everything a server sends back is untrusted text.  Callers show the
summaries in PlainText widgets only (see the Wallet Link page).

This module only *detects* and *probes* (read-only calls).  Nothing here
makes, takes or cancels offers.  Every function is blocking and returns
plain data, so callers run it off the GUI thread.
"""

from __future__ import annotations

import hashlib
import logging
import os
import re
import ssl
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Final, Optional

_log: logging.Logger = logging.getLogger(__name__)

CHIA_DEFAULT_PORT: Final[int] = 9256
SAGE_DEFAULT_PORT: Final[int] = 9257
SAGE_DEFAULT_HOST: Final[str] = "127.0.0.1"

# Tauri bundle identifier; Sage keeps config.toml, ssl/ and its wallet
# databases in the platform app-data folder named after it.
SAGE_APP_ID: Final[str] = "com.rigidnetwork.sage"

_PROBE_TIMEOUT_S: Final[float] = 5.0
_MOJOS_PER_XCH: Final[float] = 1_000_000_000_000.0

# ${VAR} / ${VAR:-default}: the substitution config.yaml and secrets.yaml
# support (the engine applies it; see the secrets.example.yaml header).
_ENV_REF_RE: Final[re.Pattern[str]] = re.compile(
    r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\}"
)


@dataclass
class ProbeResult:
    """Outcome of a connection test, ready for a one-line status label."""

    ok: bool
    summary: str
    details: dict[str, Any] = field(default_factory=dict)


# ===================================================================
# Detection
# ===================================================================

def chia_root(env: Optional[dict[str, str]] = None) -> Optional[Path]:
    """Return the Chia root folder, or None when Chia is not installed.

    Honours ``CHIA_ROOT`` (what the Chia CLI and GUI honour) before the
    standard ``~/.chia/mainnet``.  A root only counts when its
    ``config/ssl`` folder exists.
    """
    env = os.environ if env is None else env
    candidates: list[Path] = []
    if env.get("CHIA_ROOT"):
        candidates.append(Path(env["CHIA_ROOT"]).expanduser())
    candidates.append(Path.home() / ".chia" / "mainnet")
    for candidate in candidates:
        if (candidate / "config" / "ssl").is_dir():
            return candidate
    return None


def detect_chia_wallet(root: Optional[Path] = None) -> dict[str, Any]:
    """Look for the Chia reference wallet's RPC port, certs and fingerprint.

    Returns a dict with ``found`` (bool) and, when found, ``root``,
    ``port``, ``cert_path``, ``key_path`` and ``fingerprint`` (int or
    None).  Paths are forward-slash strings like the rest of the config.
    """
    root = root or chia_root()
    if root is None:
        return {"found": False}

    port = CHIA_DEFAULT_PORT
    cfg_path = root / "config" / "config.yaml"
    if cfg_path.is_file():
        try:
            import yaml  # noqa: WPS433

            with open(cfg_path, "r", encoding="utf-8") as fh:
                chia_cfg = yaml.safe_load(fh) or {}
            rpc_port = (chia_cfg.get("wallet") or {}).get("rpc_port")
            if isinstance(rpc_port, int):
                port = rpc_port
        except Exception as exc:  # noqa: BLE001 -- detection is best effort
            _log.warning("Could not read %s: %s", cfg_path, exc)

    ssl_dir = root / "config" / "ssl" / "wallet"
    cert = ssl_dir / "private_wallet.crt"
    key = ssl_dir / "private_wallet.key"

    fingerprint: Optional[int] = None
    fp_file = root / "wallet" / "db" / "last_used_fingerprint"
    try:
        text = fp_file.read_text(encoding="utf-8").strip()
        if text.isdigit():
            fingerprint = int(text)
    except OSError:
        # No wallet has logged in yet (or the file is unreadable): leave
        # the fingerprint unset and let the operator type it.
        fingerprint = None

    return {
        "found": cert.is_file() and key.is_file(),
        "root": root.as_posix(),
        "port": port,
        "cert_path": cert.as_posix() if cert.is_file() else "",
        "key_path": key.as_posix() if key.is_file() else "",
        "fingerprint": fingerprint,
    }


def sage_data_dirs(
    platform: Optional[str] = None,
    env: Optional[dict[str, str]] = None,
    home: Optional[Path] = None,
) -> list[Path]:
    """Return the folders Sage may keep its data in, most likely first."""
    platform = platform or sys.platform
    env = os.environ if env is None else env
    home = home or Path.home()
    if platform.startswith("win"):
        appdata = env.get("APPDATA")
        base = Path(appdata) if appdata else home / "AppData" / "Roaming"
        return [base / SAGE_APP_ID]
    if platform == "darwin":
        return [home / "Library" / "Application Support" / SAGE_APP_ID]
    xdg = env.get("XDG_DATA_HOME")
    base = Path(xdg) if xdg else home / ".local" / "share"
    return [base / SAGE_APP_ID]


def detect_sage(data_dirs: Optional[list[Path]] = None) -> dict[str, Any]:
    """Look for a Sage install and read its RPC settings.

    Returns a dict with ``found`` (bool) and, when found, ``data_dir``,
    ``cert_path``, ``key_path``, ``port`` and ``rpc_enabled`` (bool, or
    None when config.toml could not be read).
    """
    for data_dir in data_dirs if data_dirs is not None else sage_data_dirs():
        cert = data_dir / "ssl" / "wallet.crt"
        key = data_dir / "ssl" / "wallet.key"
        if not (cert.is_file() and key.is_file()):
            continue
        port = SAGE_DEFAULT_PORT
        rpc_enabled: Optional[bool] = None
        cfg_path = data_dir / "config.toml"
        try:
            with open(cfg_path, "rb") as fh:
                rpc = tomllib.load(fh).get("rpc") or {}
            # Sage's own defaults when a key is absent: off, 9257.
            rpc_enabled = bool(rpc.get("enabled", False))
            if isinstance(rpc.get("port"), int):
                port = rpc["port"]
        except (OSError, tomllib.TOMLDecodeError) as exc:
            _log.info("Could not read Sage config %s: %s", cfg_path, exc)
        return {
            "found": True,
            "data_dir": data_dir.as_posix(),
            "cert_path": cert.as_posix(),
            "key_path": key.as_posix(),
            "port": port,
            "rpc_enabled": rpc_enabled,
        }
    return {"found": False}


def expand_env(raw: str, env: Optional[dict[str, str]] = None) -> str:
    """Resolve ``${VAR}`` and ``${VAR:-default}`` in a config string.

    An unset ``${VAR}`` with no default is left as written, so an error
    message shows the operator what did not resolve.
    """
    env = os.environ if env is None else env

    def _sub(match: re.Match[str]) -> str:
        value = env.get(match.group(1))
        if value:
            return value
        default = match.group(2)
        return default if default is not None else match.group(0)

    return _ENV_REF_RE.sub(_sub, str(raw or "")).strip()


def expand_path(raw: str, env: Optional[dict[str, str]] = None) -> str:
    """:func:`expand_env`, then ``~``, for a config path."""
    return str(Path(expand_env(raw, env)).expanduser())


# ===================================================================
# Probing (read-only RPC calls)
# ===================================================================

PostFn = Callable[..., Any]


class _RpcError(Exception):
    """A call failed; the message is already user-facing."""


def _default_post() -> PostFn:
    import requests  # noqa: WPS433 -- GUI-only dependency
    import urllib3  # noqa: WPS433

    # Both wallets use certificates no public CA vouches for; the client
    # certificate is what authenticates the link (see module docstring).
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
    return requests.post


def cert_sha256(cert_path: str) -> str:
    """SHA-256 (hex) of the DER form of the first certificate in a PEM file."""
    text = Path(cert_path).read_text(encoding="ascii", errors="replace")
    begin = text.find("-----BEGIN CERTIFICATE-----")
    end_marker = "-----END CERTIFICATE-----"
    end = text.find(end_marker, begin)
    if begin < 0 or end < 0:
        raise ValueError("no PEM certificate found")
    der = ssl.PEM_cert_to_DER_cert(text[begin:end + len(end_marker)])
    return hashlib.sha256(der).hexdigest()


def _pinned_post(fingerprint: str) -> PostFn:
    """A ``post`` that only talks to a server presenting *fingerprint*.

    urllib3 checks ``assert_fingerprint`` against the peer certificate
    even with ``verify=False``, which is what a self-signed server needs.
    ``trust_env`` is off so an HTTPS_PROXY cannot route the call through a
    proxy pool that would skip the check (Sage only binds 127.0.0.1).
    """
    import requests  # noqa: WPS433 -- GUI-only dependency
    import urllib3  # noqa: WPS433
    from requests.adapters import HTTPAdapter  # noqa: WPS433

    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

    class _PinnedAdapter(HTTPAdapter):
        def init_poolmanager(self, *args: Any, **kwargs: Any) -> None:
            kwargs["assert_fingerprint"] = fingerprint
            super().init_poolmanager(*args, **kwargs)

    session = requests.Session()
    session.trust_env = False
    session.mount("https://", _PinnedAdapter())
    return session.post


def _check_files(cert_path: str, key_path: str) -> Optional[str]:
    for label, raw in (("certificate", cert_path), ("key", key_path)):
        if not str(raw or "").strip():
            return f"No {label} file set. Try Detect."
        if not Path(expand_path(raw)).is_file():
            return f"The {label} file was not found: {raw}"
    return None


def _call(
    post: PostFn,
    wallet: str,
    host: str,
    port: int,
    cert: tuple[str, str],
    endpoint: str,
    body: dict[str, Any],
    timeout: float,
    verify: bool | str = False,
) -> dict[str, Any]:
    """POST one RPC call and return its JSON object, or raise _RpcError."""
    host = expand_env(host)
    url = f"https://{host}:{port}/{endpoint}"
    try:
        resp = post(url, json=body, cert=cert, verify=verify,
                    timeout=timeout, allow_redirects=False)
    except Exception as exc:  # noqa: BLE001 -- classified below
        name = type(exc).__name__
        if "Fingerprints did not match" in str(exc):
            raise _RpcError(
                f"The server on {host}:{port} is not the {wallet} wallet that "
                "owns this certificate. Refusing to talk to it."
            ) from exc
        if "SSL" in name:
            reason = _tls_reason(exc)
            if "CERTIFICATE_VERIFY_FAILED" in reason:
                # Only reachable with verify_ssl on: the SERVER failed our
                # check; the client cert/key may be fine.
                raise _RpcError(
                    f"Could not verify the server certificate of {host}:{port} "
                    f"({reason}). Check chia.ca_cert_path and verify_ssl."
                ) from exc
            raise _RpcError(
                f"TLS handshake with {wallet} failed ({reason}). Check that "
                "the certificate and key belong to this wallet."
            ) from exc
        if "Timeout" in name:
            raise _RpcError(f"{wallet} did not answer within {timeout:g}s.") from exc
        raise _RpcError(
            f"Nothing answered on {host}:{port}. Is {wallet} running"
            + (" with its RPC server turned on?" if wallet == "Sage" else "?")
        ) from exc

    status = int(getattr(resp, "status_code", 200))
    if status == 401:
        raise _RpcError(f"{wallet} refused the certificate (HTTP 401).")
    if status >= 400:
        text = str(getattr(resp, "text", "") or "").strip()
        raise _RpcError(f"{wallet} returned HTTP {status} on {endpoint}: {text[:200]}")
    try:
        data = resp.json()
    except Exception as exc:  # noqa: BLE001
        raise _RpcError(f"{wallet} sent a reply that is not JSON ({endpoint}).") from exc
    if not isinstance(data, dict):
        raise _RpcError(f"{wallet} sent an unexpected reply ({endpoint}).")
    # The Chia RPC reports failures in-band as {"success": false, "error": ...}.
    if data.get("success") is False:
        raise _RpcError(f"{wallet} {endpoint} failed: {data.get('error', 'unknown error')}")
    return data


# The OpenSSL reason inside a requests/urllib3 SSLError, which wraps it in
# "HTTPSConnectionPool(...): Max retries exceeded ... (Caused by SSLError(...))".
_TLS_REASON_RE: Final[re.Pattern[str]] = re.compile(
    r"\[SSL[^\]]*\].*?(?=\s*\(_ssl\.c:\d+\)|'\)|\"\)|$)"
)


def _tls_reason(exc: BaseException) -> str:
    text = str(exc)
    match = _TLS_REASON_RE.search(text)
    return (match.group(0) if match else text).strip()[:200]


def _amount_to_int(value: Any) -> Optional[int]:
    """Sage amounts are JSON numbers, or strings above 2**53."""
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def probe_chia_wallet(
    host: str,
    port: int,
    cert_path: str,
    key_path: str,
    *,
    verify_ssl: bool = False,
    ca_cert_path: str = "",
    timeout: float = _PROBE_TIMEOUT_S,
    post: Optional[PostFn] = None,
) -> ProbeResult:
    """Read the Chia wallet's sync state, fingerprint and XCH balance.

    Server verification follows ``chia.verify_ssl`` exactly as the engine
    does: off (the localhost default, private CA issued to "Chia"), or on
    against ``ca_cert_path`` when set, else the system CA bundle.
    """
    problem = _check_files(cert_path, key_path)
    if problem:
        return ProbeResult(False, problem)
    verify: bool | str = False
    if verify_ssl:
        verify = expand_path(ca_cert_path) if str(ca_cert_path or "").strip() else True
        if isinstance(verify, str) and not Path(verify).is_file():
            return ProbeResult(
                False, f"verify_ssl is on but the CA file was not found: {ca_cert_path}")
    post = post or _default_post()
    cert = (expand_path(cert_path), expand_path(key_path))
    try:
        sync = _call(post, "Chia wallet", host, port, cert,
                     "get_sync_status", {}, timeout, verify)
        fp = _call(post, "Chia wallet", host, port, cert,
                   "get_logged_in_fingerprint", {}, timeout, verify).get("fingerprint")
        bal = _call(post, "Chia wallet", host, port, cert,
                    "get_wallet_balance", {"wallet_id": 1}, timeout, verify)
    except _RpcError as exc:
        return ProbeResult(False, str(exc))

    mojos = _amount_to_int((bal.get("wallet_balance") or {}).get(
        "confirmed_wallet_balance"))
    synced = bool(sync.get("synced"))
    state = "synced" if synced else ("syncing" if sync.get("syncing") else "not synced")
    parts = [f"Connected, {state}"]
    if fp:
        parts.append(f"fingerprint {fp}")
    if mojos is not None:
        parts.append(f"{mojos / _MOJOS_PER_XCH:,.4f} XCH")
    return ProbeResult(
        True,
        ", ".join(parts),
        {"synced": synced, "fingerprint": fp, "xch_mojos": mojos},
    )


def probe_sage(
    host: str,
    port: int,
    cert_path: str,
    key_path: str,
    *,
    timeout: float = _PROBE_TIMEOUT_S,
    post: Optional[PostFn] = None,
) -> ProbeResult:
    """Read Sage's version, logged-in key, sync progress and XCH balance."""
    problem = _check_files(cert_path, key_path)
    if problem:
        return ProbeResult(False, problem)
    if post is None:
        try:
            post = _pinned_post(cert_sha256(expand_path(cert_path)))
        except (OSError, ValueError) as exc:
            return ProbeResult(
                False, f"The certificate file is not a PEM certificate: {exc}")
    cert = (expand_path(cert_path), expand_path(key_path))
    try:
        version = _call(post, "Sage", host, port, cert,
                        "get_version", {}, timeout).get("version")
        key = _call(post, "Sage", host, port, cert, "get_key", {}, timeout).get("key")
        if not key:
            label = f"Sage {version}" if version else "Sage"
            return ProbeResult(
                False,
                f"Connected to {label}, but no wallet is logged in. "
                "Open a wallet in Sage and test again.",
                {"version": version},
            )
        sync = _call(post, "Sage", host, port, cert,
                      "get_sync_status", {}, timeout)
    except _RpcError as exc:
        return ProbeResult(False, str(exc))

    fp = key.get("fingerprint")
    name = key.get("name") or ""
    mojos = _amount_to_int(sync.get("selectable_balance"))
    unit = sync.get("unit") or {}
    # 0 is a real precision (Sage's "Mojos" unit); default only when absent.
    precision = unit.get("precision")
    if not isinstance(precision, int) or precision < 0:
        precision = 12
    ticker = str(unit.get("ticker") or "XCH")
    synced_coins = int(sync.get("synced_coins") or 0)
    total_coins = int(sync.get("total_coins") or 0)

    parts = [f"Connected to Sage {version}" if version else "Connected to Sage"]
    parts.append(f"wallet {name!r} ({fp})" if name else f"fingerprint {fp}")
    parts.append(f"{synced_coins}/{total_coins} coins synced")
    if mojos is not None:
        parts.append(f"{mojos / 10 ** precision:,.4f} {ticker} spendable")
    return ProbeResult(
        True,
        ", ".join(parts),
        {
            "version": version,
            "fingerprint": fp,
            "name": name,
            "synced": total_coins > 0 and synced_coins >= total_coins,
            "xch_mojos": mojos,
        },
    )
