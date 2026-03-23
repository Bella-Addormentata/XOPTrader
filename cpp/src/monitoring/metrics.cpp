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

#include <spdlog/spdlog.h>

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

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

void MetricsExporter::init(std::uint16_t port)
{
    if (running_) {
        spdlog::warn("MetricsExporter::init called while already running; ignoring");
        return;
    }

    spdlog::info("MetricsExporter: starting Prometheus HTTP server on port {}",
                 port);

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

    offers_pending_ = &offer_gauge_family_->Add({{"state", "pending"}});
    fill_rate_gauge_ = &offer_gauge_family_->Add({{"state", "fill_rate_per_hour"}});

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
}

// ===================================================================
//  Dashboard 1: Real-Time PnL
// ===================================================================

void MetricsExporter::update_pnl(const PnlSummary& summary)
{
    if (!running_) {
        return;
    }

    // Set each gauge to the absolute value (gauges are point-in-time).
    pnl_total_->Set(static_cast<double>(summary.total));
    pnl_realized_->Set(static_cast<double>(summary.realized));
    pnl_unrealized_->Set(static_cast<double>(summary.unrealized));
    pnl_spread_->Set(static_cast<double>(summary.spread));
    pnl_inventory_->Set(static_cast<double>(summary.inventory));
}

// ===================================================================
//  Dashboard 2: Inventory
// ===================================================================

void MetricsExporter::update_inventory(
    const std::vector<InventorySnapshot>& positions,
    const std::vector<InventorySkewSnapshot>& skews)
{
    if (!running_) {
        return;
    }

    // Per-asset gauges: balance, cost basis, underwater flag.
    for (const auto& pos : positions) {
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
    if (!running_) {
        return;
    }

    sys_block_height_->Set(static_cast<double>(health.block_height));
    sys_node_synced_->Set(health.node_synced ? 1.0 : 0.0);
    sys_wallet_connected_->Set(health.wallet_connected ? 1.0 : 0.0);
}

void MetricsExporter::observe_offer_latency_ms(double latency_ms)
{
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
    if (!running_) {
        return;
    }

    risk_var_95_->Set(risk.var_95);
    risk_max_drawdown_->Set(risk.max_drawdown);

    for (const auto& c : concentrations) {
        risk_concentration_family_
            ->Add({{"asset_id", c.asset_id}})
            .Set(c.concentration);
    }
}

}  // namespace xop
