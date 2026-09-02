// metrics.cpp -- Prometheus metrics exporter implementation for XOPTrader.
//
// Uses prometheus-cpp (https://github.com/jupp0r/prometheus-cpp) to expose
// metrics over HTTP.  All six Grafana dashboards from Section 15 of the
// strategy document are covered by the metric families registered here.
//
// Metric naming follows the Prometheus convention:
//   <namespace>_<subsystem>_<name>_<unit>
//   namespace = "xop"
//   units use base units (mojos, milliseconds, basis points) so that
//   Prometheus rate() and increase() functions work correctly.
//
// Label cardinality is bounded: at most one label per metric family (asset_id
// or pair_name), and the number of distinct label values is bounded by the
// number of configured trading pairs (typically < 20).
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no secrets on the metrics endpoint
//   ISO/IEC 5055       -- no raw pointers, RAII ownership, bounds checks
//   ISO/IEC 25000      -- documented, single-responsibility methods

#include "xop/monitoring/metrics.hpp"
#include "xop/strategy/spread_mult.hpp"

#include <spdlog/spdlog.h>

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>

namespace xop {

// ===================================================================
//  Construction / Destruction
// ===================================================================

MetricsExporter::MetricsExporter() = default;

MetricsExporter::~MetricsExporter()
{
    // Ensure the HTTP server is stopped even if the caller forgot.
    shutdown();
}

// ===================================================================
//  Lifecycle
// ===================================================================

void MetricsExporter::init(std::uint16_t port,
                           const std::vector<std::string>& asset_ids)
{
    // T2-02: Exclusive lock -- init mutates running_, registry_, exposer_,
    // known_asset_ids_, and all metric family pointers.
    std::unique_lock lock(mtx_);

    if (running_) {
        spdlog::warn("MetricsExporter::init called while already running; ignoring");
        return;
    }

    spdlog::info("MetricsExporter: starting Prometheus HTTP server on port {}",
                 port);

    // ISO/IEC 5055: populate the known-asset set from config to bound
    // Prometheus label cardinality.
    known_asset_ids_.clear();
    known_asset_ids_.insert(asset_ids.begin(), asset_ids.end());
    spdlog::info("MetricsExporter: {} known asset IDs registered for cardinality guard",
                 known_asset_ids_.size());

    // Create the shared registry that holds all metric families.
    registry_ = std::make_shared<prometheus::Registry>();

    // Register every metric family before the exposer starts serving.
    register_metrics();

    // Start the HTTP server.  Exposer binds to 0.0.0.0:<port> and serves
    // the /metrics endpoint on a background thread.
    const std::string bind_address = "0.0.0.0:" + std::to_string(port);
    try {
        exposer_ = std::make_unique<prometheus::Exposer>(bind_address);
    } catch (const std::exception& e) {
        spdlog::error("MetricsExporter: failed to bind port {}: {}",
                      port, e.what());
        throw std::runtime_error(
            "MetricsExporter: failed to bind port " + std::to_string(port)
            + ": " + e.what());
    }

    // Wire the registry to the exposer so that /metrics scrapes it.
    exposer_->RegisterCollectable(registry_);

    running_ = true;
    spdlog::info("MetricsExporter: Prometheus endpoint live at :{}/metrics",
                 port);
}

void MetricsExporter::shutdown()
{
    // T2-02: Exclusive lock -- shutdown mutates running_, exposer_, registry_.
    std::unique_lock lock(mtx_);

    if (!running_) {
        return;
    }

    spdlog::info("MetricsExporter: shutting down Prometheus HTTP server");

    // Destroying the exposer joins the HTTP server thread.
    exposer_.reset();
    // Registry is ref-counted; reset our handle.
    registry_.reset();

    running_ = false;
    spdlog::info("MetricsExporter: shutdown complete");
}

bool MetricsExporter::is_running() const noexcept
{
    // T2-02: Shared lock -- read-only access to running_.
    std::shared_lock lock(mtx_);
    return running_;
}

// ===================================================================
//  Metric Registration
// ===================================================================

void MetricsExporter::register_metrics()
{
    // ---------------------------------------------------------------
    //  Dashboard 1: Real-Time PnL
    // ---------------------------------------------------------------

    pnl_family_ = &prometheus::BuildGauge()
        .Name("xop_pnl_mojos")
        .Help("Profit and loss components in mojos")
        .Register(*registry_);

    pnl_total_      = &pnl_family_->Add({{"component", "total"}});
    pnl_realized_   = &pnl_family_->Add({{"component", "realized"}});
    pnl_unrealized_ = &pnl_family_->Add({{"component", "unrealized"}});
    pnl_spread_     = &pnl_family_->Add({{"component", "spread"}});
    pnl_inventory_  = &pnl_family_->Add({{"component", "inventory"}});

    // [PNL-UNITS 2026-07-30] USD-normalized components.  The mojo gauges
    // above mix per-pair quote currencies and an XCH-mojo fee leg, so these
    // are the only display-safe values.
    pnl_usd_family_ = &prometheus::BuildGauge()
        .Name("xop_pnl_usd")
        .Help("PnL components in USD (per-pair quote-normalized)")
        .Register(*registry_);

    pnl_usd_            = &pnl_usd_family_->Add({{"component", "total"}});
    pnl_usd_realized_   = &pnl_usd_family_->Add({{"component", "realized"}});
    pnl_usd_unrealized_ = &pnl_usd_family_->Add({{"component", "unrealized"}});
    pnl_usd_fees_       = &pnl_usd_family_->Add({{"component", "fees"}});
    // [REWARD-INCOME 2026-08-01] Other income, beside the trading
    // components; deliberately not folded into component="total".
    pnl_usd_reward_income_ =
        &pnl_usd_family_->Add({{"component", "reward_income"}});
    // [S19 2026-08-23] External capital, beside the trading components;
    // deliberately not folded into component="total".
    pnl_usd_net_deposits_ =
        &pnl_usd_family_->Add({{"component", "net_deposits"}});

    // ---------------------------------------------------------------
    //  Dashboard 2: Inventory
    // ---------------------------------------------------------------

    inv_balance_family_ = &prometheus::BuildGauge()
        .Name("xop_inventory_balance")
        .Help("Per-asset holdings in mojos")
        .Register(*registry_);

    inv_cost_basis_family_ = &prometheus::BuildGauge()
        .Name("xop_inventory_cost_basis")
        .Help("Per-asset weighted-average cost basis in mojos")
        .Register(*registry_);

    inv_skew_family_ = &prometheus::BuildGauge()
        .Name("xop_inventory_skew")
        .Help("Per-pair inventory ratio (0=all quote, 0.5=balanced, 1=all base)")
        .Register(*registry_);

    inv_underwater_family_ = &prometheus::BuildGauge()
        .Name("xop_inventory_underwater")
        .Help("Per-asset underwater flag (1=cost_basis > market, 0=healthy)")
        .Register(*registry_);

    // ---------------------------------------------------------------
    //  Dashboard 3: Market Data
    // ---------------------------------------------------------------

    mkt_mid_price_family_ = &prometheus::BuildGauge()
        .Name("xop_market_mid_price")
        .Help("Per-pair mid-price in mojos")
        .Register(*registry_);

    mkt_spread_bps_family_ = &prometheus::BuildGauge()
        .Name("xop_market_spread_bps")
        .Help("Per-pair spread in basis points")
        .Register(*registry_);

    mkt_volume_24h_family_ = &prometheus::BuildGauge()
        .Name("xop_market_volume_24h")
        .Help("Per-pair rolling 24-hour volume in mojos")
        .Register(*registry_);

    // ---------------------------------------------------------------
    //  Dashboard 4: System Health
    // ---------------------------------------------------------------

    sys_family_ = &prometheus::BuildGauge()
        .Name("xop_node")
        .Help("Node and wallet connectivity status")
        .Register(*registry_);

    sys_block_height_    = &sys_family_->Add({{"metric", "block_height"}});
    sys_node_synced_     = &sys_family_->Add({{"metric", "synced"}});
    sys_wallet_connected_ = &sys_family_->Add({{"metric", "wallet_connected"}});

    // Offer-creation latency histogram with bucket boundaries chosen for
    // Chia's ~52-second block time.  Most offer creations complete in
    // under 2 seconds (local wallet RPC); tails reach into the tens of
    // seconds when the wallet is under load.
    offer_latency_family_ = &prometheus::BuildHistogram()
        .Name("xop_offer_latency_ms")
        .Help("Offer creation latency in milliseconds")
        .Register(*registry_);

    offer_latency_ = &offer_latency_family_->Add(
        {},
        prometheus::Histogram::BucketBoundaries{
            10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000
        });

    // ---------------------------------------------------------------
    //  Dashboard 5: Offer Lifecycle
    // ---------------------------------------------------------------

    offer_gauge_family_ = &prometheus::BuildGauge()
        .Name("xop_offers")
        .Help("Offer lifecycle gauges")
        .Register(*registry_);

    // [STOPDRAIN review #14] Children are added LAZILY on the first
    // update_offers(): a childless family serialises no sample lines, so
    // a scrape that carries xop_offers samples is evidence a heartbeat
    // ran. Eager zeros let the GUI read "0 pending" from an engine that
    // had not yet looked, which retired the cancel-all latch early.
    offers_pending_ = nullptr;
    fill_rate_gauge_ = nullptr;

    stopdrain_failing_family_ = &prometheus::BuildGauge()
        .Name("xop_stopdrain_failing")
        .Help("1 while the stopped-book TTL drain is failing (see "
              "[STOPDRAIN]); the GUI chip turns this into DRAIN FAILING")
        .Register(*registry_);
    stopdrain_failing_ = &stopdrain_failing_family_->Add({});

    offer_counter_family_ = &prometheus::BuildCounter()
        .Name("xop_offers_total")
        .Help("Cumulative offer lifecycle event counters")
        .Register(*registry_);

    offers_filled_    = &offer_counter_family_->Add({{"event", "filled"}});
    offers_cancelled_ = &offer_counter_family_->Add({{"event", "cancelled"}});
    offers_expired_   = &offer_counter_family_->Add({{"event", "expired"}});

    // ---------------------------------------------------------------
    //  Dashboard 6: Risk
    // ---------------------------------------------------------------

    risk_family_ = &prometheus::BuildGauge()
        .Name("xop_risk")
        .Help("Portfolio risk metrics")
        .Register(*registry_);

    risk_var_95_       = &risk_family_->Add({{"metric", "var_95"}});
    risk_max_drawdown_ = &risk_family_->Add({{"metric", "max_drawdown"}});

    risk_concentration_family_ = &prometheus::BuildGauge()
        .Name("xop_risk_concentration")
        .Help("Per-asset portfolio concentration fraction (0-1)")
        .Register(*registry_);

    // ---------------------------------------------------------------
    //  Dashboard 7: Startup Market Analysis
    // ---------------------------------------------------------------

    // Scalar gauge: analysis window block-count target.
    // Per-pair complete flags are published under xop_analysis_pair.
    analysis_family_ = &prometheus::BuildGauge()
        .Name("xop_analysis")
        .Help("Startup market analysis global state")
        .Register(*registry_);

    // Per-pair gauges: all analysis metrics labelled by pair_name.
    analysis_pair_family_ = &prometheus::BuildGauge()
        .Name("xop_analysis_pair")
        .Help("Startup market analysis per-pair observations")
        .Register(*registry_);

    // [S42] LIVE strategy-controller state, refreshed every block.  The
    // Help string says "live" because the adjacent xop_analysis family is
    // NOT, and a consumer picked the wrong one for months.
    strategy_family_ = &prometheus::BuildGauge()
        .Name("xop_strategy")
        .Help("Live per-pair strategy controller state (updated every block)")
        .Register(*registry_);

    // ---------------------------------------------------------------
    //  Wallet reserve & stuck offers
    // ---------------------------------------------------------------

    spendable_reserve_family_ = &prometheus::BuildGauge()
        .Name("xop_spendable_reserve_pct")
        .Help("Fraction of confirmed balance that is spendable (0-1)")
        .Register(*registry_);

    stuck_offers_gauge_ = &prometheus::BuildGauge()
        .Name("xop_stuck_offers")
        .Help("Number of offers stuck beyond TTL + stuck_age threshold")
        .Register(*registry_)
        .Add({});

    // [v0.7.46 #8] Peak concurrent stuck offers in this process lifetime.
    stuck_offers_peak_gauge_ = &prometheus::BuildGauge()
        .Name("xop_stuck_offers_peak")
        .Help("Max concurrent stuck offers seen since process start")
        .Register(*registry_)
        .Add({});

    // [v0.7.46 #8] Lifetime counter of new stuck offers detected.
    stuck_offers_total_counter_ = &prometheus::BuildCounter()
        .Name("xop_stuck_offers_total")
        .Help("Lifetime count of new stuck offers detected (use rate()[1h])")
        .Register(*registry_)
        .Add({});

    // [R2 review #12] Lazy like the gate family: the GUI's
    // posting_gates_published() falls back to this family's presence, so
    // an eager zero kept that check vacuously true too.
    paused_family_ = &prometheus::BuildGauge()
        .Name("xop_bot_paused")
        .Help("1 while the GUI pause flag is active -- the pause the GUI "
              "Resume button can clear.  Breaker and protection pauses are "
              "on xop_posting_gated instead")
        .Register(*registry_);
    paused_gauge_ = nullptr;

    posting_gate_family_ = &prometheus::BuildGauge()
        .Name("xop_posting_gate")
        .Help("1 while the named standing gate disables offer posting; "
              "reason label is one of gui, breaker, wallet_circuit, "
              "flash_crash, xch_recovery, dry_run, watchdog. The watchdog "
              "gate is restart-only: once set it stays set, because the "
              "dead man's switch firing means the book was cancelled by a "
              "process that has stopped managing it.")
        .Register(*registry_);
    // [R2 review #12] Gate children are added LAZILY on the first
    // update_posting_gates(): presence of any xop_posting_gate sample in
    // a scrape is then genuine heartbeat evidence, which the GUI's
    // posting_ungated check and intent adoption both rely on. Eager zeros
    // made posting_gates_published() vacuously true.
    gate_gui_ = nullptr;
    gate_breaker_ = nullptr;
    gate_wallet_circuit_ = nullptr;
    gate_flash_crash_ = nullptr;
    gate_xch_recovery_ = nullptr;
    gate_dry_run_ = nullptr;
    gate_watchdog_ = nullptr;
    gate_cancels_pending_ = nullptr;

    posting_gated_gauge_ = &prometheus::BuildGauge()
        .Name("xop_posting_gated")
        .Help("1 when offer posting is disabled by a standing gate -- GUI "
              "pause, a latched risk breaker (max-drawdown, rolling-window "
              "loss, ledger divergence), the wallet circuit breaker, a "
              "flash-crash episode, XCH recovery mode, or dry-run.  0 means "
              "no standing gate; transient per-cycle aborts (e.g. wallet "
              "not yet synced) are not reflected here")
        .Register(*registry_)
        .Add({});

    fees_paid_24h_gauge_ = &prometheus::BuildGauge()
        .Name("xop_fees_paid_24h_mojos")
        .Help("Rolling 24-hour blockchain fees paid in mojos")
        .Register(*registry_)
        .Add({});

    trade_decision_counter_family_ = &prometheus::BuildCounter()
        .Name("xop_trade_decision_total")
        .Help("Cumulative trade decision-tree branch events")
        .Register(*registry_);
}

// ===================================================================
//  Dashboard 1: Real-Time PnL
// ===================================================================

void MetricsExporter::update_pnl(const MetricsPnlSnapshot& summary)
{
    // T2-02: Exclusive lock -- update_pnl writes to prometheus gauge objects.
    std::unique_lock lock(mtx_);

    if (!running_) {
        return;
    }

    // Set each gauge to the absolute value (gauges are point-in-time).
    pnl_total_->Set(static_cast<double>(summary.total));
    pnl_realized_->Set(static_cast<double>(summary.realized));
    pnl_unrealized_->Set(static_cast<double>(summary.unrealized));
    pnl_spread_->Set(static_cast<double>(summary.spread));
    pnl_inventory_->Set(static_cast<double>(summary.inventory));
    if (pnl_usd_)            pnl_usd_->Set(summary.usd);
    if (pnl_usd_realized_)   pnl_usd_realized_->Set(summary.usd_realized);
    if (pnl_usd_unrealized_) pnl_usd_unrealized_->Set(summary.usd_unrealized);
    if (pnl_usd_fees_)       pnl_usd_fees_->Set(summary.usd_fees);
    if (pnl_usd_reward_income_) {
        pnl_usd_reward_income_->Set(summary.usd_reward_income);
    }
    if (pnl_usd_net_deposits_) {
        pnl_usd_net_deposits_->Set(summary.usd_net_deposits);
    }
}

// ===================================================================
//  Dashboard 2: Inventory
// ===================================================================

void MetricsExporter::update_inventory(
    const std::vector<InventorySnapshot>& positions,
    const std::vector<InventorySkewSnapshot>& skews)
{
    // T2-02: Exclusive lock -- update_inventory writes to prometheus gauge
    // objects and reads known_asset_ids_.
    std::unique_lock lock(mtx_);

    if (!running_) {
        return;
    }

    // Per-asset gauges: balance, cost basis, underwater flag.
    for (const auto& pos : positions) {
        // ISO/IEC 5055: reject unknown asset IDs to prevent unbounded
        // Prometheus label cardinality.
        if (!known_asset_ids_.empty() &&
            known_asset_ids_.find(pos.asset_id) == known_asset_ids_.end()) {
            spdlog::warn("[Metrics] Unknown asset_id '{}' -- skipping inventory metric",
                         pos.asset_id);
            continue;
        }

        inv_balance_family_
            ->Add({{"asset_id", pos.asset_id}})
            .Set(static_cast<double>(pos.balance));

        inv_cost_basis_family_
            ->Add({{"asset_id", pos.asset_id}})
            .Set(static_cast<double>(pos.cost_basis));

        inv_underwater_family_
            ->Add({{"asset_id", pos.asset_id}})
            .Set(pos.underwater ? 1.0 : 0.0);
    }

    // Per-pair skew gauge.
    for (const auto& s : skews) {
        inv_skew_family_
            ->Add({{"pair_name", s.pair_name}})
            .Set(s.skew);
    }
}

// ===================================================================
//  Dashboard 3: Market Data
// ===================================================================

void MetricsExporter::update_market(const std::vector<MarketSnapshot>& snapshots)
{
    // T2-02: Exclusive lock -- update_market writes to prometheus gauge objects.
    std::unique_lock lock(mtx_);

    if (!running_) {
        return;
    }

    for (const auto& snap : snapshots) {
        mkt_mid_price_family_
            ->Add({{"pair_name", snap.pair_name}})
            .Set(static_cast<double>(snap.mid_price));

        mkt_spread_bps_family_
            ->Add({{"pair_name", snap.pair_name}})
            .Set(snap.spread_bps);

        mkt_volume_24h_family_
            ->Add({{"pair_name", snap.pair_name}})
            .Set(static_cast<double>(snap.volume_24h));
    }
}

// ===================================================================
//  Dashboard 4: System Health
// ===================================================================

void MetricsExporter::update_system_health(const SystemHealthSnapshot& health)
{
    // T2-02: Exclusive lock -- update_system_health writes to prometheus gauge
    // objects.
    std::unique_lock lock(mtx_);

    if (!running_) {
        return;
    }

    sys_block_height_->Set(static_cast<double>(health.block_height));
    sys_node_synced_->Set(health.node_synced ? 1.0 : 0.0);
    sys_wallet_connected_->Set(health.wallet_connected ? 1.0 : 0.0);
}

void MetricsExporter::observe_offer_latency_ms(double latency_ms)
{
    // T2-02: Exclusive lock -- observe_offer_latency_ms writes to prometheus
    // histogram object.
    std::unique_lock lock(mtx_);

    if (!running_) {
        return;
    }

    offer_latency_->Observe(latency_ms);
}

// ===================================================================
//  Dashboard 5: Offer Lifecycle
// ===================================================================

void MetricsExporter::update_offers(
    std::size_t pending_count,
    std::uint64_t fill_count,
    std::uint64_t cancel_count,
    std::uint64_t expired_count,
    double fill_rate_per_hour)
{
    // T2-02: Exclusive lock -- update_offers mutates shadow counters
    if (!offers_pending_) {
        offers_pending_  = &offer_gauge_family_->Add({{"state", "pending"}});
        fill_rate_gauge_ = &offer_gauge_family_->Add(
            {{"state", "fill_rate_per_hour"}});
    }
    // (last_fill_count_, last_cancel_count_, last_expired_count_) and writes
    // to prometheus gauge/counter objects.
    std::unique_lock lock(mtx_);

    if (!running_) {
        return;
    }

    // Pending is a gauge (point-in-time).
    offers_pending_->Set(static_cast<double>(pending_count));

    // Fill rate is also a gauge (derived metric, not monotonic).
    fill_rate_gauge_->Set(fill_rate_per_hour);

    // Counters are monotonically increasing.  prometheus-cpp Counter only
    // supports Increment(), so we compute the delta from the last reported
    // cumulative value and increment by that amount.

    if (fill_count > last_fill_count_) {
        offers_filled_->Increment(
            static_cast<double>(fill_count - last_fill_count_));
        last_fill_count_ = fill_count;
    }

    if (cancel_count > last_cancel_count_) {
        offers_cancelled_->Increment(
            static_cast<double>(cancel_count - last_cancel_count_));
        last_cancel_count_ = cancel_count;
    }

    if (expired_count > last_expired_count_) {
        offers_expired_->Increment(
            static_cast<double>(expired_count - last_expired_count_));
        last_expired_count_ = expired_count;
    }
}

// ===================================================================
//  Dashboard 6: Risk
// ===================================================================

void MetricsExporter::update_risk(
    const RiskSnapshot& risk,
    const std::vector<ConcentrationEntry>& concentrations)
{
    // T2-02: Exclusive lock -- update_risk writes to prometheus gauge objects
    // and reads known_asset_ids_.
    std::unique_lock lock(mtx_);

    if (!running_) {
        return;
    }

    risk_var_95_->Set(risk.var_95);
    risk_max_drawdown_->Set(risk.max_drawdown);

    for (const auto& c : concentrations) {
        // ISO/IEC 5055: reject unknown asset IDs to prevent unbounded
        // Prometheus label cardinality.
        if (!known_asset_ids_.empty() &&
            known_asset_ids_.find(c.asset_id) == known_asset_ids_.end()) {
            spdlog::warn("[Metrics] Unknown asset_id '{}' -- skipping risk metric",
                         c.asset_id);
            continue;
        }

        risk_concentration_family_
            ->Add({{"asset_id", c.asset_id}})
            .Set(c.concentration);
    }
}

// ===================================================================
//  Dashboard 7: Startup Market Analysis
// ===================================================================

void MetricsExporter::update_analysis(
    const std::string& pair_name,
    uint32_t blocks_collected,
    uint32_t blocks_target,
    bool     complete,
    double   vol_annual,
    double   mean_spread_bps,
    double   spread_cv,
    double   variance_ratio,
    double   book_imbalance,
    double   momentum,
    int      regime_code,
    int      agg_code)
{
    std::unique_lock lock(mtx_);

    if (!running_) {
        return;
    }

    // Global scalar: target block count (same for all pairs).
    analysis_family_
        ->Add({{"metric", "blocks_target"}})
        .Set(static_cast<double>(blocks_target));

    // Per-pair: blocks collected.
    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "blocks_collected"}})
        .Set(static_cast<double>(blocks_collected));

    // Per-pair: complete flag.
    //
    // [S23 2026-08-24] Reports the ANALYZER's completion state, not
    // `blocks_collected >= blocks_target`.  Those differ now: a
    // structurally unpriced pair legitimately ends the phase with zero
    // observations, and re-deriving the flag from the count left it at 0
    // forever -- so MetricsService.is_analysis_active() kept the GUI
    // showing "Analyzing" for the life of a process that was already
    // trading.  blocks_collected stays honest at 0; the window_filled
    // distinction lives in the analyzer for callers that need it.
    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "complete"}})
        .Set(complete ? 1.0 : 0.0);

    // Per-pair: volatility.
    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "volatility_annual"}})
        .Set(vol_annual);

    // Per-pair: spread.
    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "mean_spread_bps"}})
        .Set(mean_spread_bps);

    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "spread_cv"}})
        .Set(spread_cv);

    // Per-pair: variance ratio.
    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "variance_ratio"}})
        .Set(variance_ratio);

    // Per-pair: order-book imbalance.
    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "book_imbalance"}})
        .Set(book_imbalance);

    // Per-pair: momentum.
    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "momentum"}})
        .Set(momentum);

    // Per-pair: regime code (0=MeanReverting, 1=Normal, 2=Momentum).
    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "regime"}})
        .Set(static_cast<double>(regime_code));

    // Per-pair: aggressiveness code (0=Conservative, 1=Normal, 2=Aggressive).
    analysis_pair_family_
        ->Add({{"pair_name", pair_name}, {"metric", "aggressiveness"}})
        .Set(static_cast<double>(agg_code));
}

void MetricsExporter::set_analysis_spread_multiplier(double mult)
{
    std::unique_lock lock(mtx_);
    if (!running_) return;

    analysis_family_
        ->Add({{"metric", "recommended_spread_multiplier"}})
        .Set(mult);
}

// ===================================================================
//  Live strategy controllers  [S42 2026-09-02]
//
//  Called every block from Engine::step_export_metrics().  Contrast with
//  update_analysis() above, whose only two call sites are both inside
//  Engine::run_startup_analysis() -- those gauges are written during the
//  observation phase and then never again for the life of the process.
// ===================================================================

void MetricsExporter::update_strategy(
    const std::vector<MetricsStrategySnapshot>& snaps)
{
    std::unique_lock lock(mtx_);
    if (!running_) return;

    for (const auto& s : snaps) {
        const auto g = [&](const char* metric, double v) {
            strategy_family_
                ->Add({{"pair_name", s.pair_name}, {"metric", metric}})
                .Set(v);
        };

        // -- the multiplier the operator actually needs ------------------
        // Published next to the startup recommendation, never instead of
        // it, so the two can be compared rather than confused.
        g("analysis_spread_mult",   s.analysis_spread_mult);
        g("spread_pid_active",      s.spread_pid_active ? 1.0 : 0.0);
        g("spread_pid_mult",        s.spread_pid_mult);
        g("spread_pid_ema_fill",    s.spread_pid_ema_fill);
        g("spread_pid_integral",    s.spread_pid_integral);
        g("spread_pid_blocks",
          static_cast<double>(s.spread_pid_blocks));

        // [2026-09-02, review] THIS IS NOT THE MULTIPLIER APPLIED TO THE
        // SPREAD, and it is no longer named or labelled as though it were.
        // It is the product of exactly two of Step 5's ten mutation sites --
        // the startup analysis and the spread PID -- published because those
        // two are the controllers an operator reasons about, and because the
        // GUI previously showed ONE of them alone and got the direction of
        // the adjustment wrong.
        //
        // Two of the eight sites it does not capture are ASSIGNMENTS (the
        // order-book tactician, the global half-spread cap) which discard the
        // entire chain including both of these factors.  Calling this the
        // effective multiplier re-created the very defect the live gauges
        // exist to remove.  For what was actually applied, use
        // spread_applied_mult below.
        g("analysis_x_pid_mult",
          s.analysis_spread_mult
          * (s.spread_pid_active ? s.spread_pid_mult : 1.0));

        // What Step 5 ACTUALLY did to the spread, measured across the whole
        // step rather than derived from any subset of its sites: the spread
        // the optimizer produced, the spread that was posted, and the ratio.
        // A future adjustment site -- multiplicative, assignment, clamp, or
        // something not yet written -- is captured automatically, because
        // this never looks at the chain.
        //
        // 0 means NO READING (Step 5 did not reach this pair this block), not
        // 1.0x.  See xop/strategy/spread_mult.hpp for why the sentinel is 0.
        g("spread_base_bps",     s.spread_base_bps);
        g("spread_applied_bps",  s.spread_applied_bps);
        g("spread_applied_mult",
          strategy::applied_spread_mult(s.spread_base_bps,
                                        s.spread_applied_bps));

        // -- rail latch --------------------------------------------------
        g("rail_latched",           s.rail_latched ? 1.0 : 0.0);
        g("rail_side",              static_cast<double>(s.rail_side));
        g("rail_latched_blocks",
          static_cast<double>(s.rail_latched_blocks));
        g("rail_suppressed",
          static_cast<double>(s.rail_suppressed));
        g("rail_peak_abs_integral", s.rail_peak_abs_integral);

        // -- competitiveness PID ----------------------------------------
        g("comp_pid_active",        s.comp_pid_active ? 1.0 : 0.0);
        g("comp_pid_offset",        static_cast<double>(s.comp_pid_offset));
        g("comp_pid_ema_fill",      s.comp_pid_ema_fill);
        g("comp_gate_base",         static_cast<double>(s.comp_gate_base));
        g("comp_gate_effective",
          static_cast<double>(s.comp_gate_effective));

        // -- crossed-book take retry -------------------------------------
        g("take_suppressed",   static_cast<double>(s.take_suppressed));
        g("take_tracked",      static_cast<double>(s.take_tracked));
    }
}

// ===================================================================
//  Wallet Reserve & Stuck Offers
// ===================================================================

void MetricsExporter::update_spendable_reserve(
    const std::string& wallet_label, double ratio)
{
    std::unique_lock lock(mtx_);
    if (!running_) return;

    spendable_reserve_family_
        ->Add({{"wallet", wallet_label}})
        .Set(ratio);
}

void MetricsExporter::update_stuck_offers(int count)
{
    std::unique_lock lock(mtx_);
    if (!running_) return;

    stuck_offers_gauge_->Set(static_cast<double>(count));

    // [v0.7.46 #8] Track peak concurrent and lifetime delta.
    if (count > stuck_offers_peak_observed_) {
        stuck_offers_peak_observed_ = count;
        if (stuck_offers_peak_gauge_) {
            stuck_offers_peak_gauge_->Set(static_cast<double>(count));
        }
    }
    if (count > stuck_offers_last_observed_ && stuck_offers_total_counter_) {
        stuck_offers_total_counter_->Increment(
            static_cast<double>(count - stuck_offers_last_observed_));
    }
    stuck_offers_last_observed_ = count;
}

void MetricsExporter::update_posting_gates(bool gui, bool breaker,
                                           bool wallet_circuit,
                                           bool flash_crash,
                                           bool xch_recovery, bool dry_run,
                                           bool watchdog,
                                          bool cancels_pending)
{
    // Same locked lifecycle check as every neighbouring updater: shutdown()
    // resets registry_ under this mutex, destroying the gauges, so an
    // unguarded update racing shutdown dereferences stale pointers.
    std::unique_lock lock(mtx_);
    if (!running_) return;

    // [R2 review #12] Children added LAZILY on first use, under the lock:
    // presence of any xop_posting_gate sample in a scrape is then genuine
    // heartbeat evidence, which the GUI's posting_ungated check and the
    // intent-adoption guard both rely on. Eager zeros made
    // posting_gates_published() vacuously true.
    if (!gate_gui_) {
        gate_gui_ = &posting_gate_family_->Add({{"reason", "gui"}});
        gate_breaker_ = &posting_gate_family_->Add({{"reason", "breaker"}});
        gate_wallet_circuit_ =
            &posting_gate_family_->Add({{"reason", "wallet_circuit"}});
        gate_flash_crash_ =
            &posting_gate_family_->Add({{"reason", "flash_crash"}});
        gate_xch_recovery_ =
            &posting_gate_family_->Add({{"reason", "xch_recovery"}});
        gate_dry_run_ = &posting_gate_family_->Add({{"reason", "dry_run"}});
        gate_watchdog_ =
            &posting_gate_family_->Add({{"reason", "watchdog"}});
        gate_cancels_pending_ =
            &posting_gate_family_->Add({{"reason", "cancels_pending"}});
    }
    // [R2 #20] The durable cancel-all gate.
    gate_cancels_pending_->Set(cancels_pending ? 1.0 : 0.0);

    gate_gui_->Set(gui ? 1.0 : 0.0);
    gate_breaker_->Set(breaker ? 1.0 : 0.0);
    gate_wallet_circuit_->Set(wallet_circuit ? 1.0 : 0.0);
    gate_flash_crash_->Set(flash_crash ? 1.0 : 0.0);
    gate_xch_recovery_->Set(xch_recovery ? 1.0 : 0.0);
    gate_dry_run_->Set(dry_run ? 1.0 : 0.0);
    // OR in the sticky flag: a Step 12 snapshot older than the watchdog's
    // own write must not clear it. Restart-only means restart-only.
    if (watchdog) watchdog_sticky_ = true;
    const bool wd = watchdog_sticky_;
    if (gate_watchdog_) gate_watchdog_->Set(wd ? 1.0 : 0.0);
    posting_gated_gauge_->Set(
        (gui || breaker || wallet_circuit || flash_crash || xch_recovery
         || dry_run || wd || cancels_pending) ? 1.0 : 0.0);
}

void MetricsExporter::update_stopdrain_failing(bool failing)
{
    std::unique_lock lock(mtx_);
    if (stopdrain_failing_) {
        stopdrain_failing_->Set(failing ? 1.0 : 0.0);
    }
}

void MetricsExporter::update_peg_status(
    const std::vector<std::tuple<std::string, int, double>>& rows)
{
    std::unique_lock lock(mtx_);
    if (!running_ || !registry_) return;
    if (peg_status_family_ == nullptr) {
        peg_status_family_ = &prometheus::BuildGauge()
            .Name("xop_peg_status")
            .Help("Asset-level peg state: 0 holding, 1 warn, 2 SUSPENDED "
                  "(par ignored, pairs disabled until operator re-enable)")
            .Register(*registry_);
        peg_deviation_family_ = &prometheus::BuildGauge()
            .Name("xop_peg_deviation_pct")
            .Help("Last observed deviation from the declared peg, percent")
            .Register(*registry_);
    }
    for (const auto& [key, status, dev] : rows) {
        const auto bar = key.find('|');
        const std::string symbol = key.substr(0, bar);
        const std::string asset = bar == std::string::npos
            ? std::string{} : key.substr(bar + 1);
        auto& g = peg_status_family_->Add(
            {{"symbol", symbol}, {"asset", asset}});
        g.Set(static_cast<double>(status));
        auto& d = peg_deviation_family_->Add(
            {{"symbol", symbol}, {"asset", asset}});
        d.Set(dev);
    }
}

void MetricsExporter::update_watchdog_gate(bool fired)
{
    std::unique_lock lock(mtx_);
    if (!running_) return;
    // [review] STICKY, under the mutex. Two threads publish this: the
    // watchdog, here, and Step 12 through update_posting_gates(). Step 12
    // reads watchdog_fired_ BEFORE taking this lock, so a snapshot taken as
    // false can arrive after the watchdog has set true and reset both gauges
    // from a stale argument -- clearing a restart-only signal. Remembering it
    // here makes the later writer unable to lose it.
    if (fired) watchdog_sticky_ = true;
    if (gate_watchdog_) gate_watchdog_->Set(watchdog_sticky_ ? 1.0 : 0.0);
    if (watchdog_sticky_) {
        // The aggregate has to move with it, or a consumer watching only
        // xop_posting_gated sees nothing while posting is gated.
        posting_gated_gauge_->Set(1.0);
    }
}

void MetricsExporter::update_bot_paused(bool is_paused)
{
    std::unique_lock lock(mtx_);
    if (!running_) return;

    if (!paused_gauge_) paused_gauge_ = &paused_family_->Add({});
    paused_gauge_->Set(is_paused ? 1.0 : 0.0);
}

void MetricsExporter::update_fees_paid_24h(std::uint64_t total_mojos)
{
    std::unique_lock lock(mtx_);
    if (!running_) return;

    fees_paid_24h_gauge_->Set(static_cast<double>(total_mojos));
}

void MetricsExporter::increment_trade_decision(
    std::string_view strategy,
    std::string_view scenario_id,
    std::string_view result)
{
    std::unique_lock lock(mtx_);
    if (!running_ || !trade_decision_counter_family_) return;

    std::string key;
    key.reserve(strategy.size() + scenario_id.size() + result.size() + 3);
    key.append(strategy);
    key.push_back('\x1f');
    key.append(scenario_id);
    key.push_back('\x1f');
    key.append(result);

    auto it = trade_decision_counters_.find(key);
    if (it == trade_decision_counters_.end()) {
        auto& counter = trade_decision_counter_family_->Add({
            {"strategy", std::string(strategy)},
            {"scenario_id", std::string(scenario_id)},
            {"result", std::string(result)},
        });
        it = trade_decision_counters_.emplace(std::move(key), &counter).first;
    }

    it->second->Increment();
}

}  // namespace xop
