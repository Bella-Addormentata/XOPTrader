// backtest.hpp -- Event-driven backtesting framework for XOPTrader CHIA DEX
//                 market-making strategies.
//
// Provides:
//   1. BacktestEngine     -- replay historical block data through a strategy,
//                            simulating fills against recorded order flow.
//   2. Monte Carlo module -- generate N synthetic price paths from historical
//                            volatility and run full strategy simulations.
//   3. Walk-forward optimizer -- split data into rolling train/test windows
//                                to prevent parameter overfitting.
//   4. Parameter sweep    -- grid search over (gamma, kappa, phi, spread, TTL)
//                            parallelised across CPU cores or optionally CUDA.
//
// Fill simulation models CHIA's offer-take mechanics:
//   - Offers sit passively until a taker completes the spend bundle.
//   - Our bid is filled if bid >= lowest historical sell in that block.
//   - Our ask is filled if ask <= highest historical buy in that block.
//   - Fills are atomic (all-or-nothing) and settle in one block (~52 s).
//   - Partial fills are NOT supported (CHIA offers are indivisible).
//
// Anti-overfitting:
//   - Walk-forward uses 70/30 train/test splits with a rolling 1-week advance.
//   - Parameters must be profitable on BOTH splits to be accepted.
//   - Monte Carlo validates that profitability is not path-dependent.
//   - The no_loss_constraint can be toggled to verify zero loss trades.
//
// Optional GPU acceleration:
//   - Controlled at compile time by the XOP_ENABLE_CUDA preprocessor flag.
//   - When enabled, Monte Carlo paths run on the GPU via CUDA kernels.
//   - When disabled, all code compiles as standard C++20 with no CUDA deps.
//
// Compliant with:
//   ISO/IEC 27001:2022  -- no secrets; historical data only
//   ISO/IEC 5055        -- no raw pointers; bounds-checked containers
//   ISO/IEC 25000       -- documented interfaces, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard-conforming C++20 (CUDA extension optional)

#ifndef XOP_BACKTEST_HPP
#define XOP_BACKTEST_HPP

#include <xop/types.hpp>
#include <xop/config.hpp>
#include <xop/strategy/base.hpp>
#include <xop/data/volatility.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// Forward declarations for optional CUDA acceleration.
// ---------------------------------------------------------------------------
#ifdef XOP_ENABLE_CUDA
namespace cuda {
    struct MonteCarloGpuContext;
}  // namespace cuda
#endif

// ---------------------------------------------------------------------------
// HistoricalOffer -- a single dexie offer from the JSON historical data.
//
// Represents an offer that was posted and either filled or expired during the
// backtest period.  The engine uses these to reconstruct per-block order flow.
// ---------------------------------------------------------------------------

struct HistoricalOffer {
    std::string offer_id;       // Unique identifier from dexie API.
    std::string pair_name;      // Trading pair, e.g. "XCH/wUSDC".
    Side        side;           // Bid (buy) or Ask (sell) from taker perspective.
    double      price;          // Price in quote per base (double for JSON compat).
    double      size;           // Quantity in base asset units.
    BlockHeight created_block;  // Block at which the offer was first observed.
    BlockHeight filled_block;   // Block at which the offer was taken (0 if expired).
    bool        was_filled;     // True if this offer was taken by a counterparty.
};

// ---------------------------------------------------------------------------
// CexCandle -- CEX reference price bar from CSV data.
//
// Format: timestamp, open, high, low, close, volume.
// Used to establish the "true" mid-price for strategy evaluation and to
// detect adverse selection (our fill price vs. subsequent CEX price move).
// ---------------------------------------------------------------------------

struct CexCandle {
    double      timestamp;  // Unix epoch seconds (double for sub-second CSV).
    double      open;       // Open price (quote per base).
    double      high;       // High price (quote per base).
    double      low;        // Low price (quote per base).
    double      close;      // Close price (quote per base).
    double      volume;     // Volume in base asset units.
    BlockHeight block;      // Nearest CHIA block height (assigned during load).
};

// ---------------------------------------------------------------------------
// BlockData -- aggregated state at a single block height, reconstructed
//              from historical offers, CEX candles, and on-chain data.
//
// This is the fundamental simulation tick.  The engine iterates through
// blocks chronologically, and at each block the strategy produces quotes
// which are then matched against the historical order flow.
// ---------------------------------------------------------------------------

struct BlockData {
    BlockHeight height;             // Block height on the CHIA chain.
    double      timestamp;          // Unix epoch seconds.

    // CEX reference price at this block (interpolated from candle data).
    double      cex_mid;            // Mid-price from CEX.
    double      cex_bid;            // Best bid from CEX (approximated from OHLC).
    double      cex_ask;            // Best ask from CEX (approximated from OHLC).

    // On-chain order flow at this block (offers that were taken).
    double      lowest_sell;        // Lowest ask price filled in this block
                                    //   (infinity if no sells occurred).
    double      highest_buy;        // Highest bid price filled in this block
                                    //   (0.0 if no buys occurred).
    double      total_buy_volume;   // Total base-asset volume bought this block.
    double      total_sell_volume;  // Total base-asset volume sold this block.

    // Volatility snapshot for this block (from the historical candle series).
    double      sigma_block;        // Per-block volatility (annualised sigma
                                    //   is sigma_block / sqrt(block_time/year)).

    // Count of offers posted and taken at this block.
    std::uint32_t offers_posted;    // New offers observed at this height.
    std::uint32_t offers_taken;     // Offers settled at this height.
};

// ---------------------------------------------------------------------------
// SimulatedFill -- a fill generated during backtesting.
//
// Records the full context of the simulated trade for attribution analysis.
// ---------------------------------------------------------------------------

struct SimulatedFill {
    BlockHeight  block;          // Block at which the fill occurred.
    Side         side;           // Bid (we bought) or Ask (we sold).
    double       price;          // Fill price (our quote price).
    double       size;           // Quantity filled (base asset units).
    double       cex_mid_at_fill;// CEX mid at time of fill (for adverse selection).
    double       cost_basis;     // Our cost basis at time of fill (sells only).
    double       pnl;            // Realized PnL from this fill (sells only; 0 for buys).
    bool         was_loss;       // True if pnl < 0 (constraint violation flag).
};

// ---------------------------------------------------------------------------
// BacktestResult -- comprehensive output from a single backtest run.
//
// Contains all metrics required for strategy evaluation and the full
// equity curve for visualization.
// ---------------------------------------------------------------------------

struct BacktestResult {
    // -- Profitability metrics -----------------------------------------------
    double total_pnl;           // Net realized + unrealized PnL.
    double realized_pnl;        // Sum of realized PnL from all closed fills.
    double unrealized_pnl;      // Mark-to-market PnL on open inventory.
    double sharpe;              // Annualised Sharpe ratio (block-level returns).
    double max_drawdown;        // Maximum peak-to-trough drawdown (fraction).
    double profit_factor;       // Gross profit / gross loss (inf if no losses).

    // -- Fill statistics -----------------------------------------------------
    std::uint32_t fill_count;   // Total number of fills (both sides).
    std::uint32_t bid_fills;    // Number of bid (buy) fills.
    std::uint32_t ask_fills;    // Number of ask (sell) fills.
    double        fill_rate;    // fills / total_blocks (how often we get hit).

    // -- Inventory metrics ---------------------------------------------------
    double inventory_turnover;  // total_volume_traded / average_inventory.
    double adverse_selection;   // Fraction of fills followed by an adverse move.

    // -- Hard constraint: never-sell-at-loss ----------------------------------
    std::uint32_t loss_trade_count;  // Number of sells below cost basis.
                                     // MUST be 0 when no_loss_constraint is on.

    // -- Time analysis -------------------------------------------------------
    double time_underwater_pct; // Fraction of blocks with negative unrealized PnL.

    // -- Parameter identification --------------------------------------------
    StrategyConfig parameter_set;  // The parameters used for this run.

    // -- Equity curve --------------------------------------------------------
    std::vector<double> equity_curve;  // Cumulative PnL at each block.

    // -- Fill log (optional, can be large) -----------------------------------
    std::vector<SimulatedFill> fills;  // Complete record of every simulated fill.

    // -- Block count ---------------------------------------------------------
    std::uint32_t total_blocks;  // Number of blocks simulated.
};

// ---------------------------------------------------------------------------
// MonteCarloResult -- aggregate statistics from N Monte Carlo price paths.
//
// Each path generates a full BacktestResult; this struct summarizes the
// distribution of outcomes across paths.
// ---------------------------------------------------------------------------

struct MonteCarloResult {
    std::uint32_t n_paths;          // Number of paths simulated.

    // -- PnL distribution ----------------------------------------------------
    double pnl_mean;                // Mean total PnL across paths.
    double pnl_median;              // Median total PnL.
    double pnl_std;                 // Standard deviation of total PnL.
    double pnl_5th_percentile;      // 5th percentile (VaR-like).
    double pnl_95th_percentile;     // 95th percentile.

    // -- Sharpe distribution -------------------------------------------------
    double sharpe_mean;             // Mean Sharpe ratio across paths.
    double sharpe_median;           // Median Sharpe ratio.

    // -- Drawdown distribution -----------------------------------------------
    double max_drawdown_mean;       // Mean max drawdown.
    double max_drawdown_worst;      // Worst-case max drawdown across all paths.
    double max_drawdown_95th;       // 95th percentile max drawdown.

    // -- Probability metrics -------------------------------------------------
    double prob_profitable;         // Fraction of paths with positive total PnL.
    double prob_loss_trade;         // Fraction of paths with any loss_trade_count > 0.

    // -- Per-path results (optionally retained) ------------------------------
    std::vector<BacktestResult> path_results;
};

// ---------------------------------------------------------------------------
// ParameterSet -- a single point in the grid search space.
//
// Bundles the key tuning knobs from the strategy document (Section 5 & 6).
// ---------------------------------------------------------------------------

struct ParameterSet {
    double   gamma;                 // Risk aversion [0.001 - 0.1].
    double   kappa;                 // Fill intensity decay [0.5 - 5.0].
    double   phi;                   // GLFT inventory skew [0.1 - 2.0].
    double   spread_bps;            // Fixed spread component (bps) [20 - 200].
    uint32_t ttl_blocks;            // Offer time-to-live [10 - 300].
    double   min_margin_bps;        // Minimum profit margin (bps) [10 - 100].
};

// ---------------------------------------------------------------------------
// OptimizationResult -- output from walk-forward or grid-search optimization.
//
// Contains the best parameter set, its train/test performance, and the full
// grid of results for analysis.
// ---------------------------------------------------------------------------

struct OptimizationResult {
    // -- Best parameters found -----------------------------------------------
    ParameterSet best_params;          // Parameter set with best combined score.
    BacktestResult best_train_result;  // Performance on the training window.
    BacktestResult best_test_result;   // Performance on the test window.

    // -- Full grid results ---------------------------------------------------
    struct GridEntry {
        ParameterSet   params;
        BacktestResult train_result;
        BacktestResult test_result;
        double         combined_score;  // Weighted train + test metric.
        bool           accepted;        // Profitable on both train AND test.
    };
    std::vector<GridEntry> grid;       // All tested parameter combinations.

    // -- Walk-forward windows ------------------------------------------------
    struct WindowResult {
        BlockHeight    train_start;
        BlockHeight    train_end;
        BlockHeight    test_start;
        BlockHeight    test_end;
        ParameterSet   best_params;
        double         train_sharpe;
        double         test_sharpe;
        bool           accepted;       // Profitable on both splits.
    };
    std::vector<WindowResult> windows; // Results from each rolling window.

    // -- Summary -------------------------------------------------------------
    double acceptance_rate;            // Fraction of grid entries that passed.
    double mean_test_sharpe;           // Mean Sharpe on test sets (accepted only).
    double robustness_score;           // Fraction of walk-forward windows accepted.
};

// ---------------------------------------------------------------------------
// BacktestEngine -- the main simulation engine.
//
// Loads historical data, replays blocks through a strategy, simulates fills,
// and computes comprehensive performance metrics.
//
// Thread safety: the engine itself is NOT thread-safe.  Parameter sweeps
//   use internal parallelization (std::execution or thread pool).  Each
//   parallel task creates its own engine instance to avoid contention.
//
// Usage:
//   BacktestEngine engine;
//   engine.load_data("dexie_offers.json", "cex_prices.csv");
//   auto result = engine.run(strategy, params);
//   engine.export_results(result, "output.csv");
// ---------------------------------------------------------------------------

class BacktestEngine {
public:
    // -- Construction -------------------------------------------------------

    BacktestEngine();
    ~BacktestEngine();

    // Non-copyable (holds large data buffers); movable.
    BacktestEngine(const BacktestEngine&) = delete;
    BacktestEngine& operator=(const BacktestEngine&) = delete;
    BacktestEngine(BacktestEngine&&) noexcept;
    BacktestEngine& operator=(BacktestEngine&&) noexcept;

    // -- Data Loading -------------------------------------------------------

    /// Load historical offer data from a dexie API JSON export and CEX
    /// reference prices from a CSV file.  Optionally load on-chain block
    /// timestamps from a separate CSV.
    ///
    /// The JSON format follows the dexie /v1/offers endpoint schema:
    ///   [ { "id": "...", "offered": [...], "requested": [...],
    ///       "status": "completed", "block_height": N, ... }, ... ]
    ///
    /// The CEX CSV format is: timestamp,open,high,low,close,volume
    ///   (one row per candle, typically 1-minute bars).
    ///
    /// On-chain block CSV format: block_height,timestamp
    ///   (optional -- if omitted, block timestamps are estimated from
    ///    height * 52 seconds plus a base offset).
    ///
    /// @param dexie_json_path   Path to the dexie historical offers JSON.
    /// @param cex_csv_path      Path to the CEX OHLCV CSV.
    /// @param block_csv_path    Path to block-height-to-timestamp CSV (optional).
    ///
    /// @throws std::runtime_error on file I/O or parse errors.
    void load_data(const std::string& dexie_json_path,
                   const std::string& cex_csv_path,
                   const std::string& block_csv_path = "");

    /// Return the number of blocks loaded after load_data().
    std::size_t block_count() const noexcept;

    /// Return the block height range [first, last].
    /// Returns {0, 0} if no data is loaded.
    std::pair<BlockHeight, BlockHeight> block_range() const noexcept;

    // -- Single Run ---------------------------------------------------------

    /// Run a full backtest over the loaded data using the given strategy
    /// factory and parameter set.
    ///
    /// The strategy_factory creates a fresh StrategyBase instance configured
    /// with the supplied StrategyConfig.  This allows the engine to
    /// instantiate strategies without knowing the concrete type.
    ///
    /// @param strategy_factory  Callable that creates a new strategy instance.
    /// @param params            Strategy parameters for this run.
    /// @param no_loss_constraint  When true, the ask is floored at cost_basis +
    ///                            min_margin (enforcing never-sell-at-loss).
    ///
    /// @return BacktestResult with full metrics and equity curve.
    ///
    /// @throws std::runtime_error if no data has been loaded.
    using StrategyFactory = std::function<
        std::unique_ptr<StrategyBase>(const StrategyConfig&)>;

    BacktestResult run(const StrategyFactory& strategy_factory,
                       const StrategyConfig&  params,
                       bool no_loss_constraint = true);

    /// Overload that takes a pre-constructed strategy (caller retains
    /// ownership; the engine only reads from it via compute_quotes).
    /// WARNING: not safe for parallel use -- use the factory overload for
    /// parameter sweeps.
    BacktestResult run(StrategyBase&         strategy,
                       const StrategyConfig& params,
                       bool no_loss_constraint = true);

    // -- Monte Carlo Simulation ---------------------------------------------

    /// Run Monte Carlo simulation: generate n_paths synthetic price paths
    /// from the historical volatility distribution and run a full strategy
    /// simulation on each path.
    ///
    /// Price path generation (Geometric Brownian Motion with calibrated vol):
    ///   S_{t+1} = S_t * exp((mu - 0.5*sigma^2)*dt + sigma*sqrt(dt)*Z)
    ///   where Z ~ N(0,1), dt = 52 seconds, sigma from historical YZ estimator.
    ///
    /// The historical mean return (mu) is set to zero by default (conservative
    /// -- assumes no drift) to avoid fitting to a particular trend.
    ///
    /// @param strategy_factory  Factory for creating strategy instances.
    /// @param params            Strategy parameters.
    /// @param n_paths           Number of Monte Carlo paths to simulate.
    /// @param seed              RNG seed for reproducibility (0 = random seed).
    /// @param retain_paths      If true, store per-path BacktestResults
    ///                          in the MonteCarloResult (memory-intensive).
    ///
    /// @return MonteCarloResult with distributional statistics.
    MonteCarloResult run_monte_carlo(const StrategyFactory& strategy_factory,
                                     const StrategyConfig&  params,
                                     std::uint32_t          n_paths = 1000,
                                     std::uint64_t          seed    = 42,
                                     bool retain_paths              = false);

    // -- Walk-Forward Optimization ------------------------------------------

    /// Walk-forward optimization with rolling train/test splits.
    ///
    /// Algorithm:
    ///   1. Divide the data into windows of `train_blocks + test_blocks`.
    ///   2. For each window:
    ///      a. Grid-search over param_grid on the training portion.
    ///      b. Select the parameter set with the best training Sharpe.
    ///      c. Validate on the test portion.
    ///      d. Accept only if profitable on BOTH train and test.
    ///   3. Advance the window by `advance_blocks` and repeat.
    ///   4. Report the parameter set accepted by the most windows.
    ///
    /// This procedure prevents overfitting because:
    ///   - Parameters are never tested on data they were optimized on.
    ///   - Rolling windows ensure temporal robustness.
    ///   - Dual-profitability requirement filters lucky overfit parameters.
    ///
    /// @param strategy_factory  Factory for creating strategy instances.
    /// @param param_grid        Vector of ParameterSets to evaluate.
    /// @param train_pct         Fraction of each window used for training (0.7).
    /// @param advance_blocks    How many blocks to advance per window step
    ///                          (default: ~1 week = 7*24*3600/52 = 11,631 blocks).
    /// @param min_train_sharpe  Minimum Sharpe on training set to consider (0.5).
    ///
    /// @return OptimizationResult with best params, grid, and window results.
    OptimizationResult walk_forward_optimize(
        const StrategyFactory&        strategy_factory,
        const std::vector<ParameterSet>& param_grid,
        double                        train_pct       = 0.70,
        std::uint32_t                 advance_blocks  = 11631,
        double                        min_train_sharpe = 0.5);

    // -- Parameter Sweep (parallelised) -------------------------------------

    /// Exhaustive grid search over a set of parameter combinations.
    ///
    /// Each combination is evaluated independently and can run in parallel
    /// across CPU cores using std::execution::par_unseq (C++17).
    ///
    /// Results are sorted by Sharpe ratio (descending).
    ///
    /// @param strategy_factory  Factory for creating strategy instances.
    /// @param param_grid        Vector of ParameterSets to evaluate.
    /// @param max_threads       Maximum number of parallel threads (0 = auto).
    ///
    /// @return Vector of BacktestResults, one per parameter combination,
    ///         sorted by descending Sharpe.
    std::vector<BacktestResult> parameter_sweep(
        const StrategyFactory&           strategy_factory,
        const std::vector<ParameterSet>& param_grid,
        std::uint32_t                    max_threads = 0);

    // -- Export --------------------------------------------------------------

    /// Export backtest results to CSV files.
    ///
    /// Creates two files:
    ///   <base_path>_summary.csv  -- one row with all scalar metrics.
    ///   <base_path>_equity.csv   -- one row per block (block_height, equity).
    ///
    /// @param result    The BacktestResult to export.
    /// @param base_path Output path prefix (without extension).
    void export_results(const BacktestResult& result,
                        const std::string&    base_path) const;

    /// Export Monte Carlo results to CSV.
    ///
    /// Creates:
    ///   <base_path>_mc_summary.csv -- distributional statistics.
    ///   <base_path>_mc_paths.csv   -- per-path total_pnl and sharpe
    ///                                 (if path_results were retained).
    void export_results(const MonteCarloResult& result,
                        const std::string&      base_path) const;

    /// Export optimization results to CSV.
    ///
    /// Creates:
    ///   <base_path>_opt_grid.csv    -- full grid with train/test metrics.
    ///   <base_path>_opt_windows.csv -- per-window walk-forward results.
    void export_results(const OptimizationResult& result,
                        const std::string&        base_path) const;

    // -- Configuration ------------------------------------------------------

    /// Set the number of blocks per CHIA session for tau computation.
    /// Default: 120 blocks (~1.73 hours), matching AvellanedaConfig.
    void set_horizon_blocks(std::uint32_t n) noexcept;

    /// Set the assumed block time in seconds (default: 52.0).
    void set_block_time(double seconds) noexcept;

    /// Set initial capital for the simulation (in quote asset units).
    void set_initial_capital(double capital) noexcept;

    /// Enable or disable CUDA acceleration for Monte Carlo.
    /// Has no effect if XOP_ENABLE_CUDA is not defined at compile time.
    void set_cuda_enabled(bool enabled) noexcept;

    /// Return true if CUDA acceleration is compiled in AND enabled.
    bool cuda_available() const noexcept;

private:
    // -- Internal simulation helpers ----------------------------------------

    /// Run the event-driven simulation over a sub-range of blocks.
    /// Used by both run() and walk-forward to evaluate on arbitrary windows.
    BacktestResult simulate_range(StrategyBase& strategy,
                                  const StrategyConfig& params,
                                  std::size_t start_idx,
                                  std::size_t end_idx,
                                  bool no_loss_constraint) const;

    /// Check whether our bid would be filled against historical sells.
    ///   Fill condition: our_bid >= block.lowest_sell.
    ///
    /// This models the CHIA offer-take mechanic: our bid offer is taken if
    /// a seller accepts a price at or below our bid.  We compare against the
    /// lowest sell price observed in the block because that represents the
    /// cheapest available liquidity that a rational taker would accept.
    ///
    /// Limitation: this assumes our presence does not change the historical
    /// order flow (zero market impact).  For CHIA's current low volume this
    /// is a reasonable approximation, but would understate adverse selection
    /// in a higher-volume regime.
    bool would_fill_bid(double our_bid, const BlockData& block) const noexcept;

    /// Check whether our ask would be filled against historical buys.
    ///   Fill condition: our_ask <= block.highest_buy.
    bool would_fill_ask(double our_ask, const BlockData& block) const noexcept;

    /// Compute the Sharpe ratio from a vector of per-block returns.
    /// Uses annualisation factor based on configured block_time.
    static double compute_sharpe(const std::vector<double>& returns,
                                 double block_time_seconds);

    /// Compute the maximum drawdown from an equity curve.
    static double compute_max_drawdown(const std::vector<double>& equity);

    /// Convert a ParameterSet into a StrategyConfig for the strategy factory.
    static StrategyConfig params_to_config(const ParameterSet& ps,
                                           const StrategyConfig& base);

    /// Build BlockData structures from loaded raw data.
    /// Called by load_data() after parsing all files.
    void build_blocks();

    /// Generate a single synthetic price path using GBM.
    /// @param n_blocks  Number of blocks to generate.
    /// @param s0        Starting price.
    /// @param sigma     Per-block volatility.
    /// @param rng       Mersenne Twister random engine (caller owns state).
    /// @return Vector of prices, one per block.
    static std::vector<double> generate_price_path(
        std::size_t n_blocks,
        double s0,
        double sigma,
        std::mt19937& rng);

    /// Build synthetic BlockData from a generated price path.
    /// Injects synthetic order flow calibrated from historical fill rates.
    std::vector<BlockData> build_synthetic_blocks(
        const std::vector<double>& prices,
        double historical_fill_rate) const;

    // -- Data storage -------------------------------------------------------

    /// Chronologically sorted block-level simulation data.
    std::vector<BlockData> blocks_;

    /// Raw loaded data (retained for walk-forward sub-ranging).
    std::vector<HistoricalOffer> raw_offers_;
    std::vector<CexCandle>       raw_candles_;

    /// Block height to timestamp mapping (from on-chain data or estimation).
    std::vector<std::pair<BlockHeight, double>> block_timestamps_;

    // -- Configuration ------------------------------------------------------

    std::uint32_t horizon_blocks_{120};    // Rolling strategy horizon.
    double        block_time_seconds_{52.0}; // Mean inter-block interval.
    double        initial_capital_{10000.0};  // Starting capital (quote units).
    bool          cuda_enabled_{false};      // Use GPU for Monte Carlo.

    // -- Volatility estimator (trained from historical data) ----------------

    /// Historical per-block volatility series, computed during load_data().
    std::vector<double> historical_sigmas_;

    /// Mean and std of historical volatility (for MC path calibration).
    double sigma_mean_{0.0};
    double sigma_std_{0.0};

    /// Historical fill rate (fills per block), calibrated from loaded data.
    double historical_fill_rate_{0.0};

    // -- Mutex for export serialization (export is the only shared op) ------
    mutable std::mutex export_mtx_;

#ifdef XOP_ENABLE_CUDA
    /// GPU context for accelerated Monte Carlo (lazy-initialized).
    std::unique_ptr<cuda::MonteCarloGpuContext> gpu_ctx_;
#endif
};

// ---------------------------------------------------------------------------
// Utility: generate a parameter grid from ranges.
//
// Creates the Cartesian product of the supplied ranges for each parameter.
// Useful for constructing the param_grid argument to walk_forward_optimize
// or parameter_sweep.
//
// Example:
//   auto grid = make_parameter_grid(
//       {0.005, 0.01, 0.02},    // gamma
//       {1.0, 1.5, 2.0},        // kappa
//       {0.3, 0.5, 0.7},        // phi
//       {40.0, 60.0, 80.0},     // spread_bps
//       {30, 60, 120},          // ttl_blocks
//       {25.0, 35.0, 50.0}      // min_margin_bps
//   );
//   // grid.size() == 3^6 == 729 combinations.
// ---------------------------------------------------------------------------

std::vector<ParameterSet> make_parameter_grid(
    const std::vector<double>&   gamma_values,
    const std::vector<double>&   kappa_values,
    const std::vector<double>&   phi_values,
    const std::vector<double>&   spread_bps_values,
    const std::vector<uint32_t>& ttl_values,
    const std::vector<double>&   min_margin_bps_values);

}  // namespace xop

#endif  // XOP_BACKTEST_HPP
