# Changelog

All notable changes to XOPTrader are documented in this file.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.3] — 2026-03-28

### Changed

- Version bump to 0.1.3

## [0.1.2] — 2026-03-26

### Changed

- Version bump to 0.1.2

## [0.1.1] — 2026-03-26

### Fixed

- Fill-rate feedback loop: replaced hardcoded `fill_rate_24h = 0.30` and `fill_rate_per_block = 0.03` with DB-computed values from offer_log history
- Telegram alert HTML injection: added entity escaping (`&<>"`) in `post_telegram()` including unsafe fallback path
- SQLite diagnostic queries: `trade_count()`, `offer_count()`, `snapshot_count()` now check `sqlite3_step()` return values
- FetchContent supply-chain: pinned nlohmann_json, spdlog, yaml-cpp to commit SHAs instead of mutable tags
- Desktop file `Exec` path corrected for Linux packaging

### Added

- Configurable `offer_fee_mojos` in `StrategyConfig` (was hardcoded 100M mojos across 5 call sites)
- Link-Time Optimization for Release builds via `CheckIPOSupported`
- `ctest` step in CI workflow — tests now gate artifact upload
- Linux `uninstall.sh` with `--purge` option, bundled in release tarball
- `CHANGELOG.md`
- Release workflow triggers on GitHub UI release publish (+ concurrency guard)

### Changed

- TODO.md summary table updated (84/121 items complete)

## [0.1.0] — 2026-03-25

Initial release of the XOPTrader CHIA DEX market-making engine.

### Engine

- 13-step per-block heartbeat orchestration engine with Boost.Asio coroutines
- Avellaneda-Stoikov and GLFT market-making strategy implementations
- 4-component spread optimizer (adverse selection, inventory, cost basis, competition)
- Multi-tier offer ladder (configurable tiers, spacing, size allocation)
- Yang-Zhang volatility estimator with Bayesian PIN adverse-selection model
- HMM + variance-ratio regime detection (mean-reverting, random, momentum)
- Order book tactician with Thompson Sampling strategy selection
- Competitor detection and response (own-offer filtering, spread tracking, alerts)
- Whale trader detection with configurable thresholds
- CHIA structural edge multiplier (settlement speed, no-counterparty-risk, etc.)
- Strategic Loss Manager with EV-based rebalance decisions
- Arbitrage scanner (CEX-DEX, cross-DEX, triangular, cross-bridge)
- 7-layer hedging framework with Natural Hedge Efficiency tracking
- Backtesting framework with walk-forward window support

### Connectivity

- Chia full node RPC client (mTLS, port 8555) for block data and coin records
- Chia wallet RPC client (mTLS, port 9256) for offer lifecycle management
- dexie.space API client with rate-limited coroutine interface
- Per-request CURL handles with RAII wrappers for thread safety
- Configurable SSL verification with Chia CA cert support

### Risk Management

- Inventory tracking with mark-to-market concentration limits
- Soft/hard inventory limits with graduated proportional sizing
- Half-Kelly position sizing with division-by-zero guards
- Max-drawdown global circuit breaker (10% default)
- Flash-crash state machine (Normal → Crash → Recovery → Normal)
- Crowding recovery mechanism with cooldown and geometric decay
- Per-pair strategy instances (no shared mutable state)
- Configurable `max_half_spread_bps` cap preventing market withdrawal
- Configurable `offer_fee_mojos` for on-chain fee management

### Data Integrity

- SQLite persistence for trades, offers, and analytics snapshots
- `sqlite3_step()` return value checking on all diagnostic queries
- Fill-rate feedback loop computing rates from offer_log history
- Trade log timestamps populated from fill data
- Proper SHA-256 coin name computation via OpenSSL EVP
- `std::llround()` for all mojo price conversions (no truncation bias)
- Inventory units converted from mojos to base-asset display units
- Crossed-book data validation before ingestion

### Observability

- 24 Prometheus metrics with cardinality-guarded label sets
- 14 Telegram alert rules with HTML entity escaping
- Structured logging via spdlog with secrets redacted
- PnL attribution (spread / inventory / fee components)
- Tax CSV export with acquisition timestamps

### Build & Packaging

- CMake 3.24+ build system with vcpkg manifest mode
- C++20 with coroutine support (MSVC/GCC/Clang)
- Compiler hardening: `-Wall -Wextra -Werror`, stack protector, FORTIFY_SOURCE, RELRO
- Link-Time Optimization for Release builds via `CheckIPOSupported`
- FetchContent dependencies pinned to commit SHAs (nlohmann_json, spdlog, yaml-cpp)
- GitHub Actions CI/CD with 3-platform builds and artifact upload
- Tests run via `ctest` in CI before artifact creation
- Python GUI via PySide6 + pyqtgraph with PyInstaller packaging
- Windows Inno Setup installer with optional desktop shortcut
- Linux install bundle with `.desktop` file and uninstall script
- `pyproject.toml` with build backend, upper-bounded dependencies, `requires-python >=3.11,<4`

### Configuration

- YAML-based configuration with validation and error messages
- `config.example.yaml` reference with full 64-char asset ID placeholders
- GUI error handling for backend initialization failures

### Tests

- 81 unit tests across 8 test files (Google Test)
- Avellaneda-Stoikov math, spread optimizer, inventory/risk,
  volatility, regime detection, competitor detection, whale detection,
  and advanced trading methods

### Academic Rigor (Counter-Research Validation)

- VPIN validation gate with rolling-window precision tracking
- Exponential-decay tau for Avellaneda-Stoikov/GLFT
- Variance ratio Z-statistic significance gating
- Discounted Thompson Sampling for non-stationary rewards
- Sparse-fill correction for GLFT intensity estimation
- Fill-count dampening for Brock-Hommes heterogeneous agent model

### Known Limitations

- CEX reference prices not yet integrated (Phase 2)
- PreTradeCheck, GLFT, and config parsing test suites incomplete
- vcpkg baseline dated 2024-09-30
- No code signing for release binaries
