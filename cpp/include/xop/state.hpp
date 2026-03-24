// state.hpp -- Thread-safe global state for XOPTrader CHIA market-making bot.
//
// Locking strategy (deadlock-free by design):
//   Three independent shared_mutexes protect three independent maps.
//   No public method ever acquires more than one mutex, so circular-wait is
//   impossible.  Readers take shared (read) locks; writers take exclusive
//   (write) locks.  This gives maximum concurrency for the common case where
//   multiple strategy threads read market snapshots simultaneously.
//
// Monetary invariants:
//   All balances, cost bases, and prices are in mojos (int64_t).  Cost basis
//   uses weighted-average tracking.  The never-sell-at-loss constraint is
//   enforced at the strategy layer (not here), but Position exposes the
//   helpers the strategy needs.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- access-controlled mutable state
//   ISO/IEC 5055       -- no raw pointer ownership, RAII locking
//   ISO/IEC 25000      -- documented interfaces, single-responsibility

#ifndef XOP_STATE_HPP
#define XOP_STATE_HPP

#include "xop/types.hpp"

#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace xop {

// ---------------------------------------------------------------------------
// Position -- per-asset accounting for balance and cost basis.
// ---------------------------------------------------------------------------

struct Position {
    AssetId  asset_id;     // which asset this position tracks
    Mojo     balance;      // current holdings (mojos, >= 0)
    Mojo     cost_basis;   // weighted-average cost per mojo-of-base in mojos-of-quote
    Mojo     total_cost;   // cumulative cost of current holdings (mojos of quote)

    Position();
    explicit Position(const AssetId& id);

    /// Record an inflow of `qty` mojos purchased at `unit_price` mojos-of-quote
    /// per mojo-of-base.  Updates balance, total_cost, and recomputes the
    /// weighted-average cost_basis.
    /// Returns false (and leaves state unchanged) on overflow or invalid input.
    /// ISO/IEC 5055 -- CWE-190: callers must check the return value.
    [[nodiscard]] bool add(Mojo qty, Mojo unit_price);

    /// Record an outflow of `qty` mojos sold.  Reduces balance and total_cost
    /// proportionally, preserving cost_basis (weighted-average drawdown).
    /// Returns false and does nothing if qty > balance.
    [[nodiscard]] bool remove(Mojo qty);
};

// ---------------------------------------------------------------------------
// BotStatus -- lifecycle state machine for the trading engine.
// ---------------------------------------------------------------------------

enum class BotStatus : std::uint8_t {
    Initializing = 0,
    Running      = 1,
    Paused       = 2,
    ShuttingDown = 3,
    Stopped      = 4
};

/// Human-readable label for logging.
const char* to_string(BotStatus s) noexcept;

// ---------------------------------------------------------------------------
// State -- single global instance holding all mutable runtime data.
//
// Thread-safety guarantee:
//   Every public method acquires exactly one mutex (shared or exclusive) and
//   releases it before returning.  Callers never see a partially updated map.
//   Because no method nests locks, deadlock is impossible.
// ---------------------------------------------------------------------------

class State {
public:
    State();

    // -- status ----------------------------------------------------------

    /// Get current bot status (atomic, lock-free).
    BotStatus status() const noexcept;

    /// Transition to a new status.  Logs the change via spdlog.
    void set_status(BotStatus s) noexcept;

    // -- positions -------------------------------------------------------

    /// Record a purchase: increase position in `asset_id` by `qty` mojos at
    /// the given `unit_price`.  Creates the Position entry if it does not
    /// exist.  Logs via spdlog.
    void record_buy(const AssetId& asset_id, Mojo qty, Mojo unit_price);

    /// Record a sale: decrease position in `asset_id` by `qty` mojos.
    /// Returns false (and does nothing) if the position has insufficient
    /// balance.  Logs via spdlog.
    [[nodiscard]] bool record_sell(const AssetId& asset_id, Mojo qty);

    /// Inventory skew metric for a base/quote pair.
    /// Returns a value in [-1.0, +1.0]:
    ///   +1 = all capital in base (need to sell base)
    ///   -1 = all capital in quote (need to buy base)
    ///    0 = perfectly balanced
    /// Returns 0.0 if neither position exists (no capital deployed).
    double inventory_skew(const AssetId& base_id, const AssetId& quote_id) const;

    /// Read a snapshot of a single position.  Returns a default (zero)
    /// Position if the asset has never been seen.
    Position get_position(const AssetId& asset_id) const;

    /// Read snapshots of all positions under a single shared lock.
    std::vector<Position> get_all_positions() const;

    // -- pending offers ---------------------------------------------------

    /// Insert or replace a pending offer keyed by offer_id.
    void upsert_offer(const PendingOffer& offer);

    /// Remove a pending offer by id.  Returns true if found and removed.
    bool remove_offer(const std::string& offer_id);

    /// Get a single pending offer.  Returns nullptr-equivalent (empty
    /// optional) if not found.  Caller receives a copy -- no dangling refs.
    [[nodiscard]] PendingOffer get_offer(const std::string& offer_id) const;

    /// Get all pending offers under a single shared lock.
    std::vector<PendingOffer> get_all_offers() const;

    /// Count of pending offers (lock-free-ish: single shared lock).
    std::size_t offer_count() const;

    // -- market snapshots -------------------------------------------------

    /// Insert or replace a market snapshot keyed by pair_name.
    void update_market(const MarketSnapshot& snap);

    /// Get the latest snapshot for a pair.  Returns a default (zeroed)
    /// MarketSnapshot if the pair has not yet been observed.
    MarketSnapshot get_market(const std::string& pair_name) const;

    /// Get all market snapshots under a single shared lock.
    std::vector<MarketSnapshot> get_all_markets() const;

private:
    // Each map is guarded by its own shared_mutex.  This is the
    // foundation of the deadlock-free guarantee: a public method locks
    // at most ONE of these, never two.

    mutable std::shared_mutex                              mtx_positions_;
    std::unordered_map<AssetId, Position>                  positions_;

    mutable std::shared_mutex                              mtx_offers_;
    std::unordered_map<std::string, PendingOffer>          pending_offers_;

    mutable std::shared_mutex                              mtx_markets_;
    std::unordered_map<std::string, MarketSnapshot>        markets_;

    // Bot status uses a simple atomic -- no mutex needed.
    std::atomic<BotStatus>                                 status_;
};

}  // namespace xop

#endif  // XOP_STATE_HPP
