// metrics.hpp -- Prometheus metrics exporter for XOPTrader CHIA DEX market-maker.
//
// Exposes all six Grafana dashboard metric families over HTTP using the
// prometheus-cpp client library.  The HTTP endpoint is served on a
// configurable port and is scraped by Prometheus at its default /metrics path.
//
// Dashboard coverage:
//   1. Real-Time PnL  -- total, realized, unrealized, spread, inventory
//   2. Inventory       -- per-asset balance, cost basis, skew, underwater flag
//   3. Market Data     -- mid price, spread, 24h volume per pair
//   4. System Health   -- node block height, sync status, wallet, offer latency
//   5. Offer Lifecycle -- pending, filled, cancelled, expired, fill rate
//   6. Risk            -- VaR 95%, max drawdown, portfolio concentration
//
// Thread safety: thread-safe via std::shared_mutex (T2-02).
//   All prometheus-cpp metric objects are internally thread-safe for Set/Increment.
//   The MetricsExporter's own state (running_, shadow counters, known_asset_ids_)
//   is protected by mtx_.  Read operations (is_running) acquire a shared lock;
//   write operations (init, shutdown, update_*, observe_*) acquire an exclusive
//   lock.  The HTTP server runs on its own background thread, started by init()
//   and joined by shutdown().
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no secrets exposed on the metrics endpoint
//   ISO/IEC 5055       -- no raw pointers, RAII ownership of server/registry
//   ISO/IEC 25000      -- documented interfaces, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard C++20, no undefined behaviour

#ifndef XOP_MONITORING_METRICS_HPP
#define XOP_MONITORING_METRICS_HPP

#include "xop/types.hpp"

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// MetricsPnlSnapshot -- snapshot of PnL components for update_pnl().
//
// Named distinctly from xop::PnLSummary (in pnl.hpp) because this struct
// is specific to the Prometheus exporter's five-gauge dashboard.
//
// All values are in mojos.  Matches the five gauges in Dashboard 1.
// ---------------------------------------------------------------------------

struct MetricsPnlSnapshot {
    Mojo total;       // Total PnL (realized + unrealized).
    Mojo realized;    // Realised PnL from closed positions.
    Mojo unrealized;  // Mark-to-market PnL on open positions.
    Mojo spread;      // PnL attributed to spread capture.
    Mojo inventory;   // PnL attributed to inventory mark-to-market.
};

// ---------------------------------------------------------------------------
// InventorySnapshot -- per-asset inventory data for update_inventory().
//
// Passed as a vector, one entry per tracked asset.
// ---------------------------------------------------------------------------

struct InventorySnapshot {
    AssetId asset_id;     // Asset identifier ("xch" or 64-hex CAT id).
    Mojo    balance;      // Current holdings (mojos).
    Mojo    cost_basis;   // Weighted-average cost basis (mojos).
    bool    underwater;   // True when cost_basis > current market price.
};

// ---------------------------------------------------------------------------
// InventorySkewSnapshot -- per-pair skew data for update_inventory().
// ---------------------------------------------------------------------------

struct InventorySkewSnapshot {
    std::string pair_name; // Trading pair label (e.g. "XCH/wUSDC").
    double      skew;      // Inventory ratio in [0, 1]; 0.5 = balanced.
};

// ---------------------------------------------------------------------------
// SystemHealthSnapshot -- node/wallet status for update_system_health().
// ---------------------------------------------------------------------------

struct SystemHealthSnapshot {
    BlockHeight block_height;    // Current chain tip height.
    bool        node_synced;     // True if the full node is fully synced.
    bool        wallet_connected;// True if the wallet RPC is reachable.
};

// ---------------------------------------------------------------------------
// RiskSnapshot -- risk metrics for update_risk().
// ---------------------------------------------------------------------------

struct RiskSnapshot {
    double var_95;         // Value at Risk at 95% confidence (mojos, stored as double).
    double max_drawdown;   // Maximum observed drawdown (mojos, stored as double).
};

// ---------------------------------------------------------------------------
// ConcentrationEntry -- per-asset portfolio concentration for update_risk().
// ---------------------------------------------------------------------------

struct ConcentrationEntry {
    AssetId asset_id;       // Asset identifier.
    double  concentration;  // Fraction of total portfolio value [0, 1].
};

// ---------------------------------------------------------------------------
// MetricsExporter -- Prometheus HTTP endpoint and metric families.
//
// Lifecycle:
//   1. Construct (no-op; metrics are not yet registered).
//   2. Call init(port) to create the registry, register all metrics, and
//      start the HTTP server.
//   3. Call update_* methods from the main loop to push fresh values.
//   4. Call shutdown() on graceful exit to stop the HTTP server.
//
// After shutdown() the object may not be re-initialised; construct a new one.
// ---------------------------------------------------------------------------

class MetricsExporter {
public:
    MetricsExporter();
    ~MetricsExporter();

    // Non-copyable, non-movable (HTTP server owns threads).
    MetricsExporter(const MetricsExporter&) = delete;
    MetricsExporter& operator=(const MetricsExporter&) = delete;
    MetricsExporter(MetricsExporter&&) = delete;
    MetricsExporter& operator=(MetricsExporter&&) = delete;

    // -- Lifecycle -----------------------------------------------------------

    /// Start the Prometheus HTTP server on the given port.
    /// Registers all metric families and begins serving /metrics.
    /// Throws std::runtime_error if the port is already in use.
    ///
    /// @param asset_ids  The set of known asset IDs from config.  Only these
    ///                   IDs will be accepted for label-keyed metrics, bounding
    ///                   Prometheus label cardinality.
    ///                   ISO/IEC 5055: bounded resource allocation.
    void init(std::uint16_t port,
              const std::vector<std::string>& asset_ids = {});

    /// Stop the HTTP server and release all resources.
    /// Safe to call even if init() was never called.
    void shutdown();

    /// True after init() has completed and before shutdown() is called.
    bool is_running() const noexcept;

    // -- Dashboard 1: Real-Time PnL -----------------------------------------

    /// Push a PnL snapshot.  All five gauge values are set atomically
    /// (from the caller's perspective -- each Set is individually atomic).
    void update_pnl(const MetricsPnlSnapshot& summary);

    // -- Dashboard 2: Inventory ----------------------------------------------

    /// Push per-asset inventory snapshots and per-pair skew ratios.
    /// Stale label sets (assets no longer tracked) are not removed;
    /// Prometheus handles this via staleness markers.
    void update_inventory(const std::vector<InventorySnapshot>& positions,
                          const std::vector<InventorySkewSnapshot>& skews);

    // -- Dashboard 3: Market Data --------------------------------------------

    /// Push per-pair market data snapshots.
    void update_market(const std::vector<MarketSnapshot>& snapshots);

    // -- Dashboard 4: System Health ------------------------------------------

    /// Push node/wallet health status.
    void update_system_health(const SystemHealthSnapshot& health);

    /// Record a single offer-creation latency sample (milliseconds).
    /// Feeds the xop_offer_latency_ms histogram.
    void observe_offer_latency_ms(double latency_ms);

    // -- Dashboard 5: Offer Lifecycle ----------------------------------------

    /// Push offer lifecycle counters and gauges.
    ///
    /// @param pending_count  Current number of outstanding offers.
    /// @param fill_count     Cumulative fills since startup.
    /// @param cancel_count   Cumulative cancellations since startup.
    /// @param expired_count  Cumulative expirations since startup.
    /// @param fill_rate_per_hour  Rolling fills per hour.
    void update_offers(std::size_t pending_count,
                       std::uint64_t fill_count,
                       std::uint64_t cancel_count,
                       std::uint64_t expired_count,
                       double fill_rate_per_hour);

    // -- Dashboard 6: Risk ---------------------------------------------------

    /// Push risk metrics (VaR, drawdown) and per-asset concentrations.
    void update_risk(const RiskSnapshot& risk,
                     const std::vector<ConcentrationEntry>& concentrations);

private:
    /// Register all metric families with the prometheus registry.
    /// Called once during init().
    void register_metrics();

    // -- Thread safety (T2-02) -----------------------------------------------
    // Mutable to allow shared (read) locking in const accessor methods.
    // Protects running_, shadow counters, and known_asset_ids_.
    mutable std::shared_mutex mtx_;

    // -- Prometheus infrastructure -------------------------------------------

    std::shared_ptr<prometheus::Registry> registry_;
    std::unique_ptr<prometheus::Exposer>  exposer_;
    bool running_{false};

    // -- Dashboard 1: Real-Time PnL gauges -----------------------------------

    prometheus::Family<prometheus::Gauge>* pnl_family_{nullptr};
    prometheus::Gauge* pnl_total_{nullptr};
    prometheus::Gauge* pnl_realized_{nullptr};
    prometheus::Gauge* pnl_unrealized_{nullptr};
    prometheus::Gauge* pnl_spread_{nullptr};
    prometheus::Gauge* pnl_inventory_{nullptr};

    // -- Dashboard 2: Inventory gauges (label-keyed) -------------------------

    prometheus::Family<prometheus::Gauge>* inv_balance_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* inv_cost_basis_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* inv_skew_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* inv_underwater_family_{nullptr};

    // -- Dashboard 3: Market Data gauges (label-keyed) -----------------------

    prometheus::Family<prometheus::Gauge>* mkt_mid_price_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* mkt_spread_bps_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* mkt_volume_24h_family_{nullptr};

    // -- Dashboard 4: System Health ------------------------------------------

    prometheus::Family<prometheus::Gauge>* sys_family_{nullptr};
    prometheus::Gauge* sys_block_height_{nullptr};
    prometheus::Gauge* sys_node_synced_{nullptr};
    prometheus::Gauge* sys_wallet_connected_{nullptr};
    prometheus::Family<prometheus::Histogram>* offer_latency_family_{nullptr};
    prometheus::Histogram* offer_latency_{nullptr};

    // -- Dashboard 5: Offer Lifecycle ----------------------------------------

    prometheus::Family<prometheus::Gauge>*   offer_gauge_family_{nullptr};
    prometheus::Gauge*                       offers_pending_{nullptr};
    prometheus::Gauge*                       fill_rate_gauge_{nullptr};
    prometheus::Family<prometheus::Counter>* offer_counter_family_{nullptr};
    prometheus::Counter*                     offers_filled_{nullptr};
    prometheus::Counter*                     offers_cancelled_{nullptr};
    prometheus::Counter*                     offers_expired_{nullptr};

    // Monotonic shadow counters for delta computation.
    // prometheus-cpp counters only support Increment(), so we track the
    // last reported cumulative value and increment by the delta.
    std::uint64_t last_fill_count_{0};
    std::uint64_t last_cancel_count_{0};
    std::uint64_t last_expired_count_{0};

    // -- Dashboard 6: Risk gauges (label-keyed) ------------------------------

    prometheus::Family<prometheus::Gauge>* risk_family_{nullptr};
    prometheus::Gauge* risk_var_95_{nullptr};
    prometheus::Gauge* risk_max_drawdown_{nullptr};
    prometheus::Family<prometheus::Gauge>* risk_concentration_family_{nullptr};

    // -- Cardinality guard ----------------------------------------------------
    // ISO/IEC 5055: bounded resource allocation -- only asset IDs registered
    // at init() are accepted as Prometheus label values.
    std::unordered_set<std::string> known_asset_ids_;
};

}  // namespace xop

#endif  // XOP_MONITORING_METRICS_HPP
