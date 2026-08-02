// config.hpp -- Configuration data structures for XOPTrader CHIA DEX market-maker.
//
// All configuration is loaded from a YAML file via yaml-cpp. Structures are
// plain-data aggregates with no heap indirection, facilitating value semantics
// and straightforward serialisation. Field names mirror the YAML schema
// defined in config.example.yaml so that the mapping is unsurprising.
//
// Security: SSL certificate paths, wallet fingerprints, and Telegram tokens
//           are classified as secrets and are excluded from any log output.
//
// ISO/IEC 27001:2022 -- secrets handling, least-privilege logging.
// ISO/IEC 5055       -- no raw pointers, bounds-checked containers.
// ISO/IEC 25000      -- clear naming, complete documentation.

#ifndef XOP_CONFIG_HPP
#define XOP_CONFIG_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

namespace xop {

// ---------------------------------------------------------------------------
// Exception thrown when a configuration file is missing, malformed, or
// contains values outside their valid domain.
// ---------------------------------------------------------------------------
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Chia node operating mode.
//   Auto:       Attempt full_node first; fall back to wallet_only if the
//               full node is unreachable at startup (recommended).
//   FullNode:   Require a running Chia full node — abort if unavailable.
//   WalletOnly: Run with the Chia wallet service only — no full node
//               required.  Block height is obtained from the wallet's
//               synced view (get_height_info), and fee estimation falls
//               back to static fees.
// ---------------------------------------------------------------------------
enum class ChiaMode : std::uint8_t {
    Auto       = 0,
    FullNode   = 1,
    WalletOnly = 2
};

/// Human-readable label for logging.
inline const char* to_string(ChiaMode m) noexcept {
    switch (m) {
        case ChiaMode::Auto:       return "auto";
        case ChiaMode::FullNode:   return "full_node";
        case ChiaMode::WalletOnly: return "wallet_only";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Chia blockchain RPC connectivity and authentication.
// Covers both the full-node and wallet daemon endpoints.
// ---------------------------------------------------------------------------
struct ChiaConfig {
    ChiaMode    mode{ChiaMode::Auto};   // Node operating mode (auto/full_node/wallet_only).
    std::string full_node_host;         // Full-node RPC hostname.
    uint16_t    full_node_port{8555};   // Full-node RPC port (default 8555).
    std::string wallet_host;            // Wallet RPC hostname.
    uint16_t    wallet_port{9256};      // Wallet RPC port (default 9256).

    // SSL mutual-auth certificate/key for the full-node endpoint.
    std::string ssl_cert_path;          // SECRET -- never log.
    std::string ssl_key_path;           // SECRET -- never log.

    // SSL mutual-auth certificate/key for the wallet endpoint.
    std::string wallet_cert_path;       // SECRET -- never log.
    std::string wallet_key_path;        // SECRET -- never log.

    // CA certificate used to verify the Chia daemon's server certificate.
    // Required when verify_ssl is true (the default).
    std::string ca_cert_path;           // SECRET -- never log.

    // SSL certificate verification policy for Chia RPC.
    // Keep true in normal operation; set false only for trusted localhost
    // troubleshooting when certificate validation is failing.
    bool        verify_ssl{true};

    // Wallet fingerprint that identifies the key to use.
    uint32_t    wallet_fingerprint{0};  // SECRET -- never log.
};

// ---------------------------------------------------------------------------
// Dexie aggregator API settings.
// Rate limiting is enforced per the published ceiling (50 req / 10 s).
// ---------------------------------------------------------------------------
struct DexieConfig {
    std::string api_base{"https://api.dexie.space/v1"};
    uint32_t    max_requests_per_10s{50};  // Must be >= 1.
    bool        claim_rewards{true};       // Auto-claim DBX liquidity rewards on offer submission.
};

// ---------------------------------------------------------------------------
// A single trading pair the bot may market-make.
// asset IDs are 64-character lower-hex strings except for native XCH which
// uses the literal "xch".
// ---------------------------------------------------------------------------
struct PairConfig {
    std::string base_asset_id;   // e.g. "xch".
    std::string quote_asset_id;  // e.g. 64-hex CAT asset ID.
    std::string name;            // Human-readable label, e.g. "XCH/wUSDC".
    bool        enabled{true};   // Inactive pairs are loaded but skipped.

    /// Mojos-per-displayable-unit for the base asset.
    /// XCH: 10^12 (1 XCH = 1 000 000 000 000 mojos).
    /// CAT: 10^3  (1 CAT unit = 1 000 mojos).
    /// ISO/IEC 5055: explicit denomination prevents silent truncation when
    /// a CAT amount is divided by the XCH constant (off by 10^9).
    std::int64_t base_mojos_per_unit{1'000'000'000'000LL};

    /// Mojos-per-displayable-unit for the quote asset.
    /// Same convention as base_mojos_per_unit.
    std::int64_t quote_mojos_per_unit{1'000LL};

    // -- Per-pair strategy overrides ----------------------------------------
    // When set, these override the global StrategyConfig values for this
    // pair only.  Allows stablecoin pairs (e.g. BYC/wUSDC.b) to use
    // tighter spreads and lower risk aversion than volatile pairs.
    std::optional<double>   gamma_override;
    std::optional<double>   kappa_override;
    std::optional<double>   phi_override;
    std::optional<double>   q_max_override;
    std::optional<double>   min_profit_margin_bps_override;
    std::optional<std::vector<double>> tier_spacing_bps_override;
    std::optional<std::vector<double>> tier_size_pct_override;
    std::optional<double>   max_half_spread_bps_override;
    std::optional<double>   min_offer_size_units_override;

    // -- Stablecoin peg configuration ---------------------------------------
    // When is_stablecoin is true, the depeg detector monitors this pair
    // and can flag it as suspected-failed, pulling all quotes.
    bool   is_stablecoin{false};
    double peg_target{1.0};               // Expected trading price.
    double depeg_warn_pct{2.0};           // Warn when >2% off peg.
    double depeg_bail_pct{10.0};          // Bail out (pull quotes) when >10% off peg.
    uint32_t depeg_sustained_blocks{30};  // Must persist N blocks before bail (~26 min).

    // -- Stablecoin trading overrides ---------------------------------------
    double peg_anchor_threshold_pct{1.0};   // Dev pct for peg-anchor blending.
    double peg_anchor_weight{0.50};         // Weight of peg in blend (0-1).
    bool   stablecoin_exempt_buyonly{false}; // Exempt from XCH-buy-only skip.
    bool   stablecoin_undercut_all_tiers{false}; // Competitive undercut on all tiers.
    bool   stablecoin_flat_sizing{false};    // Skip adverse-selection sizing.
    bool   stablecoin_skip_gap_aware{false}; // Skip gap-aware spacing.
};

// ---------------------------------------------------------------------------
// Core Avellaneda-Stoikov / GLFT market-making algorithm parameters.
//
// gamma  -- risk aversion coefficient (controls spread width).
// kappa  -- fill intensity decay (higher = less impact of spread on fills).
// phi    -- GLFT inventory skew strength (skew = phi * q / q_max).
// q_max  -- maximum tolerated inventory in base-asset units.
//
// Multi-tier offer ladder:
//   num_tiers         -- how many price levels per side.
//   tier_spacing_bps  -- spread from mid-price for each tier, in basis points.
//   tier_size_pct     -- fraction of allocated capital placed at each tier.
//                        Values must sum to approximately 1.0.
// ---------------------------------------------------------------------------
struct StrategyConfig {
    double   gamma{0.01};
    double   kappa{1.5};
    double   phi{0.5};
    double   q_max{1000.0};
    double   min_profit_margin_bps{35.0};   // Never ask below cost + this.
    uint32_t offer_ttl_blocks{60};          // Cancel stale offers after N blocks.
    uint32_t num_tiers{4};                  // Tier count per side.
    std::vector<double> tier_spacing_bps;   // Length == num_tiers.
    std::vector<double> tier_size_pct;      // Length == num_tiers, sum ~= 1.0.

    /// Global cap on half-spread (bps) after all compounding multipliers.
    /// Prevents the multiplicative chain (regime * whale * VPIN * OFI * tactic)
    /// from producing effective market withdrawal.
    /// Default 250 bps half-spread = 500 bps round-trip = 5% total.
    double   max_half_spread_bps{250.0};

    // -- Fair-value deviation guard -----------------------------------------
    // The ladder is centred on the dexie book mid.  Nothing used to validate
    // that mid against anything, so when it was wrong EVERY tier was wrong in
    // the same direction and a single taker could lift the whole ladder: on
    // 2026-08-01 all six XCH/BYC ask tiers filled 9-11% below the price
    // implied by external data, while each fill logged a POSITIVE realized
    // P&L because the basis had been inflated by equally-mispriced bids.
    //
    // These knobs deliberately have working defaults and are absent from
    // config.yaml: the guard must protect an unconfigured deployment.

    /// Maximum tolerated deviation (bps) of a posted tier price from the
    /// INDEPENDENT fair value, in the direction that loses money -- a bid
    /// above fair value, or an ask below it.  Offending tiers are CLAMPED to
    /// the band edge, never dropped; the bot keeps quoting in all conditions.
    /// Default 300 bps (3%).  Measured against 1 500 XCH/wUSDC.b tier quotes
    /// (the healthy book), the worst binding-direction deviation was
    /// -112 bps on asks and +11 bps on bids, so 300 bps never binds there.
    /// 0 disables the clamp.
    double   max_fair_value_deviation_bps{300.0};

    /// Extra width (percent) applied to every tier's distance from the mid
    /// when NO independent fair value is available for a pair.  Quoting blind
    /// at normal width is what let the sweep happen; widening keeps us in the
    /// market but out of easy reach.  Default 50 (tiers sit 1.5x further from
    /// the mid).  0 disables the widening.
    double   blind_quote_widen_pct{50.0};

    /// Minimum price step (bps) inserted between successive tiers that all
    /// clamp to the same fair-value band edge.  Without it, six clamped asks
    /// collapse onto one price: six creation fees and six locked UTXOs for
    /// what is economically a single level (observed with the no-loss floor
    /// on 2026-07-30).  Default 10 bps.
    double   fair_value_clamp_tier_step_bps{10.0};

    // -- Uncertainty-scaled quoting -----------------------------------------
    // The binary usable/Unavailable cliff treated the solve's sigma as a
    // validity flag.  It is a WIDTH INSTRUCTION: at the 2026-08-01 sweep the
    // solve knew XCH/BYC was worth ~1.3608 +- 467 bps while the book said
    // 1.2673, and because 467 exceeded the 200 bps usability ceiling the
    // estimate was discarded and six asks were posted 9-11% below the truth.
    // These knobs make the ladder CENTRE an uncertainty-weighted blend of the
    // pair's own mid and the external estimate, and make the ladder's minimum
    // half-spread scale with the combined uncertainty.  Both are absent from
    // the shipped config.yaml on purpose: the defaults must protect an
    // unconfigured deployment, and no pair is named anywhere.

    /// Blend the ladder centre toward the external solve estimate, weighted
    /// by inverse variance (book sigma = spread/2 + staleness + depth, the
    /// same terms the solve itself uses; external sigma = the solve's own).
    /// At the sweep this moves the XCH/BYC centre from 1.2673 to ~1.345
    /// (external weight 0.84, because a 2114 bps book deserves almost no
    /// vote), while on healthy XCH/wUSDC.b (book sigma ~131 bps vs CexDirect
    /// sigma ~133 bps) the centre moves only ~17 bps -- the tight fresh book
    /// keeps pricing itself.  Default true.
    bool     quote_center_blend_enabled{true};

    /// Minimum ladder half-spread as a multiple of the combined 1-sigma of
    /// the blended centre (k_sigma).  The floor is
    ///     max(configured tier spacing, k * combined_sigma,
    ///         min_profit_margin_bps, tibetswap_fee_bps)
    /// and every existing floor downstream (no-loss, peg guard, competitive
    /// cap) still applies.  Default 1.0 -- quote one standard deviation out.
    /// At the sweep: combined sigma ~427 bps -> lowest ask at
    /// centre * 1.0427 ~= 1.4026, i.e. 0.8% below the 1.4143 truth instead of
    /// 10.4% below it.  On XCH/wUSDC.b the combined sigma is ~93 bps, inside
    /// the pair's existing 200-300 bps tier spacing, so nothing changes.
    /// 0 disables the sigma term (existing floors still apply).
    double   quote_width_sigma_mult{1.0};

    // -- Avellaneda-Stoikov reservation offset --------------------------------
    // [AS-RES 2026-08-01] The A-S reservation price was computed every
    // heartbeat in Step 4 and discarded: Step 7 centred the ladder on the
    // (uncertainty-blended) market mid and read only the risk quote's sizes.
    // Measured consequence on 2026-07-31: ~116 XCH held against an ~80 XCH
    // target and no selling lean anywhere in the posted ladder.  These knobs
    // shift the ladder CENTRE by the bounded inventory term
    //
    //     centre' = centre * (1 - q * gamma * sigma^2 * tau)
    //
    // where q = signed imbalance normalized to the pair's ratio target
    // (positive = long base), sigma = the honestly-warmed ANNUALIZED
    // volatility (see VolatilityEstimator::rehydrate_from_ticks), and tau =
    // the quote-refresh horizon as a year fraction (~19 min = 3.615e-5 yr).
    // Full dimensional analysis in strategy/reservation_offset.hpp.  All
    // downstream guards (sigma width floor, fair-value clamp, no-loss floor,
    // peg guards, competitive caps) are untouched and act on the shifted
    // centre.  No pair is named anywhere; the keys are absent from the
    // shipped config.yaml on purpose -- the defaults are the operative
    // values and must protect an unconfigured deployment.

    /// Master switch for the reservation offset.  Default true -- connecting
    /// this term to the posted ladder is the point of the mechanism; with an
    /// unwarmed estimator the sigma floor makes the term ~0 anyway (see
    /// as_reservation_gamma numbers below), so enabling it is safe even with
    /// no history.
    bool     as_reservation_enabled{true};

    /// Risk-aversion for the reservation offset, DIMENSIONLESS in the
    /// normalized units above (q as a fraction of target, sigma relative and
    /// annualized, tau in years) -- deliberately NOT strategy.gamma, whose
    /// 0.003 belongs to the raw-A-S formula's units and would make this term
    /// 0.0006 bps at the measured state (decorative).  Default 1000:
    /// the per-unit-imbalance lean gamma * sigma^2 * tau at the measured
    /// sigma = 1.11 annualized and tau = 19 min is 445 bps, far above the
    /// 100 bps rail below, so at high volatility the RAIL governs and the
    /// formula provides the smooth scaling beneath it:
    ///     sigma = 1.11 -> 445 bps per unit q  (rail binds for |q| > 0.22)
    ///     sigma = 0.50 ->  90 bps per unit q
    ///     sigma = 0.20 ->  14 bps per unit q
    ///     sigma = 0.001 (floor, no history) -> 3.6e-4 bps: nothing.
    /// At the measured 2026-07-31 state (q = +0.45, sigma = 1.11) the raw
    /// offset is 200.4 bps, capped to 100: bids AND asks shift down 1%, the
    /// correct selling lean the bot was observed failing to produce.
    double   as_reservation_gamma{1000.0};

    /// Rail (bps) on the reservation shift of the ladder centre, the bound
    /// approved when this mechanism was first planned.  Default 100 (1%).
    /// Applied symmetrically to both signs of imbalance.  0 disables the
    /// offset entirely (a zero cap admits no shift).
    double   as_reservation_max_offset_bps{100.0};

    // -- Triangulation weights ----------------------------------------------
    // The fair value is a weighted least-squares solve over the graph of
    // assets and pairs (see fair_value_solver.hpp).  Every knob below is a
    // 1-SIGMA UNCERTAINTY in basis points -- the honest error bar on one class
    // of observation -- because that is the only thing the solve needs and the
    // only thing an operator can reason about.  Weights are 1/sigma^2, so
    // halving a sigma quadruples that source's influence; nothing is a
    // dimensionless "importance" dial and no pair is named anywhere.

    /// 1-sigma disagreement between an external USD price feed and the
    /// on-chain market, in bps.  NOT the feed's quoting precision: CoinGecko
    /// prints XCH to eight digits, but its number and the dexie XCH/wUSDC.b
    /// mid routinely differ by ~1%, and 1% is what must be carried into the
    /// solve.  Default 100 bps, from that measured agreement.
    double   fair_value_feed_sigma_bps{100.0};

    /// FLOOR (bps) on the 1-sigma of an AMM implied price: the uncertainty a
    /// pool deep enough to be genuinely arbitrage-pinned still carries, which
    /// is the swap fee plus the arbitrage band.  Default 50 bps (TibetSwap's
    /// 0.7% fee implies roughly half that band each side).
    ///
    /// This was formerly the WHOLE sigma, applied flat to every pool.  That
    /// gave a $500 pool weight 1/(50e-4)^2 = 4.0e4 against ~242 for the book
    /// it was outvoting -- about 166:1 -- on the strength of an arbitrage
    /// argument that does not hold at $500.  The actual sigma is now derived
    /// from pool depth (see fair_value_amm_depth_k_bps) and this value is only
    /// its best-case asymptote.
    double   fair_value_amm_sigma_bps{50.0};

    /// Calibration constant for the depth-weighted AMM sigma, in bps:
    ///
    ///     sigma_bps = max(fair_value_amm_sigma_bps,
    ///                     fair_value_amm_depth_k_bps / sqrt(pool_usd))
    ///
    /// Numerically it is the sigma a $1 pool would carry; operationally it is
    /// set by the crossover point where an AMM stops outvoting a mediocre
    /// order book.  Default 15000 bps, calibrated so the live BYC pool lands
    /// just ABOVE the book it competes with rather than 16,500x below it:
    ///
    ///     pool             pool_usd    AMM sigma   competing book sigma
    ///     BYC              ~$501        670 bps    643 bps  (BYC/wUSDC.b)
    ///     wUSDC.b          ~$1,125      447 bps    130 bps  (XCH/wUSDC.b)
    ///     100x BYC pool    ~$50,100      67 bps    -- (approaches the floor)
    ///     ~$90,000+                      50 bps    -- (at the floor)
    ///
    /// In weight terms the BYC pool goes from 166x the book it was outvoting
    /// to 0.92x it -- a peer rather than a dictator -- while the wUSDC.b pool
    /// sits an order of magnitude below the tight book it competes with, which
    /// is correct for $1,125 of depth against a live two-sided market.
    ///
    /// The one out-of-sample check available agrees: at the 2026-08-01 sweep
    /// the BYC pool was wrong by ~550 bps (implied 1.4917, truth ~1.414), so
    /// 670 bps is the right order of magnitude and the old flat 50 bps
    /// understated the real error by roughly 11x.
    double   fair_value_amm_depth_k_bps{15000.0};

    /// Maximum age (seconds) of an AMM sample before it stops contributing.
    double   fair_value_amm_max_age_sec{300.0};

    /// Floor (bps) on the uncertainty attributed to an order-book mid, so that
    /// a momentarily one-tick book cannot claim near-infinite weight.
    double   fair_value_min_book_sigma_bps{10.0};

    /// Uncertainty (bps) added per heartbeat since a book's mid last MOVED.
    /// A frozen quote is not a fresh one: the BYC/wUSDC.b mid sat at exactly
    /// 1.1030 for 26+ consecutive snapshots (longest freeze 30.4 h) while
    /// reporting an age of 0 seconds, and the true price drifted underneath it
    /// the whole time.  Default 5 bps per heartbeat, so a book frozen for an
    /// hour (~60 heartbeats) carries ~300 bps of drift uncertainty and stops
    /// being able to outvote anything.
    double   fair_value_stale_sigma_bps_per_print{5.0};

    /// Depth term: uncertainty (bps) attributed to a book observed with a
    /// single resting third-party offer, decaying as 1/sqrt(n) with the offer
    /// count.  Default 30 bps -- small next to the width and staleness terms,
    /// which is correct: depth is a weak signal here and should not dominate.
    /// 0 disables the term.
    double   fair_value_depth_ref_bps{30.0};

    /// Above this solved 1-sigma (bps), a fair value is reported as
    /// UNAVAILABLE and the widen-don't-clamp path engages.  A 300 bps clamp
    /// band around an estimate that is itself uncertain to more than this is
    /// not a guard, it is theatre.  Default 200 bps.  Note that a pair between
    /// two feed-anchored assets inherently sits near
    /// sqrt(2) * fair_value_feed_sigma_bps ~= 141 bps, so this must stay
    /// comfortably above that or every pair goes blind.
    double   fair_value_max_sigma_bps{200.0};

    /// At or below this solved 1-sigma (bps), a cross-checked value is
    /// promoted to the CexDirect / Triangulated tiers; above it the value is
    /// still usable but reported as Inferred.  Default 150 bps.
    double   fair_value_tight_sigma_bps{150.0};

    /// The deviation band is widened by this multiple of the solve's own
    /// sigma:  band = max_fair_value_deviation_bps + mult * sigma_bps.
    /// A shakier fair value therefore clamps less aggressively instead of
    /// being trusted exactly as far as a firm one.  Default 1.0.  0 makes the
    /// band a flat max_fair_value_deviation_bps.
    double   fair_value_sigma_band_mult{1.0};

    /// Extra tier width, as a fraction of the absolute CONSISTENCY RESIDUAL,
    /// applied when a pair's own book disagrees with the rest of the graph.
    /// The residual is the disagreement signal: when the books cannot all be
    /// right, one of them is about to move, and quoting tight into that is how
    /// a ladder gets swept.  Default 0.5 -- a 1 200 bps disagreement pushes
    /// tiers 600 bps further out.  0 disables residual-driven widening.
    double   fair_value_residual_widen_ratio{0.5};

    /// Residual magnitude (bps) below which no extra widening is applied.
    /// Books never agree to the basis point; only a real disagreement should
    /// move quotes.  Default 150 bps.
    double   fair_value_residual_widen_floor_bps{150.0};

    // -- Order-book micro-price blend schedule -------------------------------
    // The order-book mid is a Stoikov micro-price: each side's top-N VWAP is
    // weighted by the OPPOSITE side's depth, so the estimate leans toward the
    // thin side -- the side that moves next.  That is genuinely the right
    // estimator on a tight two-sided book and it is why it is still here.
    //
    // Its information content collapses as the book widens.  "Which side is
    // thinner" is a statement about the next tick; on a 1259 bps book there is
    // no next tick to speak of, and the answer says essentially nothing about
    // fair value.  Worse, each side's VWAP lies OUTSIDE the BBO by
    // construction (bid_vwap <= best_bid, ask_vwap >= best_ask), so once one
    // side's depth dominates the estimate is dragged out of the book entirely:
    // BYC/wUSDC.b (~65 deep bids vs ~9 thin asks) published a "mid" of
    // 1.144728 sitting EXACTLY on its own best ask at block 9087661, against a
    // true BYC value of ~$1.01 corroborated five independent ways.
    //
    // So the micro-price weight is degraded continuously with relative spread:
    //
    //     w_micro = clamp(1 - (spread_bps - narrow) / (wide - narrow), 0, 1)
    //     mid     = w_micro * microprice + (1 - w_micro) * BBO midpoint
    //
    // Both knobs are ABSENT from the shipped config.yaml on purpose: the
    // defaults below must work unedited, because an operator who never touches
    // config.yaml is exactly the operator this defect reached.

    /// At or below this relative spread (bps), the micro-price is used whole.
    ///
    /// Default 200 bps, chosen against MEASURED snapshot spread distributions
    /// over the seven days to 2026-08-01:
    ///
    ///     pair            p10    p50    p90     n
    ///     XCH/wUSDC.b     130    237    315     509
    ///     XCH/DBX          41     63    123     489
    ///     XCH/BYC          58    794   2549     440
    ///     BYC/wUSDC.b     290   1163   1452     509
    ///
    /// XCH/DBX (p90 = 123) sits entirely inside the narrow band and keeps the
    /// micro-price in full.  XCH/wUSDC.b -- the healthy, profitable pair the
    /// fix must not disturb -- straddles it: at its p50 of 237 bps the weight
    /// is 0.94, and at its p90 of 315 bps still 0.81, so substantially all of
    /// its micro-price behaviour survives.  Setting narrow at its p50 instead
    /// would have cut the healthy pair's weight to 0.5 for no reason; setting
    /// it wider would have started trusting XCH/BYC's 794 bps p50.
    double   microprice_narrow_bps{200.0};

    /// At or above this relative spread (bps), the micro-price is discarded
    /// entirely and the plain BBO midpoint is used.
    ///
    /// Default 800 bps.  Same measured distributions.  This is set just above
    /// XCH/BYC's p50 (794) and far below BYC/wUSDC.b's p10 (290 -> w = 0.85)
    /// and p50 (1163 -> w = 0), so the pathological pair lands essentially at
    /// the plain midpoint in its normal state while XCH/wUSDC.b never comes
    /// close to reaching zero weight (its p99 of 365 bps still carries 0.73).
    /// The band 200..800 is also wide enough that the weight moves smoothly
    /// rather than snapping -- a discontinuity here is what made the old
    /// "deviates >10% from BBO midpoint, clamp to it" guard useless.
    double   microprice_wide_bps{800.0};

    // -- Published-mid BBO band ---------------------------------------------
    //
    // The order-book mid is clamped to its own BBO inside
    // compute_orderbook_mid(), but the PUBLISHED mid is a further blend of
    // that number with CEX (30%) and optionally AMM references, so it could
    // exit the book again.  That re-exit is the exact mechanism that let
    // self-referential garbage propagate: the artifact BYC/wUSDC.b "mid" of
    // 1.1447 (vs a $1.01 truth corroborated five independent ways) fed the
    // USD cross, and a comparably broken CEX reference at 30% weight could
    // drag a healthy pair's published mid ~430 bps out of its own executable
    // interval.  So the published mid is clamped to the dust-filtered
    // third-party BBO widened by a band:
    //
    //     band_bps = max(floor_bps, spread_frac * book_spread_bps)
    //
    // and the clamp applies only while the dex book is two-sided and fresh
    // (per stale_threshold) -- a stale book is history, not "now", and a
    // fresh CEX print should govern it.  When the clamp binds it is logged
    // at warning level: it means an external reference disagrees with the
    // live book beyond tolerance, which is either an arbitrage or a broken
    // feed, and both deserve eyes.
    //
    // Both knobs are ABSENT from the shipped config.yaml on purpose; the
    // defaults must work unedited.

    /// Minimum band (bps) beyond the BBO regardless of the book's spread.
    ///
    /// Default 150 bps.  The healthy pair, XCH/wUSDC.b, has a measured
    /// CexDirect solve sigma of ~133 bps: a genuine one-standard-deviation
    /// CEX-vs-DEX disagreement on a tight fresh book must NOT trip the
    /// clamp, so the floor sits just above one sigma.  Anything much larger
    /// stops being an invariant: the BYC artifact pulled the USD cross 13%
    /// (~1300 bps) off truth, and at the 30% CEX blend weight that reaches
    /// the published mid as a ~430 bps excursion -- the floor must be well
    /// below that to catch it.
    double   published_mid_band_floor_bps{150.0};

    /// Band as a fraction of the book's own relative spread.
    ///
    /// Default 0.25.  A wide book is a weak claim about location, so an
    /// external reference is allowed to pull further outside it.  Measured
    /// at the 2026-08-01 sweep (block 9087661): XCH/BYC's book was
    /// 1.1334/1.4013 (2114 bps) while external truth was 1.4143 -- 93 bps
    /// ABOVE the best ask.  A hard clamp to the raw BBO would forbid the
    /// published mid from ever reaching truth on that book; 0.25 * 2114 =
    /// 528 bps of allowance covers it with margin.  On the healthy pair's
    /// p50 spread of 237 bps the term is 59 bps, safely below the floor, so
    /// tight books get the floor and wide books get proportional room.
    double   published_mid_band_spread_frac{0.25};

    /// Minimum annualized sigma passed to the GLFT/A-S formula.
    /// When the Yang-Zhang estimator returns zero (flat market), the
    /// raw half-spread degenerates to (1/kappa)*ln(1+kappa/gamma) and
    /// the volatility-driven position-sizing term vanishes.  A small
    /// floor keeps the formula well-behaved.  Default 0.001 (~0.1%).
    double   sigma_floor{0.001};

    /// High-volatility regime multiplier for spread widening.
    /// Applied multiplicatively to the base spread when the regime
    /// detector flags high volatility.  Default 1.80 (80% wider).
    double   high_vol_multiplier{1.80};

    /// On-chain fee per offer/cancel (mojos).  Default 0.00001 XCH.
    std::uint64_t offer_fee_mojos{10'000'000ULL};

    /// Number of blocks to observe in startup market-analysis mode before
    /// entering active trading.  0 = skip analysis.  Range [0, 1440].
    /// Example: 20 blocks ≈ 17 minutes at 52 s/block.
    uint32_t startup_analysis_blocks{0};

    /// [T4-02] Reorg protection: number of confirmations required before a
    /// fill is treated as final.  Fills detected at confirmed_at_index are
    /// held in a pending buffer until current_block - fill_block >= this
    /// value.  Default 6 blocks (~5 min at 52 s/block).  0 = instant.
    uint32_t confirmation_depth_blocks{6};

    /// [T4-11] How often (in blocks) to run full offer-state reconciliation
    /// between wallet RPC state and in-memory pending-offers map.
    /// Default 20 blocks (~17 min).  0 = disabled.
    uint32_t reconciliation_interval_blocks{20};

    /// [T7-10] Batch offer creation: merge same-side tiers for a pair into
    /// a single RPC call, reducing per-heartbeat transaction count from ~40
    /// to ~10 (one offer per side per pair).  The merged offer sums the
    /// mojo amounts across tiers.  All constituent tiers share the same
    /// offer ID for lifecycle tracking.
    /// false = current behavior (one offer per tier).
    /// true  = merge same-side tiers.
    bool     batch_offers_enabled{false};

    /// Minimum fraction of confirmed balance that must remain spendable
    /// before the engine will post new offers.  Range [0, 1].  Default 0.25.
    double   min_spendable_reserve_pct{0.25};

    /// Extra blocks beyond offer_ttl_blocks before an offer is considered
    /// "stuck" and eligible for forced cancellation + alerting.
    uint32_t stuck_offer_age_blocks{30};

    // -- Minimum balance management -----------------------------------------

    /// XCH to hold back from offer allocation for paying on-chain fees
    /// (offer cancellation / creation).  Deducted from the available
    /// capital pool in Step 7 before the tier ladder is built, so offers
    /// never lock the last `fee_reserve_xch` of spendable XCH.
    /// Default 1.0 XCH.
    double   fee_reserve_xch{1.0};

    /// Minimum spendable XCH required before posting offers (Step 8 gate).
    /// Unlike fee_reserve_xch (which protects trading inventory), this is
    /// the absolute minimum XCH the wallet must have to pay on-chain fees.
    /// If spendable XCH drops below this, offer posting is skipped.
    /// Set lower than fee_reserve_xch so fees can draw from the reserve
    /// without blocking trading.  Default 0.01 XCH (~3× typical fee).
    double   fee_min_spendable_xch{0.01};

    /// Minimum spendable XCH required before executing take_offer-based
    /// strategies (crossed-book, midpoint recycling, buyer).  This is
    /// intentionally higher than fee_min_spendable_xch to leave enough headroom
    /// for Chia UTXO coin-selection quirks during taker flows.
    double   taker_min_spendable_xch{0.25};

    /// Expected mean CHIA inter-block interval in seconds.  Used by runtime
    /// freshness checks that convert block-age limits into wall-clock time.
    double   block_time_seconds{52.0};

    /// Number of spendable coins to keep unallocated as "dry powder" for
    /// opportunistic trades (arbitrage, crossed-book takes).  Step 7
    /// deducts this from the XCH UTXO headroom calculation so the tier
    /// ladder never locks the last N coins.  Default 2.
    int      arb_reserve_coins{2};

    /// Minimum units of each asset to keep as reserve.  Offers on the
    /// side that would deplete an asset below this level are suppressed.
    /// Uses the pair's mojos_per_unit for conversion.  Default 1.0.
    double   min_reserve_units{1.0};

    /// Minimum offer size in base-asset units.  Tiers with a size below
    /// this value (converted to mojos) are dropped in Step 7.  Prevents
    /// dust-sized offers that waste fees and wallet UTXOs.  Default 0.1.
    double   min_offer_size_units{0.1};

    /// Maximum offer size per tier in base-asset units for XCH-base pairs.
    /// Tiers larger than this are capped down.  0 = no cap.  Default 5.0.
    double   max_offer_size_units{5.0};

    /// When true, Step 7 caps the bid pool (avail_capital) at the quote-
    /// asset wallet's confirmed balance, and the ask pool (avail_inventory)
    /// at the base-asset wallet's confirmed balance, for CAT assets.  XCH
    /// is already capped unconditionally via xch_confirmed_balance_ above.
    /// Prevents oversized offers when the wallet does not actually hold
    /// enough to back the Avellaneda/risk-sized pool.  Default true.
    bool     wallet_balance_caps_enabled{true};

    /// When true, Step 7 ensures the bid pool is large enough to emit at
    /// least one tier above min_offer_size_units when the quote wallet has
    /// sufficient balance (and likewise for the ask pool / base wallet).
    /// Counteracts aggressive risk/allocator shrinking that would otherwise
    /// produce zero tier quotes despite ample wallet capacity to trade.
    /// Respects ratio-rebalance mode: only floors the side being acquired
    /// (bids in AcquireBase, asks in AcquireQuote, both in Neutral).
    /// Default true.
    bool     deploy_idle_inventory_enabled{true};

    /// Minimum units of each asset desired for active trading.  When an
    /// asset is below this level, the engine biases toward acquiring it
    /// by posting only buy-side offers.  Default 10.0.
    double   min_trading_units{10.0};

    /// When true, automatically post one-sided offers to acquire
    /// depleted assets when below min_trading_units.  Default true.
    bool     auto_rebalance_enabled{true};

    /// Enable target-ratio inventory rebalancing around ratio_target.
    /// When enabled, Step 8 can force one-sided posting based on the
    /// pair's value ratio using hysteresis thresholds.
    bool     ratio_rebalance_enabled{true};

    /// Target base-value ratio for each pair in [0, 1].
    /// 0.5 = equal base/quote value split.
    double   ratio_target{0.50};

    /// Optional per-pair target base-value ratio override.
    /// Key is pair name (e.g. "XCH/wUSDC.b"), value in (0, 1).
    std::unordered_map<std::string, double> ratio_target_by_pair;

    /// Enter rebalance mode when ratio deviates beyond this band from
    /// ratio_target. Example with target=0.5 and enter=0.1:
    ///   ratio >= 0.6 -> acquire quote, ratio <= 0.4 -> acquire base.
    double   ratio_band_enter{0.10};

    /// Optional per-pair enter-band override (deadband half-width in ratio
    /// space, in (0, 0.5)).  When set for a pair, replaces the global
    /// ratio_band_enter for that pair.  Allows the GUI to expose a "Target
    /// % +/-" tolerance per asset that gets mapped to a wider/narrower
    /// rebalance deadband on each pair the asset participates in.
    /// The corresponding exit band is derived as min(global exit, this/2).
    std::unordered_map<std::string, double> ratio_band_enter_by_pair;

    /// Optional per-asset portfolio target weight (fraction in [0, 1]).
    /// Keys are upper-cased asset symbols ("XCH", "BYC", "WUSDC.B", ...).
    /// Sum across keys should approximate 1.0 but is not enforced here.
    /// Consumed by the soft drift guard in Step 7 (see
    /// `asset_drift_guard_enabled`) and *not* by ratio rebalancing.
    std::unordered_map<std::string, double> asset_target_allocations;

    /// Optional per-asset tolerance around the target weight (fraction).
    /// Same upper-cased keys as `asset_target_allocations`.  The soft
    /// drift guard treats `[target - tol, target + tol]` as the no-action
    /// zone; outside that band, the relevant side of every pair touching
    /// the asset is tapered linearly toward zero, hitting zero at
    /// `target +/- asset_drift_guard_max_factor * tol`.
    std::unordered_map<std::string, double> asset_target_tolerances;

    /// When true, Step 7 applies an asset-level soft drift guard:
    /// once an asset's actual portfolio fraction exceeds
    /// `target + tol`, the bid pool of every pair where that asset is
    /// base (and the ask pool of every pair where it is quote) is
    /// scaled by `max(0, 1 - (excess - tol) / ((max_factor-1) * tol))`.
    /// Symmetric behaviour when an asset falls below `target - tol`.
    /// Stops e.g. arbitrage / cross-pair skew from accumulating an asset
    /// well past its target even when one pair's ratio controller is
    /// already in AcquireQuote.  Default true.
    bool     asset_drift_guard_enabled{true};

    /// Multiplier on tolerance defining the "hard taper" point.  At
    /// `excess = (max_factor - 1) * tol` the soft cap reaches zero.
    /// Default 2.0, meaning drift up to `target + tol` is unaffected,
    /// drift to `target + 2*tol` is fully suppressed on the acquiring
    /// side.  Must be > 1.0.
    double   asset_drift_guard_max_factor{2.0};

    /// Exit rebalance mode when ratio returns inside this tighter band
    /// around ratio_target (hysteresis to avoid side-flip churn).
    double   ratio_band_exit{0.05};

    /// When true, Step 8 suppresses one side entirely while in ratio
    /// rebalance mode. When false, ratio mode is advisory only.
    bool     ratio_force_one_sided{true};

    /// Minimum side scaling multiplier used by ratio-mode sizing in Step 7.
    /// The overweight side is reduced toward this value.
    double   ratio_tier_size_scale_min{0.35};

    /// Maximum side scaling multiplier used by ratio-mode sizing in Step 7.
    /// The underweight side is boosted up to this value.
    double   ratio_tier_size_scale_max{1.15};

    /// When true (and auto_rebalance_enabled is also true), the engine
    /// reprices the tightest ask tier to just below the current DEX
    /// best ask when the quote asset is depleted and the inventory
    /// ratio exceeds quote_recovery_ratio_threshold.  This makes the
    /// engine the cheapest seller on the DEX so buyers prefer our
    /// offer, accelerating the rebalance.  Default true.
    bool     quote_recovery_enabled{true};

    /// Inventory ratio threshold that activates quote-recovery pricing.
    /// When abs(inventory_ratio) >= this value with the quote asset
    /// depleted, the tightest ask tier is undercut aggressively.
    /// Default: 0.75 (75% base-heavy triggers recovery).
    double   quote_recovery_ratio_threshold{0.75};

    /// Basis points below the current best ask to price the recovery
    /// ask tier.  Smaller = less slippage; larger = fills faster.
    /// Default: 5.0 bps (0.05% undercut).
    double   quote_recovery_undercut_bps{5.0};

    // -- XCH inventory-preservation throttle -------------------------------

    /// When enabled, XCH-base asks become progressively less competitive as
    /// confirmed XCH falls through the configured thresholds.
    bool     xch_ask_throttle_enabled{true};

    /// Confirmed XCH level where ask throttling begins.
    double   xch_ask_throttle_caution_xch{2.0};

    /// Confirmed XCH level where ask throttling becomes materially stronger.
    double   xch_ask_throttle_low_xch{1.0};

    /// Confirmed XCH level where only a defensive outer ask is kept.
    double   xch_ask_throttle_critical_xch{0.35};

    /// Global aggressiveness multiplier for ask widening / size reduction.
    /// 1.0 = baseline, <1 = gentler, >1 = more protective.
    double   xch_ask_throttle_aggressiveness{1.0};

    // -- Coin pool management -----------------------------------------------

    /// Target number of spendable XCH coins to maintain.  The engine
    /// periodically self-sends to split large coins into this many
    /// chunks, ensuring enough UTXOs for concurrent multi-tier offers.
    /// 0 = disabled (no automatic coin splitting).
    /// Default: 20 (sufficient for 3 pairs x 6 tiers x 2 sides = 36,
    /// with some headroom from change outputs).
    int      coin_pool_target_count{20};

    /// Target denomination for each split coin, in XCH.
    /// The engine creates coins of this size when splitting.
    /// Default: 5.0 XCH.
    double   coin_pool_target_xch{5.0};

    /// How often (in blocks) to run coin pool maintenance.
    /// Default: 50 blocks (~43 minutes).  0 = only at startup.
    uint32_t coin_pool_interval_blocks{50};

    // -- CAT coin pool management -------------------------------------------

    /// Target number of spendable coins for each CAT wallet (BYC, wUSDC.b,
    /// etc.).  Works identically to coin_pool_target_count but applies to
    /// every CAT asset referenced by an enabled pair.
    /// 0 = disabled (no CAT coin splitting).
    /// Default: 10.
    int      cat_coin_pool_target_count{10};

    /// Target denomination for each split CAT coin, in display units
    /// (NOT mojos).  Converted to mojos using the pair's mojos_per_unit.
    /// For wUSDC.b (1000 mojos/unit), a value of 50.0 → 50,000 mojos.
    /// For BYC (1000 mojos/unit), a value of 50.0 → 50,000 mojos.
    /// Default: 50.0 units.
    double   cat_coin_pool_target_units{50.0};

    // -- Gap-aware dynamic tier spacing -------------------------------------

    /// Enable gap-aware dynamic tier spacing.
    bool     gap_aware_spacing{true};

    /// Minimum gap width (bps) in competing order book to target.
    double   min_gap_bps{50.0};

    /// Maximum distance from mid (bps) to scan for gaps.
    double   max_gap_scan_bps{1500.0};

    /// Blend factor for gap-directed spacing [0, 1].
    double   gap_blend_factor{0.6};

    // -- Competitive anchor pricing -----------------------------------------

    /// Anchor Tier 0 to the best competing offer instead of mid ± spacing.
    /// When the order book has competing offers, this places our tightest
    /// tier 1 tick better than the best competitor, making us top-of-book.
    /// Subsequent tiers step outward by competitive_anchor_stride_bps.
    /// Falls back to mid-based spacing when no competing offers exist.
    bool     competitive_anchor_enabled{false};

    /// Max distance (bps) from mid for a valid anchor.  Default 500.
    double   competitive_anchor_max_distance_bps{500.0};

    /// Inter-tier stride (bps) from the anchor point.  Default 65.
    double   competitive_anchor_stride_bps{65.0};

    // -- Untrustworthy-reference quoting gate --------------------------------

    /// Heartbeats the dexie mid may sit unchanged before we refuse to post
    /// new offers on that pair (see MarketDataFeed::dex_print_age).
    /// Measured: the BYC/wUSDC.b dexie mid held exactly 1.1030 for 26+
    /// consecutive snapshots (longest freeze 30.4h, 92.6% of observations
    /// unchanged) while the TibetSwap cross said 1.016196 -- the book was
    /// 854 bps wrong, and we kept quoting around it.  Default 6, roughly
    /// two hours at the ~19-minute heartbeat.  0 disables the staleness leg
    /// of the gate.  Deliberately NOT in config.yaml: the default must work
    /// unconfigured.
    uint32_t dex_print_stale_heartbeats{6};

    // -- Adverse-selection-aware tier sizing ---------------------------------

    /// Enable adverse-selection-aware tier sizing.
    bool     adverse_selection_sizing{true};

    /// Decay factor for adverse-selection sizing (lower = more outer-heavy).
    double   adverse_selection_decay{0.7};

    /// Volatility threshold above which decay is halved -- compared against
    /// the ANNUALIZED sigma the ladder step passes in.
    ///
    /// [AS-WARM recalibration 2026-08-01] Raised from 0.05 (config.yaml had
    /// 0.005) to 2.0.  The old values were tuned in a world where the
    /// volatility estimator was never ready and sigma was pinned at the
    /// 0.001 floor, so the branch NEVER fired in production and there is no
    /// working semantic to preserve.  With the warm-started estimator sigma
    /// is honest -- measured 0.4-1.9 annualized across pairs -- and a
    /// threshold of 0.005 would fire permanently on every pair, silently
    /// halving decay and pushing ~82% of ladder capital to the outer
    /// 230-300 bps tiers (weights [0.8, 1.9, 4.4, 10.4, 24.6, 57.8]%
    /// instead of the configured ~[10, 12, 15, 18, 22, 23]%) while fills
    /// happen at 30-130 bps.  2.0 (200% annualized) fires only in genuinely
    /// extreme regimes; it also sits above XCH/BYC's measured 1.91, which
    /// is partly inflated by pre-fix self-priced mids still present in the
    /// warm-start history.  0 = always use base decay.
    double   adverse_selection_sigma_threshold{2.0};

    // -- Fill-rate-weighted adaptive tier sizing ----------------------------

    /// Enable fill-rate-adaptive tier sizing.
    bool     fill_rate_sizing{true};

    /// Blend factor for fill-rate sizing [0, 1].
    double   fill_rate_blend{0.30};

    /// Lookback window in hours for fill-rate computation.
    int      fill_rate_lookback_hours{24};

    /// Minimum allocation fraction per tier when fill-rate sizing is active.
    double   fill_rate_min_pct{0.05};

    // -- AMM blend weight for market data feed ------------------------------

    /// Weight of the TibetSwap AMM implied price in the composite mid-price.
    ///
    /// DEFAULT 0.0 -- THE AMM IS A VALIDATOR, NOT A PRICE INPUT.
    ///
    /// The AMM sample feeds two consumers: this blend (composite mid ->
    /// market_mid -> centre of every ladder) and the fair-value solve that
    /// checks those ladder prices.  Letting it do both makes the guard's
    /// reference the same number that moved the thing being checked -- the
    /// solve would "confirm" a price it had itself set, and the deviation
    /// band would measure nothing.  This weight was inert until the TibetSwap
    /// client gave it a producer; wiring that client is what made the cycle
    /// real, so the cycle is broken here at the input side.
    ///
    /// Validation is the more valuable of the two roles: as a price input the
    /// AMM would move quotes by at most its blend share, while as an
    /// independent edge it is what turns a leg priced only by one wide frozen
    /// book from Unavailable into a usable clamp.  So the blend gives way.
    ///
    /// Setting this above 0 re-creates the cycle and is not supported.
    double   amm_blend_weight{0.0};

    // -- Wall-aware retail niche pricing ------------------------------------

    /// Competing offers larger than this threshold (XCH) are classified as
    /// "walls".  The engine will not undercut walls in the competitive cap
    /// (Step 7) and will widen spreads to capture a retail niche premium
    /// (Step 5).  On Chia DEX, offers are atomic — small traders cannot
    /// take wall-sized offers and must use our smaller, accessible ones.
    /// Default 20.0 XCH.
    double   wall_size_threshold_xch{20.0};

    /// Spread widening factor when walls are detected.  Applied as a
    /// multiplier on total_spread_bps in Step 5.  Default 0.15 = 15%
    /// wider spreads targeting the captive retail market segment.
    double   wall_niche_premium_pct{0.15};

    // -- Cost-aware orphan evaluation (startup reconciliation) ---------------
    //
    // Scholarly basis:
    //   Guéant, Lehalle & Fernandez-Tapia (2013) — inventory-risk-aware
    //     cancellation: cancel cost vs. expected adverse selection loss.
    //   Gao & Wang (2020) — zero-offer gap during cancel→repost is the
    //     primary adverse selection cost for latent market makers.
    //   Aït-Sahalia & Saglam (2017) — stale-quote risk scales with price
    //     deviation, remaining lifetime, and offer size.
    //
    // When the engine restarts and discovers wallet offers it doesn't
    // track ("orphans"), the default behavior was to cancel them all.
    // This wastes fees and creates a zero-offer gap.  When enabled, the
    // engine evaluates each orphan's current market attractiveness and
    // adopts well-priced orphans instead of cancelling them.

    /// Master switch for cost-aware orphan evaluation.  When false,
    /// startup reconciliation cancels all orphans (legacy behavior).
    bool     orphan_adopt_enabled{true};

    /// Maximum adverse price deviation (fraction) to adopt an orphan.
    /// An orphan whose price has drifted adversely beyond this threshold
    /// is cancelled.  Default 0.02 (2%).  Adverse means: bid too high
    /// relative to current mid (overpaying) or ask too low (underselling).
    double   orphan_adverse_threshold{0.02};

    /// Maximum age in blocks for an adoptable orphan.  Offers older than
    /// this are cancelled regardless of price accuracy.  Default 120
    /// blocks (~104 minutes).  Prevents adopting offers with very old
    /// coin references that may fail on-chain.
    uint32_t orphan_max_adopt_age_blocks{120};

    /// Extra adverse-deviation tolerance (fraction) granted to orphans
    /// that would reduce the current inventory imbalance.  For example,
    /// if we are long and the orphan is an ask (sell), it helps rebalance
    /// inventory and gets this bonus before the threshold check.
    /// Default 0.01 (1% additional tolerance → effective 3% for helpers).
    double   orphan_inventory_bonus{0.01};

    // -- Cross-pair correlated inventory skewing (Guéant 2019) --------------
    //
    // When multiple pairs share a common asset (e.g. XCH is base in
    // XCH/wUSDC.b and XCH/BYC; BYC is in both XCH/BYC and BYC/wUSDC.b),
    // each pair's inventory skew should account for inventory pressure
    // from the other pairs.  This creates a cross-pair demand signal:
    //
    //   If XCH/BYC is short BYC (needs BYC), AND BYC/wUSDC.b also trades
    //   BYC, then BYC/wUSDC.b should skew its bids UP to acquire more BYC.
    //
    // The adjustment is additive to the standard inventory_ratio:
    //   effective_ratio = clamp(inv_ratio + cross_adj, 0, 1)
    //   cross_adj = cross_pair_skew_phi * Σ(deviation_P' * weight_P')
    //
    // where the sum runs over all OTHER pairs sharing a base or quote asset
    // with the current pair, deviation is (inv_ratio_P' - 0.5) normalised,
    // and weight is the market allocator fraction (or 1/N if disabled).

    /// Master switch for cross-pair correlated skewing.
    bool     cross_pair_skew_enabled{false};

    /// Strength of the cross-pair signal [0, 1].  Higher values cause
    /// stronger coordination between pairs sharing assets.  Conservative
    /// default 0.3 limits the adjustment to ±0.15 of the ratio.
    double   cross_pair_skew_phi{0.30};

    // -- PID adaptive spread controller -------------------------------------
    //
    // Feedback loop that tightens spreads when offers aren't filling and
    // widens when fills become frequent.  Operates per-pair using an EMA
    // of a per-block binary fill signal (1 = any fill this block, 0 = none).
    //
    // PID output drives a spread multiplier:
    //   error = target_fill_rate - ema_fill_rate
    //   output = Kp*e + Ki*∫e + Kd*de/dt
    //   mult = clamp(1.0 - output, pid_min_mult, pid_max_mult)
    //
    // Positive error (underfilling) -> mult < 1.0 -> tighter spreads.
    // Negative error (overfilling)  -> mult > 1.0 -> wider spreads.

    /// Master switch for PID adaptive spread controller.
    bool     pid_spread_enabled{true};

    /// Target fill rate: fraction of blocks where at least one fill occurs.
    /// 0.10 = target one fill per ~10 blocks (~8.7 minutes).
    double   pid_target_fill_rate{0.10};

    /// Proportional gain.  Controls immediate response to fill-rate error.
    double   pid_kp{0.8};

    /// Integral gain.  Addresses persistent underfilling/overfilling.
    double   pid_ki{0.05};

    /// Derivative gain.  Dampens oscillation from rapid error changes.
    double   pid_kd{0.2};

    /// EMA smoothing factor for fill-rate signal.  Lower = smoother.
    /// 0.02 gives an effective window of ~50 blocks (~43 minutes).
    double   pid_ema_alpha{0.02};

    /// Minimum spread multiplier (maximum tightening).  0.70 = tighten 30%.
    double   pid_min_mult{0.70};

    /// Maximum spread multiplier (maximum widening).  1.30 = widen 30%.
    double   pid_max_mult{1.30};

    /// Anti-windup clamp for the integral error accumulator.
    double   pid_integral_max{2.0};

    /// Number of blocks before the PID controller activates (warm-up period).
    /// During warm-up the controller observes but does not adjust spreads.
    uint32_t pid_warmup_blocks{50};

    // -- Adaptive competitiveness-threshold PID controller -----------------
    // Companion to the spread PID above.  Where the spread PID adjusts the
    // half-spread of newly posted offers, this controller adjusts the
    // *competitiveness gate* (Step 8 of the engine pipeline) -- the integer
    // 0-10 score below which a tier is suppressed before posting.
    //
    // Rationale (v0.7.47 audit): even at 8/10 competitiveness, fills can
    // dry up in trending markets when the static gate keeps weak tiers
    // suppressed.  The PID monitors the binary fill signal per block and
    // lowers the effective gate when fills fall below target, raising it
    // when we are over-trading.  Output is an integer offset added to the
    // pair's base threshold (1 for stablecoin pairs, 3 otherwise as of
    // v0.7.46), clamped to [comp_pid_min_offset, comp_pid_max_offset] and
    // then to the legal gate range [0, 10].
    //
    // See cpp/include/xop/strategy/competitiveness_pid.hpp for the
    // controller implementation and its unit tests.

    /// Master switch for the competitiveness-threshold PID controller.
    bool     comp_pid_enabled{true};

    /// Target fill rate in blocks-with-fills / total-blocks units.
    /// 0.05 ~= one fill per ~17 minutes at 52 s blocks.
    double   comp_pid_target_fill_rate{0.05};

    /// PID gains (output is in *integer offset units*).
    double   comp_pid_kp{8.0};
    double   comp_pid_ki{0.5};
    double   comp_pid_kd{2.0};

    /// EMA smoothing alpha for the per-block fill signal.
    double   comp_pid_ema_alpha{0.02};

    /// Anti-windup clamp on the integral accumulator.
    double   comp_pid_integral_max{4.0};

    /// Warm-up window before the controller emits non-zero offsets.
    uint32_t comp_pid_warmup_blocks{50};

    /// Output offset bounds.  Negative = lower gate (more aggressive).
    int      comp_pid_min_offset{-3};
    int      comp_pid_max_offset{+3};
};

// ---------------------------------------------------------------------------
// Risk / inventory management thresholds.
//
// Percentages are expressed as fractions in [0, 1].
//   soft_limit_pct          -- begin aggressive quote skewing.
//   hard_limit_pct          -- pull quotes on overweight side.
//   single_cat_cap_pct      -- max portfolio fraction in any one CAT.
//   kelly_fraction          -- fraction of full Kelly to use (Half-Kelly = 0.5).
//   max_capital_per_pair_pct-- upper bound on capital allocated to one pair.
//
// Circuit breakers (ISO/IEC 27001:2022 §8.20 -- continuous risk monitoring):
//   max_drawdown_pct     -- peak-to-trough drawdown fraction that pauses the
//                           engine.  Default 10% (0.10).  Measures the drop
//                           from the all-time PnL high-water mark.
//   loss_window_blocks   -- rolling window size in blocks for the time-window
//                           loss circuit breaker.  Default 1152 blocks ≈ 10 h
//                           at the Chia mean block time of 52 s.
//   max_window_loss_bps  -- maximum loss (in basis points, i.e. 0.01 % per bp)
//                           permitted within the rolling window before the
//                           engine is paused.  Default 500 bps = 5 %.
//                           A value of 0 disables the window circuit breaker.
// ---------------------------------------------------------------------------
struct RiskConfig {
    double   soft_limit_pct{0.60};
    double   hard_limit_pct{0.80};
    double   single_cat_cap_pct{0.12};
    double   kelly_fraction{0.50};
    double   max_capital_per_pair_pct{0.20};

    // -- Circuit breakers ---------------------------------------------------
    double   max_drawdown_pct{0.10};        ///< HWM drawdown threshold (0,1].
    uint32_t drawdown_grace_blocks{100};    ///< Blocks to skip drawdown check at startup.
    uint32_t loss_window_blocks{1152};      ///< Rolling window size in blocks.
    double   max_window_loss_bps{500.0};    ///< Max loss in window (bps; 0=disabled).

    // -- Flash crash detection (T7-07, T7-08) --------------------------------
    double   flash_crash_threshold_pct{0.20};      ///< Drop % to trigger crash (0,1].
    uint32_t recovery_stable_blocks_phase1{50};     ///< Blocks stable for Crash→Recovery.
    uint32_t recovery_stable_blocks_phase2{100};    ///< Blocks stable for Recovery→Normal.
    double   recovery_stability_band_pct{0.05};     ///< Max price deviation in recovery.

    // -- Circuit-breaker rebalance (T7-09) -----------------------------------
    // Automatically enables StrategicLossManager for a pair when all of:
    //   1. inventory_ratio > circuit_breaker_hard_limit_ratio (one-sided)
    //   2. DriftAnalyzer recommends ManualRebalance or PullOverweight
    //   3. position_age > aging_start_blocks * circuit_breaker_age_multiplier
    // The loss is capped at circuit_breaker_max_loss_bps.
    bool     circuit_breaker_enabled{false};         ///< Master switch (opt-in).
    double   circuit_breaker_hard_limit_ratio{0.80}; ///< Inventory ratio trigger (0,1].
    double   circuit_breaker_age_multiplier{2.0};    ///< age >= aging_start * this.
    double   circuit_breaker_max_loss_bps{100.0};    ///< Max loss cap per rebalance [0,500].
};

// ---------------------------------------------------------------------------
// Yang-Zhang hybrid volatility estimator settings.
//
// lookback_blocks -- rolling window length in blocks (~52 s each).
// yz_alpha        -- blending weight for the YZ estimator (0, 1).
// ---------------------------------------------------------------------------
struct VolatilityConfig {
    uint32_t lookback_blocks{200};
    double   yz_alpha{0.34};

    /// [T5-CR6] Number of blocks to aggregate into a single OHLC candle
    /// before feeding the Yang-Zhang estimator.  With >90% of blocks
    /// producing degenerate (O=H=L=C) candles, aggregating N blocks into
    /// one proper candle dramatically improves the Rogers-Satchell component.
    /// Default 10 blocks (~8.7 min).  1 = no aggregation (legacy).
    uint32_t candle_aggregation_blocks{10};
};

// ---------------------------------------------------------------------------
// Observability: Prometheus metrics exporter and Telegram alert bot.
//
// telegram_bot_token and telegram_chat_id are SECRET and must not be logged.
// ---------------------------------------------------------------------------
struct MonitoringConfig {
    uint16_t    prometheus_port{9090};
    std::string telegram_bot_token;   // SECRET -- never log.
    std::string telegram_chat_id;     // SECRET -- never log.
};

// ---------------------------------------------------------------------------
// Persistent storage path (SQLite in Phase 1, PostgreSQL URI later).
// ---------------------------------------------------------------------------
struct DatabaseConfig {
    std::string path{"data/xop_trader.db"};
};

// ---------------------------------------------------------------------------
// Depeg detector configuration (applies to all stablecoin pairs globally).
// Individual thresholds are set per-pair in PairConfig.
// ---------------------------------------------------------------------------
struct DepegConfig {
    bool   enabled{true};                 // Master switch for depeg detection.
    double default_warn_pct{2.0};         // Default warn threshold (%).
    double default_bail_pct{10.0};        // Default bail threshold (%).
    uint32_t default_sustained_blocks{30};// Default sustained-blocks window.
    bool   auto_disable_pair{true};       // Automatically disable pair on bail.
    bool   alert_on_warn{true};           // Send Telegram alert on warning.
    bool   alert_on_bail{true};           // Send Telegram alert on bail.
};

// ---------------------------------------------------------------------------
// ArbitrageSettings -- YAML-configurable parameters for arbitrage detection.
//
// Maps 1:1 to the `arbitrage:` section of config.yaml.  These values are
// copied into the ArbitrageConfig struct (strategy/arbitrage.hpp) at engine
// construction time.  Keeping the YAML parsing separate from the strategy
// struct avoids a circular dependency between config.hpp and arbitrage.hpp.
// ---------------------------------------------------------------------------
struct ArbitrageSettings {
    bool     enabled{true};                 // Master switch for arb scanning.

    // -- Triangular arbitrage ------------------------------------------------
    double   triangular_min_profit_bps{30.0};
    double   triangular_slippage_bps{10.0};
    double   triangular_per_leg_fee_bps{5.0};
    uint32_t triangular_max_legs{3};

    // -- CEX-DEX arbitrage ---------------------------------------------------
    double   cex_dex_min_edge_bps{50.0};
    double   cex_dex_max_edge_bps{500.0};
    double   cex_fee_bps{10.0};
    double   bridge_fee_bps{0.0};
    double   cex_dex_confidence_cap{0.25};   // Hard cap on CEX-DEX confidence.
                                             //   CoinGecko data is aggregated/delayed
                                             //   and vulnerable to manipulation.

    // -- Cross-DEX arbitrage -------------------------------------------------
    double   cross_dex_min_edge_bps{15.0};
    double   tibetswap_fee_bps{70.0};
    double   dexie_fee_bps{0.0};

    // -- Cross-Bridge arbitrage ----------------------------------------------
    double   cross_bridge_min_edge_bps{20.0};
    double   bridge_cost_bps{15.0};

    // -- Crossed-book arbitrage (intra-DEX, Dexie has no matching engine) ----
    bool     crossed_book_enabled{true};
    double   crossed_book_min_edge_bps{10.0};
    double   crossed_book_max_take_xch{5.0};

    /// When true, Step 9c will cancel the least competitive pending offer
    /// to free a locked coin before executing a crossed-book take, if no
    /// spendable coins are available.  Default true.
    bool     cancel_worst_to_free{true};

    // -- Midpoint recycling (Step 9d) --------------------------------------
    bool     midpoint_recycling_enabled{false};
    std::vector<std::string> midpoint_recycling_pairs;
    // Actionable slack above the derived minimum discount floor.
    double   midpoint_recycling_band_bps{20.0};
    double   midpoint_recycling_max_take_xch{0.25};
    double   midpoint_recycling_min_take_xch{0.10};
    uint32_t midpoint_recycling_cooldown_blocks{4};
    uint32_t midpoint_recycling_max_takes_per_block{1};
    double   midpoint_recycling_daily_take_xch_cap{2.0};
    uint32_t midpoint_recycling_epoch_blocks{4608};
    double   midpoint_recycling_min_expected_edge_bps{5.0};
    double   midpoint_recycling_fee_buffer_bps{2.0};
    double   midpoint_recycling_toxicity_buffer_bps{6.0};
    double   midpoint_recycling_slippage_buffer_bps{2.0};
    double   midpoint_recycling_inventory_ratio_cap{0.60};
    bool     midpoint_recycling_require_cex_ref{true};
    uint32_t midpoint_recycling_max_cex_age_blocks{10};
    double   midpoint_recycling_vpin_max{0.70};

    /// Synthetic half-spread used when building a conservative CEX bid/ask
    /// around a single CoinGecko mid-price reference.
    double   cex_reference_half_spread_bps{10.0};

    // -- Cross-stablecoin arbitrage (XCH/BYC vs XCH/wUSDC.b) ----------------
    //
    // Execution is gated by a STATE MACHINE over observed edge history, not
    // by a single point-in-time reading.  Spreads here are wildly volatile
    // (XCH/BYC has ranged 0-1247 bps, XCH/wUSDC.b 7-615 bps), so one sample
    // cannot tell a genuine opportunity from a stale quote -- and a stale
    // quote is the common case: on 2026-07-30 the scanner's chosen offer came
    // back status=3 (already gone) when it tried to take it.
    //
    // An edge that PERSISTS across observations is far more likely to be
    // executable.  The monitor therefore runs continuously and records every
    // observation to arb_edge_log, arming only after sustained evidence and
    // disarming on a lower threshold so it cannot flap at the boundary.
    bool     cross_stable_arb_enabled{true};   ///< Run the monitor at all.
    double   cross_stable_min_edge_bps{15.0};  ///< Legacy floor; still applied.
    double   cross_stable_max_take_xch{5.0};   ///< Candidate size filter (see below).

    /// Net edge (bps) that must be sustained to ARM the leg pair.
    double   cross_stable_arm_edge_bps{50.0};
    /// Net edge (bps) below which it DISARMS.  Must be < arm to give
    /// hysteresis; between the two the current state is held.
    double   cross_stable_disarm_edge_bps{20.0};
    /// Consecutive observations above the arm threshold before arming.
    uint32_t cross_stable_arm_observations{3};

    /// Master switch for ACTUALLY TRADING when armed.  Default false: the
    /// monitor measures viability first, and capital is only ever at risk
    /// after that data says the edge is real and repeatable.
    bool     cross_stable_execute_when_armed{false};

    // -- Peg-crossing offer taker (stablecoin pair direct arb) ---------------
    // Takes competing offers that cross the $1 peg on stablecoin pairs
    // when the depeg detector reports Normal (peg is trusted).
    bool     peg_arb_enabled{true};
    double   peg_arb_min_edge_bps{5.0};     // min deviation from peg to take
    double   peg_arb_max_take_units{50.0};   // max base-asset units per take
    double   peg_arb_max_inventory_ratio{0.70}; // max base ratio before suppressing buys

    // -- Drift corrector (Step 9f: active asset rebalancing taker) -----------
    // Scans DEX for competitive offers that, if taken, move the portfolio
    // back toward the asset_target_allocations.  Triggered when an asset's
    // share is outside target +/- trigger_factor*tolerance; stops at
    // target +/- exit_factor*tolerance (hysteresis).
    bool     drift_corrector_enabled{false};
    double   drift_corrector_trigger_factor{2.0};
    double   drift_corrector_exit_factor{1.0};
    double   drift_corrector_max_take_units{10.0};   // max base units per take
    double   drift_corrector_max_premium_bps{50.0};  // max deviation from mid
    uint32_t drift_corrector_cooldown_blocks{3};
    uint32_t drift_corrector_max_trades_per_day{10};

    // -- General parameters --------------------------------------------------
    double   max_position_size{100.0};
    double   default_confidence{0.75};
    double   min_confidence_threshold{0.40};
    uint32_t default_urgency_blocks{5};
};

// ---------------------------------------------------------------------------
// CoinGecko external price reference -- free-tier API configuration.
//
// Provides CEX-grade mid-prices for assets that have CoinGecko listings.
// The free tier allows ~10-30 calls/min with no API key.  An optional
// api_key field supports the "Demo" plan (30 calls/min guaranteed).
//
// Asset mapping:
//   XCH          -> coingecko id "chia"
//   wmilliETH.b  -> coingecko id "ethereum" (price / 1000)
//   wmilliETH    -> coingecko id "ethereum" (price / 1000)
//   wUSDC.b      -> coingecko id "usd-coin" (~1.0)
//   BYC          -> no CoinGecko listing (DEX-only)
// ---------------------------------------------------------------------------
struct CoinGeckoConfig {
    bool        enabled{false};              // Master switch.

    /// Base URL for the CoinGecko API (no trailing slash).
    std::string base_url{"https://api.coingecko.com/api/v3"};

    /// CoinGecko coin IDs to fetch (e.g. "chia", "ethereum", "usd-coin").
    std::vector<std::string> coin_ids;

    /// How often to poll CoinGecko (milliseconds).  Free tier: 30-60 s.
    uint32_t    polling_interval_ms{30'000};

    /// HTTP request timeout.
    uint32_t    request_timeout_ms{15'000};

    /// TCP + TLS connect timeout.
    uint32_t    connect_timeout_ms{10'000};

    /// Maximum retries on 429 / 5xx.
    uint32_t    max_retries{3};

    /// Base delay between retries (exponential backoff).
    uint32_t    retry_base_delay_ms{1'000};

    /// Rate limiter: max requests per window.
    uint32_t    rate_limit_max_requests{10};

    /// Rate limiter: sliding window width (milliseconds).
    uint32_t    rate_limit_window_ms{60'000};

    /// Number of threads in the CURL worker pool.
    uint32_t    curl_thread_pool_size{2};

    /// Optional API key (CoinGecko Demo plan).  Empty = free tier.
    std::string api_key;

    /// User-Agent header.
    std::string user_agent{"XOPTrader-CoinGecko/1.0"};
};

// ---------------------------------------------------------------------------
// TibetSwap AMM client configuration (`tibetswap:` section).
//
// TibetSwap is the on-chain constant-product AMM on Chia.  Its pool reserves
// give an independent, arbitrage-anchored marginal price for every pair with
// an XCH leg -- the reference that feeds ArbitrageDetector::scan_cross_dex()
// and MarketDataFeed::ingest_amm_mid().
//
// Every field has a working default: the section may be omitted entirely from
// config.yaml and the client still runs against the public API.
// ---------------------------------------------------------------------------
struct TibetSwapConfig {
    /// Master switch.  Enabled by default -- the API is public, unauthenticated
    /// and cheap, and without it the AMM leg is dead code.
    bool        enabled{true};

    /// Base URL for the TibetSwap v2 API (no trailing slash).
    std::string base_url{"https://api.v2.tibetswap.io"};

    /// How often to poll pool reserves (milliseconds).  Default 60 s, roughly
    /// one Chia block (~52 s); reserves only move when a swap lands on chain.
    uint32_t    polling_interval_ms{60'000};

    /// HTTP request timeout.
    uint32_t    request_timeout_ms{15'000};

    /// TCP + TLS connect timeout.
    uint32_t    connect_timeout_ms{10'000};

    /// Maximum retries on 429 / 5xx.
    uint32_t    max_retries{3};

    /// Base delay between retries (exponential backoff).
    uint32_t    retry_base_delay_ms{1'000};

    /// Rate limiter: max requests per window.
    uint32_t    rate_limit_max_requests{30};

    /// Rate limiter: sliding window width (milliseconds).
    uint32_t    rate_limit_window_ms{60'000};

    /// Number of threads in the CURL worker pool.
    uint32_t    curl_thread_pool_size{2};

    /// Page size for GET /pairs (the pool directory; ~370 pools today).
    uint32_t    page_limit{500};

    /// Hard cap on directory size, guarding against a runaway paging loop.
    uint32_t    max_pools{5'000};

    /// How long the asset_id -> pair_id directory stays valid before it is
    /// re-fetched (milliseconds).  The mapping is effectively static, so an
    /// hour is generous; a request for an unknown asset forces an early
    /// refresh regardless.
    uint32_t    directory_refresh_ms{3'600'000};

    /// User-Agent header.
    std::string user_agent{"XOPTrader-TibetSwap/1.0"};
};

// ---------------------------------------------------------------------------
// Fee budget tracking and dynamic fee selection.
//
// Controls two behaviours:
//   1. Fee-vs-gain gating: skip posting an offer when the blockchain fee
//      exceeds a configurable fraction of the expected gain from the trade.
//   2. Adaptive fee selection: track observed on-chain fees and try to pay
//      the minimum fee that achieves timely inclusion.
//
// When disabled (enabled == false), the static offer_fee_mojos from
// StrategyConfig is used in all fee sites (backward-compatible).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Inventory aging configuration (T4-09).
//
// Controls gradual relaxation of the no-loss constraint for positions that
// have been held for an extended period.  The rationale is that capital
// locked in a permanently-underwater position has an opportunity cost;
// accepting a small controlled loss to free it up can be net-positive.
//
// The effective margin discount grows linearly from 0 at aging_start_blocks
// to max_loss_relax_bps at the maximum aging horizon:
//   discount_bps = min(max_loss_relax_bps,
//                      (age - aging_start_blocks) * relax_rate_bps_per_block)
//   effective_margin = min_profit_margin_bps - discount_bps
//
// The effective margin is never allowed to go below -max_loss_relax_bps
// (i.e. the bot will never accept a loss larger than the configured cap).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// AccountingConfig -- double-entry ledger and its reconciliation control.
//
// The control ties the ledger's implied per-asset balance to the wallet's
// CONFIRMED balance.  Thresholds are flow-based rather than a flat
// percentage, because the CAT wallets are tiny (a single heartbeat of fills
// can move 20-75% of the wUSDC.b balance) while 1% of the XCH wallet would
// swallow an entire missed 1-XCH fill.
//
// Tolerance for asset a:
//     live_offer_exposure(a)              -- only live offers can settle
//   + fee_slack (XCH only)                -- observed dust is 5,000-mojo steps
//   + max(floor_a, pct * confirmed_a)
// ---------------------------------------------------------------------------

struct AccountingConfig {
    /// Master switch for ledger posting and the invariant check.
    bool     ledger_enabled{true};

    /// Divergence beyond the ALERT tolerance, sustained for
    /// `alert_observations` consecutive same-sign checks, raises an alert.
    double   alert_pct{0.005};              ///< 0.5% of confirmed balance.
    uint32_t alert_observations{2};

    /// Divergence beyond the PAUSE tolerance for `pause_observations`
    /// consecutive same-sign checks pauses quoting.
    double   pause_pct{0.02};               ///< 2% of confirmed balance.
    uint32_t pause_observations{3};

    /// OFF by default.  Several real balance movements have no ledger event
    /// yet (taker fills from the arbitrage/drift steps, external deposits and
    /// withdrawals), so auto-pausing would halt trading on legitimate
    /// activity.  Turn this on only once the ledger runs clean.
    bool     pause_enabled{false};

    /// Absolute noise floors, in mojos.  XCH: 0.001 XCH covers fee dust.
    /// CAT: 100 mojos = 0.1 unit, 100x the 1-mojo per-leg rounding error.
    std::int64_t floor_xch_mojos{1'000'000'000LL};
    std::int64_t floor_cat_mojos{100LL};

    /// Extra XCH slack for accumulated per-offer fee dust between checks.
    /// Observed worst case was 55,000 mojos per heartbeat; 200,000 covers
    /// ~40 offer events.
    std::int64_t fee_slack_mojos{200'000LL};

    /// Skip the check when the balance snapshot is older than this many
    /// blocks (the wallet reader is skipped in several engine modes).
    uint32_t max_balance_age_blocks{10};

    /// Observations retained per asset for breach scoring.  Breaches are
    /// counted over this window rather than required to be strictly
    /// consecutive: the tolerance includes live offer exposure, which swings
    /// by two orders of magnitude between heartbeats as the book is
    /// re-quoted, so a real constant divergence would otherwise keep having
    /// its consecutive counter reset and might never escalate.
    uint32_t observation_window{6};

    /// On sustained divergence, post an `adjust` leg that brings the ledger
    /// back in line and RECORDS the unexplained amount as a discrete entry.
    ///
    /// Without this the ledger drifts monotonically -- the known-unrecorded
    /// flows (taker fills, DBX rewards, external transfers) are all
    /// one-directional -- so the first breach becomes permanent and the only
    /// remaining operator action is to switch the control off.  With it, each
    /// unexplained movement becomes a queryable adjusting entry, which is
    /// both proper accounting treatment and the measurement wanted:
    ///   SELECT SUM(delta_mojos) FROM ledger_entries WHERE event_type='adjust'
    bool     auto_adjust_enabled{true};

    // -- Dexie reward income ingestion ([REWARD-INCOME 2026-08-01]) --------
    //
    // Every offer submission passes claim_rewards=true, so dexie pays DBX
    // liquidity incentives -- previously booked NOWHERE, surfacing as
    // wallet-vs-books divergence that the invariant's adjusting entries
    // absorbed (income reclassified as "unexplained discrepancy").  With
    // this on, the engine scans the reward asset's wallet each heartbeat
    // for dexie's daily payout bursts (many small plain incoming
    // transactions in one block; measured 1-219 mojos per coin vs
    // >= 100,589 mojos for the smallest trading flow), books each as a
    // 'reward' ledger entry at CoinGecko fair value, folds the quantity
    // into the cost basis at that FMV, and accumulates the USD as reward
    // income SEPARATE from trading P&L.  Detection evidence and treatment:
    // accounting/reward_ingest.hpp.
    bool     reward_ingest_enabled{true};

    /// Asset the rewards arrive in: DBX (dexie bucks), mainnet CAT id.
    /// Matches the rewardRate asset of every program on /v1/incentives.
    std::string reward_asset_id{
        "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20"};

    /// Per-coin ceiling (mojos) separating reward coins from trading
    /// flows.  Measured reward coins: 1-219 mojos (41 daily bursts,
    /// 2026-06-11..07-31).  Smallest observed trading flow: 100,589
    /// mojos.  2,000 (2 DBX) gives ~10x headroom over the largest
    /// observed reward coin while staying 50x below the smallest trade.
    std::int64_t reward_max_mojos_per_coin{2'000LL};

    // -- Stablecoin peg monitor (2026-07-30) -------------------------------
    //
    // Accounting values wUSDC.b / wUSDC / USDS at exactly $1.00 -- they are
    // the numeraire, and feeding a live rate into a PERSISTED cost basis
    // recreates the bug class removed in v0.8.0 (a hardcoded 2.70 XCH rate
    // baked into stored basis).  The exposure to an actual depeg is real
    // though, so it is MONITORED instead of being priced in.
    //
    // Note this is not covered by the existing `depeg:` detector, which
    // compares a pair's own mid against a config constant and is registered
    // only for BYC/wUSDC.b -- it can never see wUSDC.b itself move, because
    // wUSDC.b is that pair's quote unit.
    bool     peg_monitor_enabled{true};

    /// CoinGecko `usd-coin` vs $1.00.  Catches a NATIVE USDC depeg.  Clean,
    /// low-noise signal, so a tight threshold is appropriate.
    double   peg_external_warn_pct{1.0};

    /// Implied wUSDC.b value from cex_mid / dex_mid on XCH/wUSDC.b.  Catches
    /// a BRIDGE depeg, which the CoinGecko feed cannot see (native USDC can
    /// hold $1.00 while the wrapper breaks).
    ///
    /// Threshold must clear the structural DEX-vs-CEX basis on this venue.
    /// The engine's existing arbitrage signal is algebraically the same
    /// quantity; across 217 logged samples it ran p50 78 bps, p90 118 bps,
    /// max 218 bps.  3% sits above that observed noise floor.
    double   peg_implied_warn_pct{3.0};

    /// Consecutive breaching observations before alerting.  DEX-vs-CEX basis
    /// spikes are transient; a real depeg persists.
    uint32_t peg_observations{4};
};

struct InventoryAgingConfig {
    bool     enabled{false};                   // Master switch.

    /// Number of blocks an underwater position must age before relaxation
    /// begins.  Default 1000 blocks (~14.4 hours at 52 s/block).
    uint32_t aging_start_blocks{1000};

    /// Maximum allowed loss (in basis points) for aged positions.
    /// Default 50 bps (0.50%).  The effective margin will never go below
    /// -max_loss_relax_bps.
    double   max_loss_relax_bps{50.0};

    /// Rate at which the no-loss floor relaxes, in bps per block, once
    /// the position age exceeds aging_start_blocks.
    /// Default 0.05 bps/block => 50 bps max loss reached after ~1000 extra
    /// blocks (~14.4 hours after aging begins).
    double   relax_rate_bps_per_block{0.05};
};

struct FeeConfig {
    bool     enabled{false};                    // Master switch.

    /// Maximum total blockchain fees the bot may spend in a rolling 24 h
    /// window.  Default 10 000 000 000 mojos (0.01 XCH/day).
    std::uint64_t daily_budget_mojos{10'000'000'000ULL};

    /// Maximum acceptable ratio of fee-to-expected-gain per offer tier.
    /// If fee / expected_gain > this value, the tier is skipped.
    /// Default 0.30 (30%).  0.0 disables fee-vs-gain gating.
    double   fee_to_gain_max_ratio{0.30};

    /// Multiplier applied to the fee in the fee-vs-gain ratio check to
    /// account for the round-trip cost of posting + cancelling an offer.
    /// A value of 2.0 means the gate checks (2×fee)/gain, reflecting that
    /// every offer that doesn't fill will also incur a cancellation fee.
    /// With adaptive fees the cancel may cost more than the post, so
    /// values > 2.0 provide additional margin.  Default 2.0.
    double   cancel_cost_multiplier{2.0};

    /// Absolute fee floor (mojos).  The tracker will never recommend a fee
    /// below this value.  Default 5 000 000 (0.000005 XCH).
    std::uint64_t min_fee_mojos{5'000'000ULL};

    /// Absolute fee ceiling (mojos).  The tracker will never recommend a
    /// fee above this value.  Default 100 000 000 (0.0001 XCH).
    std::uint64_t max_fee_mojos{100'000'000ULL};

    /// When true, query the full node's get_fee_estimate RPC to adapt the
    /// fee dynamically based on mempool congestion.
    bool     adaptive_enabled{true};

    /// Rolling window (in blocks) over which cumulative fees are tracked
    /// for daily budget enforcement.  Default 1662 ≈ 24 h at 52 s/block.
    uint32_t fee_window_blocks{1662};

    /// Target inclusion time (seconds) passed to the full node's
    /// get_fee_estimate RPC.  Lower values request higher fees for faster
    /// inclusion; higher values allow the node to return cheaper estimates.
    /// Market-making offers are long-lived (offer_ttl_blocks ~60), so
    /// urgency is low.  Default 300 s (5 min).
    uint32_t fee_estimate_target_seconds{300};
};

// ---------------------------------------------------------------------------
// Market data aggregation configuration (T4-05).
//
// Exposes VPIN, OFI, whale detection, and competitor detection parameters
// that were previously only code-configurable.  All fields have sensible
// defaults matching MarketDataConfig in execution/market_data.hpp.
// ---------------------------------------------------------------------------
struct MarketDataSettings {
    // -- Whale detection ---------------------------------------------------
    /// Minimum trade size (mojos) to classify as a whale trade.
    /// Default: 50 XCH = 50e12 mojos.
    std::int64_t whale_trade_threshold{50LL * 1'000'000'000'000LL};

    /// Fraction of rolling 24h volume that triggers whale classification.
    double whale_volume_fraction{0.05};

    /// Blocks over which whale events are counted.
    uint32_t whale_window_blocks{10};

    /// Maximum spread multiplier during whale activity.
    double whale_max_spread_multiplier{3.0};

    // -- VPIN (flow toxicity) -----------------------------------------------
    /// Volume per VPIN bucket (base-asset units, e.g. XCH).
    double vpin_bucket_size{10.0};

    /// Number of completed buckets in the rolling VPIN window.
    uint32_t vpin_window_buckets{50};

    // -- OFI (order flow imbalance) ----------------------------------------
    /// Number of order-book snapshots for OFI computation.
    uint32_t ofi_window_size{20};

    // -- Competitor detection -----------------------------------------------
    /// Enable competitor tracking from order book data.
    bool enable_competitor_tracking{true};

    /// Minimum offer size (mojos) to consider as competitor.
    std::int64_t min_competitor_offer_size{1'000'000'000'000LL};

    /// Spread threshold (bps) that triggers a competitor alert.
    double competitor_alert_threshold_bps{50.0};

    // -- Asymmetric spread --------------------------------------------------
    /// Skew factor controlling whale-side asymmetry (0.0–1.0).
    double asymmetric_skew_factor{0.5};

    // -- CEX freshness ------------------------------------------------------
    /// Seconds before CEX data weight decays to zero.
    double cex_freshness_threshold_sec{120.0};

    // -- Order-book-derived mid-price (depth-weighted VWAP micro-price) -----

    /// When true, the aggregator prefers a depth-weighted VWAP micro-price
    /// from competing offers over the simple Dexie BBO midpoint.
    bool orderbook_mid_enabled{true};

    /// Number of order book levels per side to include in the VWAP
    /// micro-price computation.  Default: 5.
    uint32_t orderbook_mid_depth{5};
};

// ---------------------------------------------------------------------------
// Adverse selection (PIN model) configuration (T4-05).
// ---------------------------------------------------------------------------
struct AdverseSelectionSettings {
    double prior_alpha{2.0};         ///< Adverse fill pseudo-count.
    double prior_beta{8.0};          ///< Non-adverse fill pseudo-count.
    uint32_t observation_blocks{10}; ///< Post-fill observation window.
    double adverse_threshold{0.003}; ///< 30 bps adverse classification.
    uint32_t max_history{500};       ///< Rolling fill window.
    double decay_factor{0.0};        ///< Exponential decay in posterior.
};

// ---------------------------------------------------------------------------
// Dynamic market allocator configuration.
//
// Scores each enabled pair on five dimensions (spread, volume, competition,
// fill-rate, triangular-arb) and computes a target capital allocation
// fraction per pair.  Hysteresis and EMA smoothing prevent oscillation.
// ---------------------------------------------------------------------------
struct MarketAllocatorConfig {
    bool     enabled{false};                // Master switch.
    uint32_t eval_interval_blocks{50};      // Re-score every N blocks (~43 min).
    double   min_alloc_pct{0.10};           // Minimum per-pair (10%).
    double   max_alloc_pct{0.50};           // Maximum per-pair (50%).
    double   hysteresis_bps{50.0};          // Score change threshold to act.
    double   smooth_alpha{0.20};            // EMA smoothing (0,1].

    // Dimension weights (normalised internally).
    double   weight_spread{1.0};
    double   weight_volume{1.0};
    double   weight_competition{1.0};
    double   weight_fill_rate{1.0};
    double   weight_tri_arb{1.0};

    // Triangular arbitrage detection.
    double   tri_arb_fee_bps{15.0};         // Per-leg fee for arb calc.
    double   tri_arb_min_edge_bps{5.0};     // Minimum edge to score > 0.
};

// ---------------------------------------------------------------------------
// XCH Recovery Mode -- automatic XCH acquisition when balance critically low.
//
// When XCH spendable drops below `xch_low_threshold`, the engine enters
// recovery mode:
//   1. Cancels all outstanding offers (freeing locked coins).
//   2. Skips Steps 7-8 (no new market-making offers posted).
//   3. Monitors Dexie order books for reasonable XCH-selling asks on
//      XCH-base pairs (e.g. XCH/wUSDC.b) and takes them to acquire XCH.
//   4. Resumes normal trading once XCH spendable > `xch_recovery_target`.
//
// Fees are conserved: only cancellation + recovery takes consume fees.
// ---------------------------------------------------------------------------
struct RecoveryConfig {
    bool     enabled{true};                 // Master switch.
    double   xch_low_threshold{0.25};       // Enter recovery below this (XCH).
    double   xch_recovery_target{1.0};      // Exit recovery above this (XCH).
    double   max_take_per_block_xch{0.5};   // Max XCH to acquire per block.
    double   max_premium_bps{100.0};        // Max premium over CEX price to pay.
    bool     cancel_on_enter{true};         // Cancel all offers on entry.
    double   zero_fee_below_xch{0.001};     // Skip fee when balance is effectively dust.
    std::vector<std::string> pair_allowlist; // Optional XCH-base recovery universe.
};

// ---------------------------------------------------------------------------
// Buyer configuration -- dedicated opportunistic offer-taker flow.
//
// Loaded from a separate buyer.yaml file (path specified in buyer_config_path).
// The buyer runs as its own heartbeat step (Step 9e) and is designed to
// accept/take offers from the Dexie order book when they meet profitability
// criteria.  It is aware of (but does not modify) the maker config settings:
// inventory limits, recovery mode, wallet health, and fee budgets are all
// respected.
//
// Key differences from crossed-book arb (Step 9c):
//   - 9c requires a crossed book (bid >= ask); buyer does not.
//   - 9c is reactive; buyer proactively seeks underpriced offers.
//   - Buyer has its own per-pair rules, caps, and scoring formula.
//   - Buyer config lives in a separate file for independent tuning.
// ---------------------------------------------------------------------------
struct BuyerPairRule {
    std::string pair_name;                  // e.g. "XCH/wUSDC.b"
    bool        enabled{true};
    std::string side{"ask"};                // "ask" = buy base, "bid" = sell base
    double      band_bps{30.0};             // Actionable slack above derived minimum discount floor.
    double      min_edge_bps{12.0};         // Minimum net edge after all costs.
    double      max_take_units{0.25};       // Max size per take (base units).
    double      min_take_units{0.05};       // Min size (avoid micro-takes).
    double      daily_cap_units{5.0};       // Daily take cap (base units).
    double      max_premium_over_cex_bps{50.0}; // Max premium over CEX ref.
    double      inventory_ratio_cap{0.65};  // Max inventory ratio before suppressing.
};

struct BuyerConfig {
    bool        enabled{false};             // Master switch (disabled by default).
    std::string config_path;                // Path to buyer.yaml (empty = inline).

    // -- Global settings (apply to all pairs) --------------------------------
    double      fee_budget_pct{0.3};        // Max pct of daily fee budget for buyer.
    uint32_t    cooldown_blocks{3};         // Min blocks between takes on same pair.
    uint32_t    epoch_blocks{4608};         // Daily cap reset period (~24h).
    double      vpin_max{0.70};             // Global VPIN ceiling.
    double      slippage_buffer_bps{3.0};   // Slippage deduction from edge.
    double      toxicity_buffer_bps{8.0};   // Adverse-selection deduction.
    bool        require_cex_ref{true};      // Require CoinGecko reference.
    uint32_t    max_cex_age_blocks{10};     // Max staleness of CEX ref (~3 min).
    uint32_t    max_takes_per_block{1};     // Global per-block cap.
    bool        respect_recovery_mode{true};// Suppress when recovery active.
    bool        respect_flash_crash{true};  // Suppress during crash states.
    bool        include_relist_credit{true};// Include expected relist profit in edge.
    double      relist_fill_probability{0.5}; // Assumed fill prob for relist credit.

    // -- Per-pair rules (from buyer.yaml pairs section) ----------------------
    std::vector<BuyerPairRule> pair_rules;
};

// ---------------------------------------------------------------------------
// Top-level application configuration aggregating every section.
// ---------------------------------------------------------------------------
struct AppConfig {
    ChiaConfig       chia;
    DexieConfig      dexie;
    std::vector<PairConfig> pairs;
    StrategyConfig   strategy;
    RiskConfig       risk;
    VolatilityConfig volatility;
    MonitoringConfig monitoring;
    DatabaseConfig   database;
    DepegConfig      depeg;
    ArbitrageSettings arbitrage;
    CoinGeckoConfig  coingecko;
    TibetSwapConfig  tibetswap;
    FeeConfig        fees;
    InventoryAgingConfig inventory_aging;
    AccountingConfig accounting;
    MarketDataSettings market_data;
    AdverseSelectionSettings adverse_selection;
    MarketAllocatorConfig market_allocator;
    RecoveryConfig   recovery;
    BuyerConfig      buyer;
};

// ---------------------------------------------------------------------------
// Load and fully validate a YAML configuration file, returning a populated
// AppConfig.  Throws xop::ConfigError on any structural or domain error.
//
// Tilde (~) prefixes in filesystem paths are expanded to the user's HOME.
//
// When `secrets_path` is non-empty, the file is loaded and deep-merged onto
// the base config tree before parsing.  This allows sensitive values (wallet
// fingerprint, SSL paths, API keys) to live in a gitignored secrets.yaml
// while the operational config.yaml remains version-controlled.
// ---------------------------------------------------------------------------
AppConfig load_config(const std::string& path,
                      const std::string& secrets_path = {});

} // namespace xop

#endif // XOP_CONFIG_HPP
