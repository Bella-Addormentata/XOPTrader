/**
 * @file offer_manager.cpp
 * @brief Implementation of the CHIA DEX offer lifecycle manager.
 *
 * See offer_manager.hpp for the full interface contract and design rationale.
 *
 * Key implementation notes:
 *   - Offer creation follows the exact Chia wallet RPC protocol:
 *       offer_dict maps wallet_id (int) -> signed mojo amount.
 *       Negative = we are offering (spending), positive = we request.
 *   - Fill detection uses the Chia trade-record status enum:
 *       0 = PENDING, 4 = CONFIRMED, 5 = CANCELLED, 6 = FAILED.
 *   - The never-sell-at-loss constraint is applied in build_tier_ladder():
 *       every ask price is floored at cost_basis * (1 + min_margin_bps/10000).
 *   - Rebalance triggers are pure functions of the current market state vs.
 *     the stored baseline snapshot -- no side effects.
 *
 * ISO/IEC 27001:2022 -- offer bech32 text logged at DEBUG only (not INFO).
 * ISO/IEC 5055       -- checked arithmetic on mojo conversions; no UB paths.
 * ISO/IEC 25000      -- structured logging with context; deterministic cleanup.
 */

#include <xop/execution/offer_manager.hpp>

#include <xop/execution/cancel_escalation.hpp>
#include <xop/execution/cancel_retry.hpp>
#include <xop/execution/coin_manager.hpp>
#include <xop/execution/cross_guard.hpp>
#include <xop/execution/fee_feedback.hpp>
#include <xop/execution/fill_proof.hpp>
#include <xop/execution/stuck_tx_verdict.hpp>
#include <xop/execution/wallet_circuit.hpp>
#include <xop/execution/wallet_poll_throttle.hpp>
#include <xop/risk/watchdog.hpp>
#include <xop/rpc/rpc_retry_policy.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace xop::execution {

// ---------------------------------------------------------------------------
// Chia wallet trade-record status codes (from chia-blockchain source).
// Newer Chia wallet versions return status as a string enum; older versions
// return an integer.  parse_trade_status() handles both.
// ---------------------------------------------------------------------------
namespace trade_status {
    constexpr int kPendingAccept    = 0;
    // [S25 2026-08-24] 1 and 2 had no names because nothing needed to
    // distinguish them from "not terminal".  recheck_terminal does: a
    // recognised pending state is evidence the offer is live, whereas an
    // unrecognised code (parse returns -1) is no evidence at all.
    constexpr int kPendingConfirm   = 1;
    constexpr int kPendingCancel    = 2;
    constexpr int kCancelled        = 3;
    constexpr int kConfirmed        = 4;
    constexpr int kFailed           = 5;

    inline int parse(const nlohmann::json& status_val) {
        if (status_val.is_number_integer()) {
            return status_val.get<int>();
        }
        if (status_val.is_string()) {
            const auto& s = status_val.get_ref<const std::string&>();
            if (s == "PENDING_ACCEPT")  return 0;
            if (s == "PENDING_CONFIRM") return 1;
            if (s == "PENDING_CANCEL")  return 2;
            if (s == "CANCELLED")       return kCancelled;
            if (s == "CONFIRMED")       return kConfirmed;
            if (s == "FAILED")          return kFailed;
        }
        return -1;  // Unknown status.
    }

    /// [FILL-PROOF, review #171 round 8] A status the wallet really reported:
    /// one of Chia's six TradeStatus codes.  parse() answers -1 for an unknown
    /// string and passes any integer through, so anything outside this range
    /// is no evidence of a state at all -- and in particular none that an
    /// offer the proof holds has left CONFIRMED.
    constexpr bool is_known(int status) noexcept {
        return status >= kPendingAccept && status <= kFailed;
    }

    /// [FILL-PROOF, review #171 round 11] The statuses a cancel writes over
    /// any other, CONFIRMED included.  chia 2.7.4's cancel_pending_offers sets
    /// PENDING_CANCEL (secure) or CANCELLED (insecure) on the trade it cancels
    /// AND on every trade not yet CANCELLED that shares a cancellation coin
    /// with it (get_trades_by_coin) -- whoever sent the cancel, this engine's
    /// sweeps, its per-offer cancels, or the watchdog's.  And a real take does
    /// not fail a pending offer that shared one of its coins: that one stays
    /// PENDING_ACCEPT, so cancelling it later reaches the taken trade.  On an
    /// offer the fill proof holds, one of these may therefore hide a take.
    constexpr bool written_by_a_cancel(int status) noexcept {
        return status == kPendingCancel || status == kCancelled;
    }
}  // namespace trade_status

// ---------------------------------------------------------------------------
// [S74 / review #165] PostingMark -- "a create_offer is out, and the offer it
// makes is not in State yet"
// ---------------------------------------------------------------------------
//
// Engine::shutdown()'s KEEP branch waits while the engine's posting_in_flight_
// flag is set (util::keep_stop_drain_step), because stopping the io_context
// inside this window lets the wallet finish a create with nobody left to record
// it: the next boot meets that offer as an ORPHAN and may CANCEL it, which is
// the one outcome a keep stop exists to prevent.
//
// The window is exactly [create_offer issued .. the offer is in State]. That
// includes the Dexie submission, which sits between the wallet's answer and
// state_->upsert_offer -- an offer created and published but not yet in State
// is the WORST case, since it is unknown to the engine entirely.
//
// One mark per create, never one per ladder: everything created earlier in the
// same post_quotes call is already in State, and a keep stop's flush gives each
// of those an offer_log row. Holding the mark across the ladder would make a
// stop wait for up to 2 x num_tiers creates (12 at the live config) for no
// extra safety -- and would need a budget to match.
//
// RAII, so a throw, an early `continue`, a `break`, a `co_return` and a
// coroutine frame destroyed by ioc_.stop() all clear it. Single-threaded: the
// flag is written here and read by the shutdown continuation, both on the
// io_context thread.
// ---------------------------------------------------------------------------
namespace {

class PostingMark {
public:
    explicit PostingMark(bool* flag) noexcept : flag_(flag)
    {
        if (flag_ != nullptr) {
            *flag_ = true;
        }
    }
    ~PostingMark() { release(); }

    PostingMark(const PostingMark&)            = delete;
    PostingMark& operator=(const PostingMark&) = delete;
    PostingMark(PostingMark&&)                 = delete;
    PostingMark& operator=(PostingMark&&)      = delete;

    /// This create's window is over: a keep stop may proceed.
    ///
    /// [review #165, round 4] IT DOES NOT SAY THE OFFER IS RECORDED. It used
    /// to read "the offer is in State (or there is no offer)", which is a
    /// FALSE assurance in one case the destructor also covers: a create that
    /// threw a TRANSPORT error. A timeout, an empty reply or a 5xx does not
    /// prove the wallet refused the request -- the handler may have built the
    /// offer and only the answer was lost (rpc::request_possibly_submitted,
    /// and PR #162 encodes the same principle for this RPC family: "no answer
    /// is not a refusal"). The mark is released anyway, deliberately: it
    /// exists only to keep the io_context alive until THIS coroutine reaches
    /// state_->upsert_offer, and once the create has thrown there is nothing
    /// left for it to wait for -- holding it would burn the drain budget and
    /// still end with an untracked offer. What the caller does instead is
    /// RECORD the uncertainty (create_outcome_unknown_flag_) so the keep stop
    /// reports a possibly-untracked offer rather than clean success.
    void release() noexcept
    {
        if (flag_ != nullptr) {
            *flag_ = false;
            flag_  = nullptr;
        }
    }

private:
    bool* flag_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

OfferManager::OfferManager(asio::io_context&                    /*ioc*/,
                           std::shared_ptr<rpc::ChiaWalletRPC>  wallet,
                           std::shared_ptr<rpc::DexieClient>    dexie_client,
                           std::shared_ptr<State>               state,
                           const AppConfig&                     config)
    : wallet_(std::move(wallet))
    , dexie_client_(std::move(dexie_client))
    , state_(std::move(state))
    , strategy_cfg_(config.strategy)
    , risk_cfg_(config.risk)
    , dexie_cfg_(config.dexie)
    , logger_(spdlog::default_logger()->clone("OfferMgr"))
    , current_fee_mojos_(config.strategy.offer_fee_mojos)
{
    // Validate tier configuration arrays match declared tier count.
    if (strategy_cfg_.tier_spacing_bps.size() != strategy_cfg_.num_tiers) {
        throw std::invalid_argument(
            "tier_spacing_bps length must equal num_tiers");
    }
    if (strategy_cfg_.tier_size_pct.size() != strategy_cfg_.num_tiers) {
        throw std::invalid_argument(
            "tier_size_pct length must equal num_tiers");
    }

    // Build pair config lookup so evaluate_rebalance() can resolve
    // base/quote asset IDs from a pair name without external help.
    // ISO/IEC 5055: deterministic O(1) lookup, value-copy for safety.
    for (const auto& pc : config.pairs) {
        pair_config_map_.emplace(pc.name, pc);
    }

    // [OFFER-EXPIRY] An on-chain expiry MUST outlast the longest life this
    // bot itself intends for an offer.  If it does not, the chain retires
    // offers the engine still believes are live: no cancel is ever
    // recorded, and the book thins with nothing in the log to explain it.
    // [review #150] Not claimed: that the coins come back.  Expiry makes an
    // offer untakeable; only a cancel or a fill retires the trade, and
    // nothing here documents what get_locked_coins() returns for an expired
    // one -- so this bound protects tracking, not collateral.
    //
    // Checked here rather than in the config parser because this is where
    // kHardTtlMultiplier lives; restating the multiplier in config.cpp
    // would give one safety bound two definitions, free to drift apart.
    //
    // Every pair is checked, not only the global: a per-pair override is
    // exactly where someone sets a short expiry without revisiting the TTL.
    {
        const auto check_expiry =
            [&](std::uint32_t secs, const std::string& who) {
                if (expiry_outlasts_hard_ttl(
                        secs, strategy_cfg_.offer_ttl_blocks,
                        kHardTtlMultiplier,
                        strategy_cfg_.block_time_seconds)) {
                    return;
                }
                const auto floor_s = static_cast<std::int64_t>(
                    hard_ttl_seconds(strategy_cfg_.offer_ttl_blocks,
                                     kHardTtlMultiplier,
                                     strategy_cfg_.block_time_seconds)
                    * kExpiryFloorMargin);
                throw std::invalid_argument(
                    who + " offer_expiry_secs (" + std::to_string(secs)
                    + "s) must exceed " + std::to_string(floor_s)
                    + "s -- the hard TTL (offer_ttl_blocks "
                    + std::to_string(strategy_cfg_.offer_ttl_blocks)
                    + " x " + std::to_string(kHardTtlMultiplier)
                    + " blocks at a configured "
                    + std::to_string(strategy_cfg_.block_time_seconds)
                    + "s/block) times a safety margin, because the real "
                      "block rate varies and an expiry inside the TTL would "
                      "let the chain expire offers this engine still tracks "
                      "as live");
            };

        check_expiry(strategy_cfg_.offer_expiry_secs, "strategy");
        for (const auto& pc : config.pairs) {
            if (pc.offer_expiry_secs_override.has_value()) {
                check_expiry(*pc.offer_expiry_secs_override,
                             "pair " + pc.name);
            }
        }
    }

    logger_->info("OfferManager initialised: {} tiers, TTL {} blocks, "
                  "{} pairs",
                  strategy_cfg_.num_tiers,
                  strategy_cfg_.offer_ttl_blocks,
                  pair_config_map_.size());
}

// ---------------------------------------------------------------------------
// [OFFER-EXPIRY] wiring -- the decisions live in offer_expiry.hpp
// ---------------------------------------------------------------------------

std::optional<std::uint64_t>
OfferManager::expiry_max_time_for(const PairConfig& pair) const
{
    const std::uint32_t secs = effective_offer_expiry_secs(
        pair.offer_expiry_secs_override, strategy_cfg_.offer_expiry_secs);

    const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const auto max_time = expiry_max_time_from(
        static_cast<std::int64_t>(now_s), secs);

    if (secs > 0 && !max_time.has_value()) {
        // Expiry was asked for and we declined to build one: the only way
        // that happens is an unusable clock, which is worth saying out loud
        // rather than silently posting an unbounded offer.
        logger_->error("[offer-expiry] system clock returned {} -- posting "
                       "{} WITHOUT an expiry", now_s, pair.name);
    }
    return max_time;
}

// ---------------------------------------------------------------------------
// [MIN-INPUT-COIN] wiring -- the decisions live in offer_min_input_coin.hpp
// ---------------------------------------------------------------------------

asio::awaitable<json> OfferManager::create_offer_min_coin(
    const json&                  offer_dict,
    std::optional<std::uint64_t> expiry_max_time,
    const PairConfig&            pair,
    Side                         side,
    int                          tier_index,
    const char*                  context)
{
    const double frac = strategy_cfg_.offer_min_input_coin_frac;
    // nullopt for an XCH-funded offer, a disabled fraction, or any dict shape
    // the rule was not designed for: the request is then the old one.
    const std::optional<std::uint64_t> min_coin =
        offer_min_input_coin(offer_dict, frac);

    co_return co_await create_offer_with_min_coin_fallback(
        // Deliberately NOT a coroutine lambda: it hands back the wallet's
        // own awaitable, so nothing reads these captures after it returns.
        [this, &offer_dict, expiry_max_time](
            std::optional<std::uint64_t> coin_floor) {
            return wallet_->create_offer(
                offer_dict, current_fee_mojos_, /*validate_only=*/false,
                expiry_max_time, coin_floor);
        },
        min_coin,
        [this, &pair, side, tier_index, context, min_coin, frac](
            const std::string& refusal) {
            logger_->warn(
                "[min-input-coin] wallet refused {} {} tier {} ({}) under "
                "min_coin_amount={} mojos (strategy."
                "offer_min_input_coin_frac={}): {} -- retrying ONCE without "
                "the floor; the offer may then be built from dust, which "
                "Dexie refuses above about 125 inputs",
                pair.name, to_string(side), tier_index, context,
                min_coin.value_or(0), frac, refusal);
        });
}

void OfferManager::note_dexie_too_many_inputs(const std::string& posting,
                                              std::size_t        offer_chars)
{
    ++dexie_too_many_inputs_count_;

    // [review #162, round 7] THE ADVICE IS COMPUTED, NOT FIXED.  This
    // function is handed the posting and the offer's size and NOTHING about
    // how the offer was built, so it cannot observe whether the no-floor
    // retry ran -- and the shipped text drew a conclusion that needs exactly
    // that fact ("was built with NO floor").
    //
    // What it CAN do is decide whether the fact is needed at all.  The
    // floored create's own CAT leg is bounded at ceil(1 / frac), and while
    // that plus today's one-coin XCH fee leg fits inside Dexie's measured
    // accept, a refusal for input count is arithmetically impossible for a
    // floored create -- so the conclusion is DERIVED, not assumed.  Below
    // that fraction the bound exceeds the limit on its own, a create the
    // wallet SATISFIES can be refused exactly like this one, and the advice
    // inverts.  The range is open at the bottom (config.cpp takes [0, 1)
    // with the low end closed), so that branch is reachable by configuration
    // alone; config.cpp now warns at load as well.
    //
    // NOT THREADED ON PURPOSE.  Handing this function the floor the create
    // sent would sharpen only the second branch, and it would have to be the
    // optional floor, not a "did the fallback run" bool -- an XCH-funded
    // offer and a dict shape the rule skips also send no floor, so a bool
    // would answer "false" for them and seed a fresh wrong inference.  In
    // the shipped branch the fact is deducible without it, and the second
    // branch names the log line that already records it at the moment it
    // happens.
    const double        frac       = strategy_cfg_.offer_min_input_coin_frac;
    const std::uint64_t cat_bound  = min_input_coin_cat_leg_bound(frac);
    const bool          bound_fits = min_input_coin_bound_fits_dexie(frac);
    const double        safe_frac  = min_input_coin_safe_frac();

    std::string advice;
    if (cat_bound == 0) {
        advice = fmt::format(
            "The floor is OFF (strategy.offer_min_input_coin_frac = {}), so "
            "no create carries a bound and this offer was built from "
            "whatever the wallet selected.  Setting it to the default 0.01 "
            "bounds the CAT leg of each FLOORED create at {} inputs, inside "
            "that limit.",
            frac, min_input_coin_cat_leg_bound(0.01));
    } else if (bound_fits) {
        advice = fmt::format(
            "Do NOT RAISE strategy.offer_min_input_coin_frac (now {}): it "
            "bounds the CAT LEG of a FLOORED create at ceil(1 / frac) = {} "
            "inputs, {} with today's one-coin XCH FEE LEG, inside that "
            "limit -- so THIS offer was built with NO floor (the no-floor "
            "retry, or a posting path that carries none: XCH-funded, or a "
            "dict shape the rule skips).  Raising the floor only makes the "
            "wallet refuse more floored creates and fire that retry more "
            "often.  The only fraction change that can help is "
            "a SMALL REDUCTION, never below {:.5f} (= 1 / {}), "
            "where the CAT-leg bound plus the fee coin reaches the limit "
            "exactly.",
            frac, cat_bound, cat_bound + xop::execution::kDexieFeeLegInputsToday,
            safe_frac,
            xop::execution::kDexieMeasuredInputLimit
                - xop::execution::kDexieFeeLegInputsToday);
    } else {
        advice = fmt::format(
            "strategy.offer_min_input_coin_frac is {}, whose CAT-LEG bound "
            "is ceil(1 / frac) = {} inputs -- ABOVE that limit on its own, "
            "before the XCH FEE LEG.  At this fraction the floor is not a "
            "bound Dexie will honour, so this refusal does NOT show that "
            "the no-floor retry ran: a create the wallet SATISFIED can "
            "carry {} inputs and be refused exactly like this one.  "
            "RAISE the fraction to at least {:.5f} (= 1 / {}); "
            "the default 0.01 "
            "bounds it at {}.  To tell the two apart for THIS offer, look "
            "for a preceding '[min-input-coin] wallet refused' line naming "
            "the same pair and tier -- present means the retry ran and no "
            "floor was sent.",
            frac, cat_bound, cat_bound, safe_frac,
            xop::execution::kDexieMeasuredInputLimit
                - xop::execution::kDexieFeeLegInputsToday,
            min_input_coin_cat_leg_bound(0.01));
    }

    logger_->warn(
        "[dexie-too-many-inputs] Dexie refused {} ({} characters): too many "
        "input coins.  The offer EXISTS in the wallet, is listed nowhere and "
        "locks its coins until it is cancelled.  Dexie was measured to "
        "accept {} input coins.  Remedy: combine the small coins of the CAT "
        "this offer spends (chia wallet coins combine) -- the only remedy "
        "that removes the cause; the dust is CAT reward payouts and the CAT "
        "LEG is what the floor bounds.  It does not bound the XCH FEE LEG, "
        "which is a separate selection and is one coin only while every XCH "
        "coin covers the fee; if that leg ever contributes, the coins to "
        "combine there are XCH, not the CAT.  {}  Seen {} time(s) since "
        "start.",
        posting, offer_chars, xop::execution::kDexieMeasuredInputLimit, advice,
        dexie_too_many_inputs_count_);
}

asio::awaitable<void>
OfferManager::retire_offer_failed_expiry(const PendingOffer& adopt,
                                         std::uint64_t expected_max_time,
                                         const char*   context)
{
    logger_->error("[offer-expiry] {} offer {} for {} was created WITHOUT the "
                   "requested expiry (max_time={}); retiring it rather than "
                   "resting an offer whose lifetime we cannot bound",
                   context, adopt.offer_id.substr(0, 12), adopt.pair_name,
                   expected_max_time);

    // Adopt BEFORE cancelling: the cancel is an unconfirmed spend and a
    // counterparty can still take this offer first.  A fill on an unadopted
    // trade is invisible to fill detection and to the next startup.
    state_->upsert_offer(adopt);

    try {
        co_await cancel_offer_charged(
            adopt.offer_id, xop::risk::watchdog_cancel().fee_mojos,
            xop::risk::watchdog_cancel().secure);
        // [review #150] Mark it, exactly as cancel_stale does after its own
        // successful cancel.  Without this the adopted record keeps
        // cancel_pending == false, so the next refresh sees a live-looking
        // offer, fires a SECOND secure cancel, and pays a second fee on a
        // spend already in flight.  The record deliberately stays in State
        // either way: a secure cancel is unconfirmed, and a counterparty can
        // still win the race.
        state_->mark_cancel_pending(adopt.offer_id);
    } catch (const std::exception& e) {
        // [review #150] This path serves BOTH "the wallet dropped max_time"
        // and "the wallet echoed a DIFFERENT max_time" -- expiry_echo_ok
        // compares for equality, and EchoRejectedOnMismatch pins that.  In
        // the second case the offer DOES carry a timelock, at an instant we
        // do not know and which may be SOONER than our own TTL, so calling
        // it unbounded inverts the remediation on the highest-severity page
        // this feature can emit.  Report the echo failure and the value we
        // requested; do not assert an absence we never verified.
        logger_->critical("[offer-expiry] could not cancel {} -- it is LIVE "
                          "with an UNVERIFIED expiry (requested max_time={}, "
                          "not echoed back): {}",
                          adopt.offer_id, expected_max_time, e.what());
        if (escalate_) {
            escalate_("an offer was created without its requested on-chain "
                      "expiry (requested max_time="
                      + std::to_string(expected_max_time)
                      + ", not echoed back) and could NOT be cancelled; it is "
                      "LIVE and its expiry is UNVERIFIED -- it may carry no "
                      "timelock at all, or one at a different time: trade "
                      + adopt.offer_id + " (" + e.what()
                      + "). It IS tracked in State, so fills on it will be "
                      "seen.");
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// [S70 2026-09-20] retire_expired_offers -- ttl_cancel_mode: expire
//
// The decisions are in offer_expiry.hpp; this supplies the wallet's answers.
// Every read is of THIS heartbeat, and the order is fixed: WHO the wallet's
// full-node peers are, then the chain clock (once), then who they are again,
// then per offer the trade record, then the verdict, then -- only on
// RetireLocal -- the one insecure cancel this function exists to send.
//
// [review #164 2026-09-21] The peer census is not decoration.  The clock is
// one connected peer's unvalidated assertion, and acting on a false one frees
// the maker coins of a STILL-TAKEABLE offer with no spend and no undo, with
// the take then invisible to this wallet forever.
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>>
OfferManager::retire_expired_offers(BlockHeight current_block)
{
    std::vector<std::string> retired;
    if (strategy_cfg_.ttl_cancel_mode != TtlCancelMode::Expire) {
        co_return retired;
    }

    // Host clock: a PRE-FILTER only, so a heartbeat with nothing near its
    // expiry costs no RPC.  It never decides a retire.
    const auto host_now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<PendingOffer> due;
    for (const auto& po : state_->get_all_offers()) {
        if (po.cancel_pending) continue;   // a cancel is already in hand
        if (expiry_worth_checking(po.expiry_max_time,
                                  static_cast<std::int64_t>(host_now_s))) {
            due.push_back(po);
        }
    }
    if (due.empty()) {
        co_return retired;
    }

    // The chain clock, read kExpiredRetireDepthBlocks BELOW the height the
    // wallet has FINISHED syncing to.  If the latest transaction block at
    // that depth was already stamped >= an offer's max_time, then no later
    // block can carry a take of it and the block that expired it is buried
    // that deep -- a seconds margin proves neither (see offer_expiry.hpp).
    //
    // [review #164 2026-09-21] AND IT IS ONLY WORTH ANYTHING IF THE PEER THAT
    // ANSWERED IT IS OURS.  get_timestamp_for_height takes the first answer
    // from any connected full node, unanchored and unvalidated, so the wallet
    // is asked WHO those peers are -- before the clock and again after it,
    // because the set can change under the read -- and nothing is retired
    // unless every one of them is on this host both times.  The depth does not
    // help here: forging a timestamp 32 blocks back is exactly as cheap.
    const rpc::TransportCounters pass_start = wallet_->transport_counters();
    // This pass repeats every heartbeat while anything waits past its expiry,
    // and a wallet that cannot answer fails the same way each time: WARN once
    // per kExpiryWarnIntervalBlocks, debug in between.
    const bool warn_now = expiry_warn_due(expiry_warned_block_, current_block);
    const auto problem_level =
        warn_now ? spdlog::level::warn : spdlog::level::debug;
    bool problem_logged = false;

    const auto trust_of = [](const rpc::FullNodePeerCensus& c) {
        return chain_clock_trust(c.readable, c.full_node_peers,
                                 c.non_local_peers);
    };
    const auto trust_error = [](ChainClockTrust t,
                                const rpc::FullNodePeerCensus& c) {
        std::string why = chain_clock_trust_reason(t);
        if (!c.first_non_local_host.empty()) {
            why += " (" + c.first_non_local_host + ")";
        }
        return why;
    };

    std::uint64_t chain_time_s = 0;
    std::string clock_error;
    try {
        const rpc::FullNodePeerCensus before =
            co_await wallet_->get_full_node_peer_census();
        const ChainClockTrust trust_before = trust_of(before);
        if (trust_before != ChainClockTrust::Trusted) {
            clock_error = trust_error(trust_before, before);
        } else {
            const std::int64_t synced_height =
                co_await wallet_->get_height_info();
            const std::int64_t clock_height =
                expired_retire_clock_height(synced_height);
            if (clock_height <= 0) {
                clock_error = "the wallet is fewer than "
                    + std::to_string(kExpiredRetireDepthBlocks)
                    + " blocks into the chain";
            } else {
                const std::uint64_t answered =
                    co_await wallet_->get_timestamp_for_height(clock_height);
                // The peer set can change WHILE the clock is being read, and
                // the RPC never says which peer answered.  Re-ask.
                const rpc::FullNodePeerCensus after =
                    co_await wallet_->get_full_node_peer_census();
                const ChainClockTrust trust_after = trust_of(after);
                if (trust_after != ChainClockTrust::Trusted) {
                    clock_error = "the peer set changed while the clock was "
                                  "being read: " + trust_error(trust_after,
                                                               after);
                } else {
                    chain_time_s = answered;
                }
            }
        }
    } catch (const std::exception& e) {
        clock_error = e.what();
    }
    if (chain_time_s == 0) {
        logger_->log(problem_level,
                     "[offer-expiry] no trusted chain clock this heartbeat "
                     "({}) -- {} offer(s) past their expiry stay tracked and "
                     "keep their coins locked; retiring on an untrusted or "
                     "missing clock is the one mistake this pass cannot undo",
                     clock_error.empty() ? "the wallet returned no timestamp"
                                         : clock_error.c_str(),
                     due.size());
        if (warn_now) expiry_warned_block_ = current_block;
        co_return retired;
    }

    for (const auto& po : due) {
        if (!expired_at_depth(po.expiry_max_time, chain_time_s)) {
            continue;   // expired by the host clock only; ask again later
        }
        if (abort_predicate_ && abort_predicate_()) {
            break;      // a stop is cancelling the book its own way
        }
        if (unanswered_transport_failure_since(
                pass_start, wallet_->transport_counters())) {
            break;      // [WALLET-CIRCUIT] no more calls into a dead wallet
        }

        ExpiredRetire verdict = ExpiredRetire::NotExpired;
        try {
            const json rec = co_await wallet_->get_offer(
                po.offer_id, /*file_contents=*/false);
            const bool pending_accept = rec.contains("status")
                && trade_status::parse(rec["status"])
                       == trade_status::kPendingAccept;
            verdict = decide_expired_retire(pending_accept,
                                            po.expiry_max_time,
                                            trade_record_max_time(rec),
                                            chain_time_s);
        } catch (const std::exception& e) {
            logger_->log(problem_level,
                         "[offer-expiry] get_offer failed for expired {}: {} "
                         "-- left tracked", po.offer_id.substr(0, 12),
                         e.what());
            problem_logged = true;
            continue;
        }

        if (verdict == ExpiredRetire::LeaveToWallet) {
            // CONFIRMED is a fill and `filled` always wins; a cancel status
            // belongs to the path that sent it.  detect_fills owns both.
            logger_->debug("[offer-expiry] {} is past its expiry but the "
                           "wallet no longer reports PENDING_ACCEPT -- left "
                           "to fill detection", po.offer_id.substr(0, 12));
            continue;
        }
        if (verdict == ExpiredRetire::Unverified) {
            // The wallet's record does not repeat the expiry we tracked.
            // Forget it: the offer goes back under the hard TTL, which
            // cancels it securely.
            state_->set_offer_expiry(po.offer_id, 0);
            logger_->warn("[offer-expiry] {} is tracked with max_time={} but "
                          "the wallet record does not carry it -- expiry "
                          "dropped, the hard TTL applies again",
                          po.offer_id.substr(0, 12), po.expiry_max_time);
            continue;
        }
        if (verdict != ExpiredRetire::RetireLocal) {
            continue;
        }
        // A cancel may have been sent for it while this pass was suspended.
        if (state_->get_offer(po.offer_id).cancel_pending) {
            continue;
        }

        try {
            // secure=false, fee 0: the trade goes CANCELLED in the wallet and
            // its coins leave get_locked_coins(); nothing is spent.  Safe
            // ONLY because of the verdict above -- see offer_expiry.hpp.
            co_await cancel_offer_charged(po.offer_id, 0, /*secure=*/false);
        } catch (const std::exception& e) {
            logger_->log(problem_level,
                         "[offer-expiry] local cancel of expired {} failed: "
                         "{} -- retried next heartbeat",
                         po.offer_id.substr(0, 12), e.what());
            problem_logged = true;
            continue;
        }
        // cancel_pending, not removed: only the wallet's CANCELLED verdict,
        // seen by detect_fills, completes the offer_log row (#157).
        // [review #171 round 15] detect_fills keeps any other local cancel
        // tracked while its coins are unspent.  This one is proven expired
        // at depth, so nothing can take it, and it closes on that verdict.
        state_->mark_cancel_pending(po.offer_id);
        expiry_retired_.insert(po.offer_id);
        retired.push_back(po.offer_id);
        logger_->info("[offer-expiry] retired {} ({} {} tier {}) at block {}: "
                      "max_time={} chain_time_at_depth={} (+{}s) -- "
                      "local cancel, no fee, coins released",
                      po.offer_id.substr(0, 12), po.pair_name,
                      to_string(po.side), po.tier, current_block,
                      po.expiry_max_time, chain_time_s,
                      chain_time_s - po.expiry_max_time);
    }

    if (problem_logged && warn_now) {
        expiry_warned_block_ = current_block;
    }
    co_return retired;
}

// ---------------------------------------------------------------------------
// post_quotes -- create multi-tier bid + ask offers on-chain
// ---------------------------------------------------------------------------

asio::awaitable<int> OfferManager::post_quotes(
    const PairConfig&              pair,
    const std::vector<TierQuote>&  quotes,
    BlockHeight                    block_height,
    double                         fee_reserve_override)
{
    // If the caller supplied a positive override (e.g. engine recovery
    // mode), use that instead of the configured fee_reserve_xch.
    const double effective_reserve = (fee_reserve_override > 0.0)
        ? fee_reserve_override
        : strategy_cfg_.fee_reserve_xch;
    // Per-call state for the T5-08 exemption below.
    xch_ledger_suppressed_ = false;
    // Lazy-init the wallet-ID cache on first call.
    if (!wallet_ids_resolved_) {
        co_await init_wallet_id_map();
    }

    const std::int64_t base_wid = resolve_wallet_id(pair.base_asset_id);
    const std::int64_t quote_wid = resolve_wallet_id(pair.quote_asset_id);
    if (base_wid < 0 || quote_wid < 0) {
        logger_->error("Skipping {} -- required wallet IDs are unavailable: "
                       "base='{}' (wid={}), quote='{}' (wid={})",
                       pair.name,
                       pair.base_asset_id, base_wid,
                       pair.quote_asset_id, quote_wid);
        co_return 0;
    }

    // [T7-10] Batch mode: merge same-side tiers into a single RPC call.
    if (strategy_cfg_.batch_offers_enabled && quotes.size() > 1) {
        // Split quotes by side.
        std::vector<TierQuote> bids, asks;
        for (const auto& tq : quotes) {
            if (tq.side == Side::Bid) bids.push_back(tq);
            else                      asks.push_back(tq);
        }

        int bid_count = 0;
        int ask_count = 0;

        // -- XCH fee reserve pre-check (batch mode) ------------------------
        // Verify XCH spendable >= 2x fee_reserve_xch before posting.
        // ALL offers lock XCH UTXOs for on-chain fees, even buy-XCH
        // offers.  Use 2x reserve as the creation floor so that
        // worst-case UTXO locking still preserves the reserve.
        //
        // Recovery zone: when spendable is between 1x and 2x reserve,
        // allow buy-XCH offers through to escape the low-balance
        // deadlock (can't buy XCH because can't create offers because
        // not enough XCH).  Buy-XCH offers will net-increase XCH
        // balance when filled, restoring normal operation.
        bool reserve_breached = false;
        const bool bids_buy_xch = (pair.base_asset_id == "xch");
        const bool asks_buy_xch = (pair.quote_asset_id == "xch");
        if (effective_reserve > 0.0) {
            try {
                auto xch_bal = co_await wallet_->get_wallet_balance(1);
                Mojo xch_spendable = 0;
                if (xch_bal.contains("spendable_balance"))
                    xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
                const auto creation_floor = static_cast<Mojo>(std::llround(
                    effective_reserve * 2.0
                    * static_cast<double>(kMojosPerXch)));
                if (xch_spendable < creation_floor) {
                    // Check recovery zone: 1x reserve <= spendable < 2x.
                    const auto recovery_floor = static_cast<Mojo>(
                        std::llround(effective_reserve
                                     * static_cast<double>(kMojosPerXch)));
                    if (xch_spendable >= recovery_floor) {
                        // Recovery zone: allow buy-XCH side only.
                        logger_->info(
                            "XCH UTXO-lock recovery zone (batch): "
                            "spendable {:.6f} XCH in [{:.3f}, {:.3f}) "
                            "-- allowing buy-XCH offers only for {}",
                            static_cast<double>(xch_spendable) / kMojosPerXch,
                            effective_reserve, effective_reserve * 2.0,
                            pair.name);
                        if (!bids_buy_xch) bids.clear();
                        if (!asks_buy_xch) asks.clear();
                        if (bids.empty() && asks.empty())
                            reserve_breached = true;
                    } else {
                        logger_->warn(
                            "XCH UTXO-lock pre-check (batch): spendable "
                            "{:.6f} XCH < reserve {:.3f} XCH before {} "
                            "-- skipping all offers",
                            static_cast<double>(xch_spendable) / kMojosPerXch,
                            effective_reserve, pair.name);
                        reserve_breached = true;
                    }
                }
            } catch (const std::exception& e) {
                logger_->warn("XCH fee reserve pre-check (batch) failed "
                              "for {}: {}", pair.name, e.what());
            }
        }

        if (!bids.empty() && !reserve_breached) {
            bid_count = co_await post_merged_side(pair, bids, block_height);
        }

        // -- XCH fee reserve guard (batch mode, UTXO-aware) ----------------
        // Check XCH spendable balance after bids; skip asks if below 2x reserve.
        // No buy-XCH exemption: UTXO locking is direction-agnostic.
        if (bid_count > 0 && !asks.empty()
            && effective_reserve > 0.0) {
            try {
                auto xch_bal = co_await wallet_->get_wallet_balance(1);
                Mojo xch_spendable = 0;
                if (xch_bal.contains("spendable_balance"))
                    xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
                const auto creation_floor = static_cast<Mojo>(std::llround(
                    effective_reserve * 2.0
                    * static_cast<double>(kMojosPerXch)));
                if (xch_spendable < creation_floor) {
                    logger_->warn(
                        "XCH UTXO-lock guard (batch): spendable {:.6f} XCH "
                        "< 2x reserve {:.3f} XCH after posting {} bids "
                        "-- skipping ask batch",
                        static_cast<double>(xch_spendable) / kMojosPerXch,
                        effective_reserve * 2.0, pair.name);
                    reserve_breached = true;
                }
            } catch (const std::exception& e) {
                logger_->warn("XCH fee reserve guard (batch): balance check "
                              "failed after {} bids: {}", pair.name, e.what());
            }
        }

        if (!asks.empty() && !reserve_breached) {
            ask_count = co_await post_merged_side(pair, asks, block_height);
        }

        // [T5-08] Asymmetric ladder guard: if one side posted successfully
        // but the other side failed completely, cancel the posted side to
        // prevent running a one-sided book (pure adverse selection).
        // Per Avellaneda-Stoikov (2008), a market maker with only bids
        // (or only asks) is guaranteed to accumulate inventory directionally
        // with no spread capture to offset the risk.
        //
        // Exception: when XCH reserve is breached and the posted side buys
        // XCH, the other side was *intentionally* suppressed.  Do not
        // cancel in that case -- the one-sided book is by design.
        // A ledger suppression is DELIBERATE like a reserve breach, so the
        // exemption below spares the surviving side from cancellation --
        // but only when that survivor buys XCH (buy-XCH offers are
        // cap-exempt yet floor-checked, so they too can be refused and set
        // this flag; a surviving spend side is never exempted).  Without
        // the exemption, cancelling the survivor would burn create+cancel
        // fees every cycle in a livelock (review).
        const bool bid_side_exempt =
            (reserve_breached || xch_ledger_suppressed_) && bids_buy_xch;
        const bool ask_side_exempt =
            (reserve_breached || xch_ledger_suppressed_) && asks_buy_xch;
        if (bid_count > 0 && ask_count == 0 && !asks.empty()
            && !bid_side_exempt) {
            logger_->warn("Asymmetric ladder: {} bids posted but 0/{} asks "
                          "-- cancelling bids to prevent one-sided book",
                          bid_count, asks.size());
            // Cancel the just-posted bids.
            auto bid_offers = state_->get_all_offers();
            for (const auto& po : bid_offers) {
                if (po.pair_name == pair.name &&
                    po.side == Side::Bid &&
                    po.created_at_block == block_height &&
                    !po.cancel_pending) {
                    bool cancel_ok = false;
                    bool needs_emergency = false;
                    try {
                        co_await cancel_offer_charged(
                            po.offer_id, cancel_fee_for(po.offer_id), /*secure=*/true);
                        cancel_ok = true;
                    } catch (const rpc::ChiaRPCError& e) {
                        const std::string_view msg{e.what()};
                        needs_emergency =
                            msg.find("insufficient funds") != std::string_view::npos ||
                            msg.find("spendable balance") != std::string_view::npos;
                        if (!needs_emergency)
                            logger_->error("Failed to cancel asymmetric bid {}: {}",
                                           po.offer_id.substr(0, 12), e.what());
                    }
                    if (needs_emergency)
                        cancel_ok = co_await emergency_cancel(
                            po.offer_id, "asymmetric_bid");
                    if (cancel_ok) {
                        state_->mark_cancel_pending(po.offer_id);
                        --bid_count;
                    }
                }
            }
        } else if (ask_count > 0 && bid_count == 0 && !bids.empty()
                   && !ask_side_exempt) {
            logger_->warn("Asymmetric ladder: {} asks posted but 0/{} bids "
                          "-- cancelling asks to prevent one-sided book",
                          ask_count, bids.size());
            auto ask_offers = state_->get_all_offers();
            for (const auto& po : ask_offers) {
                if (po.pair_name == pair.name &&
                    po.side == Side::Ask &&
                    po.created_at_block == block_height &&
                    !po.cancel_pending) {
                    bool cancel_ok = false;
                    bool needs_emergency = false;
                    try {
                        co_await cancel_offer_charged(
                            po.offer_id, cancel_fee_for(po.offer_id), /*secure=*/true);
                        cancel_ok = true;
                    } catch (const rpc::ChiaRPCError& e) {
                        const std::string_view msg{e.what()};
                        needs_emergency =
                            msg.find("insufficient funds") != std::string_view::npos ||
                            msg.find("spendable balance") != std::string_view::npos;
                        if (!needs_emergency)
                            logger_->error("Failed to cancel asymmetric ask {}: {}",
                                           po.offer_id.substr(0, 12), e.what());
                    }
                    if (needs_emergency)
                        cancel_ok = co_await emergency_cancel(
                            po.offer_id, "asymmetric_ask");
                    if (cancel_ok) {
                        state_->mark_cancel_pending(po.offer_id);
                        --ask_count;
                    }
                }
            }
        }

        int created_count = bid_count + ask_count;
        logger_->info("post_quotes (batched): {}/{} offers created for {}",
                      created_count, quotes.size(), pair.name);
        co_return created_count;
    }

    int created_count = 0;
    bool bid_funds_exhausted = false;
    bool ask_funds_exhausted = false;

    // [XCH-LOCK-LEDGER] Ladder preflight (review round 8): ladders order
    // all bids before all asks, so in this non-batch path a mid-ladder
    // cap exhaustion could admit every bid and then refuse every ask,
    // leaving a one-sided book with no T5-08 cleanup (the guard exists
    // only in the batch branch).  Run the whole ladder against a COPY of
    // the cycle ledger first; a side whose every tier would be refused is
    // dropped up front, and if the surviving side does not buy XCH it is
    // dropped too -- the same exemption rule the batch guard applies.
    if (xch_cycle_ledger_.active()) {
        // Local: the batch branch's equivalents are scoped inside it.
        const bool bids_buy_xch = (pair.base_asset_id == "xch");
        const bool asks_buy_xch = (pair.quote_asset_id == "xch");
        CoinLockLedger probe = xch_cycle_ledger_;
        int  admit_bid = 0, admit_ask = 0;
        bool any_bid = false, any_ask = false;
        for (const auto& tier : quotes) {
            json probe_dict = build_offer_dict(pair, tier);
            if (probe_dict.empty()) {
                continue;
            }
            const bool ok =
                xch_ledger_probe_admits(probe, probe_dict, pair, tier.side);
            if (tier.side == Side::Bid) {
                any_bid = true;
                if (ok) ++admit_bid;
            } else {
                any_ask = true;
                if (ok) ++admit_ask;
            }
        }
        const PreflightDrops drops = preflight_side_drops(
            any_bid, admit_bid, any_ask, admit_ask,
            bids_buy_xch, asks_buy_xch);
        if (drops.drop_asks || drops.drop_bids) {
            ask_funds_exhausted = ask_funds_exhausted || drops.drop_asks;
            bid_funds_exhausted = bid_funds_exhausted || drops.drop_bids;
            xch_ledger_suppressed_ = true;
            logger_->warn("XCH lock ledger preflight: {} dropping{}{} up "
                          "front (one-sided guard)",
                          pair.name,
                          drops.drop_bids ? " bids" : "",
                          drops.drop_asks ? " asks" : "");
        }
    }

    for (const auto& tier : quotes) {
        // Skip further tiers on the side that already reported
        // insufficient funds; the other side may still succeed.
        if (tier.side == Side::Bid && bid_funds_exhausted) continue;
        if (tier.side == Side::Ask && ask_funds_exhausted) continue;

        // -- XCH fee reserve pre-creation guard (per-offer, UTXO-aware) ----
        // Check XCH spendable BEFORE each individual offer creation.
        // The Chia wallet locks entire UTXOs for fee coins -- a single
        // create_offer can lock far more than the 5M mojo fee.  Use 2x
        // the reserve as the creation floor so that even after worst-case
        // UTXO locking, the reserve is preserved.  This prevents the
        // create -> drain -> cancel -> create churn cycle.
        //
        // Recovery zone: when spendable is between 1x and 2x reserve,
        // allow buy-XCH offers through to escape the low-balance
        // deadlock.  Buy-XCH offers net-increase XCH when filled.
        if (effective_reserve > 0.0) {
            try {
                auto xch_bal = co_await wallet_->get_wallet_balance(1);
                Mojo xch_spendable = 0;
                if (xch_bal.contains("spendable_balance"))
                    xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
                // 2x reserve: survive worst-case UTXO lock.
                const auto creation_floor = static_cast<Mojo>(std::llround(
                    effective_reserve * 2.0
                    * static_cast<double>(kMojosPerXch)));
                if (xch_spendable < creation_floor) {
                    // Check recovery zone: 1x <= spendable < 2x.
                    const auto recovery_floor = static_cast<Mojo>(
                        std::llround(effective_reserve
                                     * static_cast<double>(kMojosPerXch)));
                    const bool tier_buys_xch =
                        (tier.side == Side::Bid
                         && pair.base_asset_id == "xch")
                        || (tier.side == Side::Ask
                            && pair.quote_asset_id == "xch");
                    if (xch_spendable >= recovery_floor && tier_buys_xch) {
                        logger_->info(
                            "XCH UTXO-lock recovery: spendable {:.6f} XCH "
                            "in [{:.3f}, {:.3f}) -- allowing {} {} tier {} "
                            "(buys XCH)",
                            static_cast<double>(xch_spendable) / kMojosPerXch,
                            effective_reserve, effective_reserve * 2.0,
                            pair.name, to_string(tier.side), tier.tier_index);
                        // Allow this buy-XCH tier through.
                    } else if (xch_spendable >= recovery_floor) {
                        // Recovery zone but this tier doesn't buy XCH.
                        logger_->debug(
                            "XCH UTXO-lock recovery: skipping non-XCH-buy "
                            "{} {} tier {} (spendable {:.6f})",
                            pair.name, to_string(tier.side), tier.tier_index,
                            static_cast<double>(xch_spendable) / kMojosPerXch);
                        if (tier.side == Side::Bid) bid_funds_exhausted = true;
                        else                        ask_funds_exhausted = true;
                        continue;
                    } else {
                        logger_->warn(
                            "XCH UTXO-lock pre-check: spendable {:.6f} XCH "
                            "< reserve {:.3f} XCH before {} {} tier {} "
                            "-- stopping all offers",
                            static_cast<double>(xch_spendable) / kMojosPerXch,
                            effective_reserve,
                            pair.name, to_string(tier.side), tier.tier_index);
                        bid_funds_exhausted = true;
                        ask_funds_exhausted = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                logger_->warn("XCH UTXO-lock pre-check failed before "
                              "{} tier {}: {} -- skipping cautiously",
                              pair.name, tier.tier_index, e.what());
                bid_funds_exhausted = true;
                ask_funds_exhausted = true;
                break;
            }
        }

        // Step 1: Build the offer_dict for the wallet RPC.
        json offer_dict = build_offer_dict(pair, tier);
        if (offer_dict.empty()) {
            logger_->warn("Skipping tier {} {} -- could not build offer_dict",
                          tier.tier_index, to_string(tier.side));
            continue;
        }

        // [XCH-LOCK-LEDGER] Self-accounted whole-coin budget; the wallet's
        // spendable_balance lags our own just-created offers, so re-querying
        // it (below) cannot stop a fast batch on its own.
        if (!xch_ledger_admits(offer_dict, pair, tier.side, "tier create")) {
            continue;
        }

        // [S31] Consulted before every create, not once at entry. This
        // coroutine creates and publishes several tiers with awaits between
        // them, so a gate at the engine's step boundary stops the NEXT cycle
        // while this one goes on posting -- onto a book the dead man's
        // switch may have just cancelled, with spends still unconfirmed.
        if (abort_predicate_ && abort_predicate_()) {
            logger_->error("aborting offer creation for {} mid-flight: the "
                           "engine asked us to stop", pair.name);
            break;
        }

        // [S74 / review #165, round 4] STOP CREATING, CANCEL NOTHING. A stop
        // has been latched: start no further create. Not the same check as the
        // abort above -- that one cancels a create that landed late, which a
        // KEEP stop must never do. Without this, the keep stop's drain (which
        // suspends on a poll timer, handing control straight back to this
        // coroutine) waited out the rest of the ladder under a budget sized
        // for ONE create, and this loop went on posting new offers after the
        // operator asked the engine to stop.
        if (stop_creating_predicate_ && stop_creating_predicate_()) {
            logger_->warn("not creating any further {} tier: a stop is "
                          "latched. Nothing already created is cancelled.",
                          pair.name);
            break;
        }

        // Step 2: Call wallet.create_offer() to produce the spend bundle.
        // [OFFER-EXPIRY] nullopt unless this pair opted in, in which case
        // the payload is unchanged from before the feature existed.
        const std::optional<std::uint64_t> expiry_max_time =
            expiry_max_time_for(pair);
        // [MIN-INPUT-COIN] Through the floor, like every create here.
        json result;
        // [S74 / review #165] From here until this tier's offer is in State, a
        // keep stop waits rather than stopping the io_context under it.
        PostingMark posting_mark{posting_in_flight_flag_};
        try {
            result = co_await create_offer_min_coin(
                offer_dict, expiry_max_time, pair, tier.side,
                static_cast<int>(tier.tier_index), "tier");
        } catch (const rpc::ChiaRPCTransportError& e) {
            // [review #165, round 4] BEFORE the base handler, which would take
            // this too. A transport failure is not a refusal: the wallet may
            // have built the offer and only the answer was lost. Recorded so
            // the keep stop stops calling this case "no offer" (the
            // PostingMark comment has the whole argument). Nothing is sent
            // and nothing is waited for -- there is no tier id to cancel,
            // and a second create would be a second offer.
            logger_->error("create_offer failed for {} {} tier {}: {}",
                           pair.name, to_string(tier.side),
                           tier.tier_index, e.what());
            note_create_outcome_unknown(e, pair.name, "tier");
            continue;
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("create_offer failed for {} {} tier {}: {}",
                           pair.name, to_string(tier.side),
                           tier.tier_index, e.what());
            const std::string_view msg{e.what()};
            if (msg.find("insufficient funds") != std::string_view::npos ||
                msg.find("spendable balance") != std::string_view::npos) {
                if (tier.side == Side::Bid) {
                    bid_funds_exhausted = true;
                    logger_->warn("Stopping {} BID tiers -- "
                                  "wallet reported insufficient funds",
                                  pair.name);
                } else {
                    ask_funds_exhausted = true;
                    logger_->warn("Stopping {} ASK tiers -- "
                                  "wallet reported insufficient funds",
                                  pair.name);
                }
            }
            continue;
        }

        // [review] CHECK AGAIN, AFTER the create. The pre-call check cannot
        // stop a create already in flight: if the switch's bulk cancel
        // enumerates the book before this trade registers, the coroutine
        // resumes here holding an offer the cancel never saw, and publishing
        // it would leave a live offer behind a fired dead man's switch.
        //
        // The offer exists now, so refusing to publish is not enough -- the
        // coins are locked either way. Cancel the trade we just created.
        if (abort_predicate_ && abort_predicate_()) {
            const std::string late_id = late_trade_id(result, "tier");
            logger_->error("create for {} tier {} landed AFTER the engine "
                           "asked us to stop; cancelling trade {} rather "
                           "than publishing it", pair.name, tier.tier_index,
                           late_id.empty() ? "<unknown>" : late_id);
            if (!late_id.empty()) {
                // [review round 11] Adopt BEFORE cancelling, as the
                // merged path does: a secure cancel is an unconfirmed
                // spend, and a counterparty can still win the race -- a
                // fill on an unadopted trade is invisible to detection and
                // to the next startup.
                {
                    PendingOffer pending;
                    pending.offer_id         = late_id;
                    pending.pair_name        = pair.name;
                    pending.side             = tier.side;
                    pending.price            = tier.price;
                    pending.size             = tier.size;
                    pending.tier             = tier.tier_index;
                    pending.created_at_block = block_height;
                    pending.created_at_ts    = std::chrono::system_clock::now();
                    pending.fee_mojos        = current_fee_mojos_;
                    pending.post_spread_bps  = tier.spread_bps;
                    state_->upsert_offer(pending);
                }
                try {
                    co_await cancel_offer_charged(
                        late_id, xop::risk::watchdog_cancel().fee_mojos,
                        xop::risk::watchdog_cancel().secure);
                } catch (const std::exception& e) {
                    logger_->critical("could not cancel the late offer {}: "
                                      "{} -- it is LIVE and unmanaged",
                                      late_id, e.what());
                    if (escalate_) {
                        escalate_("an offer created after the stop could NOT "
                                  "be cancelled and is LIVE: trade " + late_id
                                  + " (" + e.what() + "). The bulk cancel "
                                  "never saw it, so any earlier "
                                  "cancellation-submitted message does not "
                                  "cover this one.");
                    }
                }
            }
            break;
        }

        // Step 3: Extract the bech32m offer text.
        if (!result.contains("offer") || !result["offer"].is_string()) {
            logger_->error("create_offer response missing 'offer' field "
                           "for {} tier {}", pair.name, tier.tier_index);
            continue;
        }
        std::string offer_text = result["offer"].get<std::string>();

        // Extract the trade_id for lifecycle tracking.
        std::string trade_id;
        if (result.contains("trade_record") &&
            result["trade_record"].contains("trade_id")) {
            trade_id = result["trade_record"]["trade_id"].get<std::string>();
        } else {
            logger_->warn("No trade_id in create_offer response for {} tier {}",
                          pair.name, tier.tier_index);
            continue;
        }

        // [OFFER-EXPIRY] Verified BEFORE dexie: publishing an offer whose
        // expiry silently did not take would advertise a mis-specified
        // offer to the whole market, while we believed it self-retires.
        if (expiry_max_time.has_value()
            && !expiry_echo_ok(result, *expiry_max_time)) {
            PendingOffer adopt;
            adopt.offer_id         = trade_id;
            adopt.pair_name        = pair.name;
            adopt.side             = tier.side;
            adopt.price            = tier.price;
            adopt.size             = tier.size;
            adopt.tier             = tier.tier_index;
            adopt.created_at_block = block_height;
            adopt.created_at_ts    = std::chrono::system_clock::now();
            adopt.fee_mojos        = current_fee_mojos_;
            adopt.post_spread_bps  = tier.spread_bps;
            co_await retire_offer_failed_expiry(adopt, *expiry_max_time,
                                                "tier");
            continue;
        }

        // Step 4: Submit to dexie for cross-platform aggregation (best-effort).
        const std::string dexie_id = co_await submit_to_dexie(
            offer_text,
            fmt::format("{} {} tier {}", pair.name, to_string(tier.side),
                        tier.tier_index));
        if (dexie_id.empty()) {
            logger_->warn("Dexie submission failed for {} tier {} -- "
                          "offer is still valid on-chain, but with no dexie "
                          "id it cannot be excluded from our own arbitrage "
                          "scan",
                          pair.name, tier.tier_index);
        }

        // Step 5: Track as a PendingOffer in shared state.
        PendingOffer pending;
        pending.offer_id         = trade_id;
        pending.pair_name        = pair.name;
        pending.side             = tier.side;
        pending.price            = tier.price;
        pending.size             = tier.size;
        pending.tier             = tier.tier_index;
        pending.created_at_block = block_height;
        pending.created_at_ts    = std::chrono::system_clock::now();
        pending.fee_mojos        = current_fee_mojos_;
        // [WALLET-LOAD] Post-time distance-from-mid, for the fill-poll
        // backoff's striking-distance reset.
        pending.post_spread_bps  = tier.spread_bps;
        // Retain dexie's id -- own-offer exclusion in the arbitrage taker
        // matches the orderbook feed on THIS id, not the wallet trade id.
        pending.dexie_id         = dexie_id;
        // [S70] Reached only past expiry_echo_ok above, so a non-zero value
        // here IS the wallet's echo, not merely what we asked for.
        pending.expiry_max_time  = expiry_max_time.value_or(0);

        state_->upsert_offer(pending);
        // [S74] Recorded: a keep stop that stops the io_context now finds this
        // offer in State, and its flush gives it an offer_log row.
        posting_mark.release();
        ++created_count;

        logger_->info("Posted {} {} tier {} @ {} mojos, size {} mojos [{}]",
                      pair.name, to_string(tier.side), tier.tier_index,
                      tier.price, tier.size, trade_id.substr(0, 12));
        // Full offer text at DEBUG only (ISO/IEC 27001: minimise exposure).
        logger_->debug("Offer text: {}...", offer_text.substr(0, 40));

        // -- XCH UTXO-lock guard (post-creation) ---------------------------
        // The Chia wallet locks entire UTXOs when creating offers, not just
        // the offered amount.  A 0.03 XCH offer can lock a 13 XCH UTXO.
        // Even non-XCH offers lock XCH for fee coins.  Check the actual
        // wallet spendable balance after each creation and stop if below
        // the 2x reserve floor.
        //
        // Recovery zone: if between 1x and 2x reserve, only stop
        // non-buy-XCH tiers.  Buy-XCH tiers are allowed through to
        // help the engine escape the low-balance state.
        if (effective_reserve > 0.0) {
            try {
                auto xch_bal = co_await wallet_->get_wallet_balance(1);
                Mojo xch_spendable = 0;
                if (xch_bal.contains("spendable_balance"))
                    xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
                const auto creation_floor = static_cast<Mojo>(std::llround(
                    effective_reserve * 2.0
                    * static_cast<double>(kMojosPerXch)));
                if (xch_spendable < creation_floor) {
                    const auto recovery_floor = static_cast<Mojo>(
                        std::llround(effective_reserve
                                     * static_cast<double>(kMojosPerXch)));
                    if (xch_spendable >= recovery_floor) {
                        // Recovery zone: stop the sell-XCH side only.
                        const bool bid_sells_xch =
                            (pair.quote_asset_id == "xch");
                        const bool ask_sells_xch =
                            (pair.base_asset_id == "xch");
                        if (bid_sells_xch) bid_funds_exhausted = true;
                        if (ask_sells_xch) ask_funds_exhausted = true;
                    } else {
                        logger_->warn(
                            "XCH UTXO-lock guard: spendable {:.6f} XCH "
                            "< reserve {:.3f} XCH after posting {} {} "
                            "tier {} -- stopping further offers",
                            static_cast<double>(xch_spendable) / kMojosPerXch,
                            effective_reserve,
                            pair.name, to_string(tier.side), tier.tier_index);
                        bid_funds_exhausted = true;
                        ask_funds_exhausted = true;
                    }
                }
            } catch (const std::exception& e) {
                logger_->warn("XCH fee reserve guard: balance check failed "
                              "after {} tier {}: {}",
                              pair.name, tier.tier_index, e.what());
            }
        }
    }

    logger_->info("post_quotes complete: {}/{} offers created for {}",
                  created_count, quotes.size(), pair.name);
    co_return created_count;
}

// ---------------------------------------------------------------------------
// detect_fills -- poll wallet, identify settled offers, emit Fill events
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<Fill>> OfferManager::detect_fills(
    BlockHeight current_block)
{
    std::vector<Fill> fills;

    // [S25] Describes THIS call only; the engine drains it after we return.
    last_terminal_offers_.clear();
    last_dead_offers_.clear();

    // [WALLET-LOAD 2026-08-04] Advance the poll heartbeat counter once per
    // invocation -- the backoff schedule below is phased on it.
    //
    // [FILL-PROOF, review #171 round 4] It also numbers the call a fill proof
    // belongs to.  Advanced here, before the first await, it makes every proof
    // an earlier call made stale for the whole of this one, so a cancel that
    // runs while this call waits is withheld.  The block could not do this:
    // two calls at one height would share it.
    ++fill_poll_heartbeat_;
    // [review #171 round 19] For cancel_all's depth check of a Dead proof.
    latest_fill_poll_block_ = current_block;

    // Get all known pending offers from state for comparison.
    auto pending_offers = state_->get_all_offers();
    if (pending_offers.empty()) {
        fill_poll_pending_counts_.clear();
        fill_proof_deferrals_.clear();
        cancel_status_proven_.clear();
        expiry_retired_.clear();
        cancel_status_inconclusive_.clear();
        local_cancel_live_.clear();
        co_return fills;
    }

    // Build a lookup set of pending offer IDs for O(1) membership testing.
    std::unordered_map<std::string, PendingOffer> pending_map;
    pending_map.reserve(pending_offers.size());
    for (auto& po : pending_offers) {
        pending_map.emplace(po.offer_id, std::move(po));
    }

    // Prune throttle bookkeeping for offers no longer tracked.
    for (auto it = fill_poll_pending_counts_.begin();
         it != fill_poll_pending_counts_.end();) {
        it = pending_map.count(it->first)
                 ? std::next(it)
                 : fill_poll_pending_counts_.erase(it);
    }
    for (auto it = cancel_status_proven_.begin(); it != cancel_status_proven_.end();) {
        it = pending_map.count(it->first) ? std::next(it) : cancel_status_proven_.erase(it);
    }
    for (auto it = expiry_retired_.begin(); it != expiry_retired_.end();) {
        it = pending_map.count(*it) ? std::next(it) : expiry_retired_.erase(it);
    }
    for (auto it = cancel_status_inconclusive_.begin(); it != cancel_status_inconclusive_.end();) {
        it = pending_map.count(it->first) ? std::next(it) : cancel_status_inconclusive_.erase(it);
    }
    for (auto it = local_cancel_live_.begin(); it != local_cancel_live_.end();) {
        it = pending_map.count(*it) ? std::next(it) : local_cancel_live_.erase(it);
    }
    for (auto it = fill_proof_deferrals_.begin(); it != fill_proof_deferrals_.end();) {
        it = pending_map.count(it->first) ? std::next(it) : fill_proof_deferrals_.erase(it);
    }

    // Query only offers we currently track AND due for a poll this
    // heartbeat.  [WALLET-LOAD 2026-08-04] This was one get_offer per
    // tracked offer (~26) EVERY heartbeat, a major share of the load that
    // froze the wallet daemon.  Throttles (wallet_poll_throttle.hpp):
    //   - age gate: a just-posted offer cannot have settled;
    //   - backoff: an offer still PENDING_ACCEPT after M consecutive
    //     polls is polled every Kth heartbeat, reset to every-heartbeat
    //     the moment the book mid is within 2x its post-time
    //     distance-from-mid or a cancel is pending on it.
    // Safety: a fill in a skipped heartbeat is DELAYED detection, never
    // lost -- CONFIRMED offers stay tracked in State until this function
    // processes them (cae2bfd), and the completeness sweep catches
    // stragglers.
    std::vector<json>        trade_records;
    std::vector<std::string> polled_ids;
    std::size_t              skipped_age = 0, skipped_backoff = 0;
    trade_records.reserve(pending_map.size());
    // [WALLET-CIRCUIT 2026-09-13] After one get_offer fails at the transport
    // level, every later one in this loop is another ~15.5 s of doomed
    // retries -- block 9284260 issued six in a row.  The mark is LOCAL to this
    // call, so the first due poll is always issued; a poll skipped here is
    // exactly a throttled one -- delayed detection, never a lost fill.
    const rpc::TransportCounters poll_start = wallet_->transport_counters();
    std::size_t skipped_transport = 0;
    for (const auto& [trade_id, po] : pending_map) {
        const std::int64_t age_blocks =
            (current_block > 0 && po.created_at_block > 0
             && current_block >= po.created_at_block)
                ? static_cast<std::int64_t>(current_block
                                            - po.created_at_block)
                : -1;   // unknown -> age gate does not apply

        // Striking distance: current mid vs the offer's post-time
        // distance.  cancel_pending offers are near resolution and are
        // always polled at full cadence.
        const Mojo mid = state_->get_market(po.pair_name).mid_price;
        const bool striking = po.cancel_pending
            || execution::within_striking_distance(po.price, mid,
                                                   po.post_spread_bps);
        if (striking) {
            fill_poll_pending_counts_.erase(trade_id);
        }

        auto cnt_it = fill_poll_pending_counts_.find(trade_id);
        const std::uint32_t consecutive =
            (cnt_it != fill_poll_pending_counts_.end()) ? cnt_it->second
                                                        : 0;
        if (!execution::fill_poll_due(
                age_blocks,
                strategy_cfg_.detect_fills_min_age_blocks,
                consecutive,
                strategy_cfg_.detect_fills_backoff_polls,
                strategy_cfg_.detect_fills_backoff_interval,
                fill_poll_heartbeat_,
                striking)) {
            if (age_blocks >= 0
                && age_blocks < static_cast<std::int64_t>(
                       strategy_cfg_.detect_fills_min_age_blocks)) {
                ++skipped_age;
            } else {
                ++skipped_backoff;
            }
            continue;
        }

        if (unanswered_transport_failure_since(
                poll_start, wallet_->transport_counters())) {
            ++skipped_transport;
            continue;
        }

        try {
            json rec = co_await wallet_->get_offer(trade_id, /*file_contents=*/false);
            // [FILL-PROOF, review #171 round 4] HELD FROM THE POLL THAT READS
            // CONFIRMED.  The guard used to be entered only after this offer's
            // own proof lookup, and every await before that -- this loop's
            // later polls included -- let a detached Cancel All or the shutdown
            // ladder run, find no guard, and report the offer cancelled or
            // overwrite the CONFIRMED the proof waits on.  No await comes
            // between the read and the hold.  A new entry is never cancellable,
            // and an existing one keeps its count for the log.
            //
            // [review #171 round 6] AND RELEASED BY THE POLL THAT READS ANYTHING
            // ELSE.  A hold left for the erase after this loop would refuse, at
            // every later await here, the cancel of an offer the wallet has
            // already taken out of CONFIRMED.  That erase stays as a fallback.
            //
            // [review #171 round 8] Released only by a status the wallet really
            // reported.  An unrecognised one is no evidence the offer left
            // CONFIRMED, so the hold stays until a poll reads one that is.
            //
            // [review #171 round 11] And not by a status a cancel writes: a
            // cancel of another offer sharing one of its coins overwrites the
            // CONFIRMED of a real take (trade_status::written_by_a_cancel).
            // The loop below proves such an offer on-chain once more, and
            // releases it only when the chain shows no take.
            if (const auto st = rec.find("status"); st != rec.end()) {
                const int polled = trade_status::parse(*st);
                if (polled == trade_status::kConfirmed) {
                    fill_proof_deferrals_.try_emplace(trade_id);
                } else if (trade_status::is_known(polled)
                           && !trade_status::written_by_a_cancel(polled)) {
                    fill_proof_deferrals_.erase(trade_id);
                }
            }
            trade_records.push_back(std::move(rec));
            polled_ids.push_back(trade_id);
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("get_offer failed during fill detection for {}: {}",
                           trade_id.substr(0, 12), e.what());
        }
    }

    if (skipped_transport > 0) {
        logger_->warn("detect_fills: wallet transport failed -- {} due "
                      "poll(s) deferred to the next heartbeat (delayed "
                      "detection, never a lost fill)",
                      skipped_transport);
    }
    if (skipped_age + skipped_backoff > 0) {
        logger_->debug("detect_fills: polled {}/{} tracked offers "
                       "(skipped {} too-young, {} backed-off)",
                       polled_ids.size(), pending_map.size(),
                       skipped_age, skipped_backoff);
    }

    // Update the backoff counters from what the wallet reported: still
    // PENDING_ACCEPT extends the streak; any other status ends it.
    {
        std::unordered_map<std::string, int> polled_status;
        for (const auto& rec : trade_records) {
            if (rec.contains("trade_id") && rec.contains("status")) {
                polled_status[rec["trade_id"].get<std::string>()] =
                    trade_status::parse(rec["status"]);
            }
        }
        for (const auto& id : polled_ids) {
            auto st = polled_status.find(id);
            if (st != polled_status.end()
                && st->second == trade_status::kPendingAccept) {
                ++fill_poll_pending_counts_[id];
            } else {
                fill_poll_pending_counts_.erase(id);
            }
        }
    }

    // [S70] Expiry backfill.  The boot restore rebuilds State from offer_log
    // (pending_offer_from_db), which has no expiry column, so every restored
    // offer comes back with expiry_max_time == 0 and would fall to the hard
    // TTL under ttl_cancel_mode: expire.  The record this poll just fetched
    // carries the wallet's own valid_times, so read it back from there -- no
    // extra RPC, and Step 2 runs before Step 8 in the same heartbeat.  Expire
    // mode only: in the default mode nothing reads the field.
    if (strategy_cfg_.ttl_cancel_mode == TtlCancelMode::Expire) {
        for (const auto& rec : trade_records) {
            if (!rec.contains("trade_id") || !rec["trade_id"].is_string()
                || !rec.contains("status")
                || trade_status::parse(rec["status"])
                       != trade_status::kPendingAccept) {
                continue;
            }
            const auto id = rec["trade_id"].get<std::string>();
            const auto it = pending_map.find(id);
            if (it == pending_map.end() || it->second.expiry_max_time != 0) {
                continue;
            }
            const std::uint64_t max_time = trade_record_max_time(rec);
            if (max_time > 0 && state_->set_offer_expiry(id, max_time)) {
                logger_->info("[offer-expiry] {} carries max_time={} per the "
                              "wallet record -- tracked for expiry",
                              id.substr(0, 12), max_time);
            }
        }
    }

    // [FILL-PROOF] Set by the first coin lookup that fails; see below.
    // [review #171 round 15] Not by one the node or wallet refuses, which is
    // that offer's alone.
    bool proof_lookup_failed = false;
    for (const auto& rec : trade_records) {
        // Extract trade_id and status from the record.
        if (!rec.contains("trade_id") || !rec.contains("status")) {
            continue;
        }
        std::string trade_id = rec["trade_id"].get<std::string>();
        int status = trade_status::parse(rec["status"]);

        // Only process records that are in our pending map.
        auto it = pending_map.find(trade_id);
        if (it == pending_map.end()) {
            continue;
        }

        const PendingOffer& po = it->second;

        // [FILL-PROOF, review #171 round 2] Under proof only while the wallet
        // says CONFIRMED.  An entry left behind would go on withholding the
        // cancels of an offer the wallet has since called live or cancelling.
        // [round 8] A status the wallet really reported, as at the poll.
        // [round 11] Other than one a cancel writes, as at the poll.
        if (status != trade_status::kConfirmed && trade_status::is_known(status)
            && !trade_status::written_by_a_cancel(status)) {
            fill_proof_deferrals_.erase(trade_id);
        }

        // [FILL-PROOF, review #171 round 11] A CANCEL'S STATUS HIDES NOTHING
        // FROM THE PROOF.  A cancel may have overwritten a real take -- one
        // sharing a coin with an offer that was cancelled, by anything that
        // cancels -- and Chia keeps the record's coins and confirmed_at_index,
        // so the proof can still be asked.
        //
        // [round 12] Not only for an offer held here, which read CONFIRMED at
        // an earlier poll of this process.  The overwrite can come before that
        // poll, and a restart forgets every hold.  So an offer is proven the
        // first time it shows each cancel status, and while it is held, every
        // heartbeat.  About one lookup per cancel status, since
        // cancel_status_proven_ remembers a final answer.  The chain decides:
        //   - Settled: a take after all.  Booked below as the CONFIRMED offer
        //     it was, from this proof, with nothing asked twice;
        //   - Live or Dead: no take, so the cancel's status stands.  A hold is
        //     released, and the offer is handled as that status, as before.
        //     [round 14] Only Dead is remembered: a Live offer can still be
        //     taken, so it is asked again next heartbeat;
        //     [round 15] and a Live offer the wallet calls CANCELLED is not
        //     closed.  Every maker coin is unspent, so it was cancelled
        //     locally -- emergency_cancel's last resort, or the wallet's own
        //     UI -- and can still be taken, while the wallet watches no
        //     CANCELLED trade's coins (chia 2.7.4 get_trades_by_coin) and
        //     would never report the take.  It stays tracked, cancel_pending,
        //     unless retire_expired_offers proved it expired before it
        //     cancelled it: nothing can take that one;
        //     [round 16] And Dead only once the spend that killed the offer
        //     is confirmation_depth_blocks deep, as for a CONFIRMED offer
        //     (dead_offer_closable).  A shallower spend can be reorganised
        //     out, and the wallet would go on reporting its cancel's status
        //     over an offer that can be taken again: chia 2.7.4's
        //     reorg_rollback leaves its trade records alone.  Until then a
        //     held offer stays held, and any other is kept tracked,
        //     cancel_pending, and asked again next heartbeat;
        //   - Unknown, held: still held, and asked again next heartbeat, as a
        //     CONFIRMED offer would be;
        //   - Unknown, not held, after a lookup failed -- its own, refused or
        //     failed, or an earlier one this heartbeat: asked again next
        //     heartbeat, and its status waits;
        //   - [review #172] Unknown from an answer that settled nothing -- not
        //     every maker coin covered, a record that cannot be read, the
        //     wallet's silence: a node catching up can still complete it, so
        //     it is asked again every heartbeat, cancel_pending, for
        //     confirmation_depth_blocks from the first such answer under this
        //     status, and only then does the status stand;
        //   - Unknown with nothing to ask -- no readable maker coin, or no
        //     settlement coin or requested amount to look for: no retry can
        //     answer it, so the status stands.  An offer never seen CONFIRMED
        //     is not held on a question that no retry will answer.
        //
        // [review #171 round 14] A cancel is in flight for an offer the wallet
        // reports PENDING_CANCEL, whoever sent it -- this engine's sweeps, the
        // watchdog's, or a sibling's cancel through a shared coin -- so State
        // says so too, as recheck_terminal's revival does.  Otherwise, once a
        // proof below releases the hold, the TTL and reprice paths would read
        // the offer as cancellable and send a second secure cancel for the same
        // coins.  Before the proof, so no path through it can skip the mark.
        if (status == trade_status::kPendingCancel) {
            state_->mark_cancel_pending(trade_id);
        }

        std::optional<FillProofResult> reproved;
        bool reproved_asked_node = false;
        const bool held = fill_proof_deferrals_.count(trade_id) > 0U;
        bool reprove = false;
        if (trade_status::written_by_a_cancel(status)) {
            const auto seen = cancel_status_proven_.find(trade_id);
            reprove = held || seen == cancel_status_proven_.end() || seen->second != status;
        }
        if (reprove) {
            std::string reproof_failure;
            FillProofResult reproof;
            ProofLookup reproof_lookup = ProofLookup::Answered;
            if (proof_lookup_failed) {
                reproof_lookup = ProofLookup::Failed;
                reproof_failure = "an earlier coin lookup this heartbeat failed -- "
                                  "not asked again until the next one";
            } else {
                reproof = co_await prove_fill_on_chain(rec, reproof_failure,
                                                       reproof_lookup,
                                                       reproved_asked_node);
                if (reproof_lookup == ProofLookup::Failed) {
                    proof_lookup_failed = true;
                }
            }
            // [review #171 round 16] Dead is final only at confirmation depth.
            const bool dead_at_depth = dead_offer_closable(
                reproof, current_block, strategy_cfg_.confirmation_depth_blocks);
            // [review #172] A conclusive answer ends a run of inconclusive ones.
            if (reproof.verdict != FillProof::Unknown) {
                cancel_status_inconclusive_.erase(trade_id);
            }
            // [review #171 round 18] Taken, or [round 19] dead at confirmation
            // depth: no longer takeable.  A shallower spend could still be
            // reorganised out, so a live local cancel stays remembered.
            if (dead_at_depth || reproof.verdict == FillProof::Settled) {
                local_cancel_live_.erase(trade_id);
            }
            if (reproof.verdict == FillProof::Settled) {
                logger_->warn("[FILL-PROOF] {} ({}): the wallet reports it {}, but "
                              "the chain shows the take -- a cancel of an offer "
                              "sharing its coins overwrote the status; booked as "
                              "the fill it is",
                              trade_id.substr(0, 12), po.pair_name,
                              status == trade_status::kPendingCancel ? "PENDING_CANCEL"
                                                                     : "CANCELLED");
                reproved = reproof;
                status = trade_status::kConfirmed;
            } else if (reproof.verdict == FillProof::Live || dead_at_depth) {
                fill_proof_deferrals_.erase(trade_id);
                // [round 14] Only Dead is final.  Live -- every maker coin
                // unspent -- can still be taken, and a take that a later cancel
                // overwrites again would show this same status, so a Live offer
                // is asked again next heartbeat: one lookup, until its cancel
                // lands or its status changes.  [round 16] And Dead only at
                // confirmation depth.
                if (dead_at_depth) {
                    cancel_status_proven_[trade_id] = status;
                } else if (status == trade_status::kCancelled
                           && expiry_retired_.count(trade_id) == 0U) {
                    // [review #171 round 15] Cancelled locally and still
                    // takeable: tracked until the chain shows it taken
                    // (booked) or dead (closed), and cancel_pending, so
                    // nothing cancels it again meanwhile.  [round 18] Nor is it
                    // reported as a cancel in flight (cancel_ids).
                    state_->mark_cancel_pending(trade_id);
                    local_cancel_live_.insert(trade_id);
                    logger_->debug("[FILL-PROOF] {} ({}): the wallet reports it "
                                   "CANCELLED, but every maker coin is unspent -- a "
                                   "local cancel, still takeable; kept tracked",
                                   trade_id.substr(0, 12), po.pair_name);
                    continue;
                }
            } else if (held) {
                // Unknown, or [round 16] Dead not yet at depth: still held, as
                // a CONFIRMED offer would be.
                std::uint64_t claimed_height = 0;
                if (const auto idx = rec.find("confirmed_at_index");
                    idx != rec.end() && idx->is_number_unsigned()) {
                    claimed_height = idx->get<std::uint64_t>();
                }
                handle_unproven_fill(trade_id, po, reproof, reproof_failure,
                                     reproved_asked_node, claimed_height, current_block);
                continue;
            } else if (reproof.verdict == FillProof::Dead) {
                // [review #171 round 16] Dead, but the spend is not yet deep
                // enough to close on: kept tracked, cancel_pending, and
                // proven again next heartbeat, until it is -- or until a
                // reorganisation undoes it and the chain shows the offer live.
                state_->mark_cancel_pending(trade_id);
                logger_->debug("[FILL-PROOF] {} ({}): dead at block {}, not yet "
                               "{} blocks deep -- kept tracked until it is",
                               trade_id.substr(0, 12), po.pair_name, reproof.height,
                               strategy_cfg_.confirmation_depth_blocks);
                continue;
            } else if (reproof_lookup == ProofLookup::Refused
                       || reproof_lookup == ProofLookup::Failed) {
                logger_->debug("[FILL-PROOF] {} ({}): a coin lookup failed this "
                               "heartbeat -- its cancel status is proven next "
                               "heartbeat", trade_id.substr(0, 12), po.pair_name);
                continue;
            } else if (reproof_lookup == ProofLookup::Answered) {
                // [review #172] An answer that settled nothing may yet be
                // completed, so it is asked again every heartbeat,
                // cancel_pending, for confirmation_depth_blocks from the first
                // such answer under this status.  Only then does it stand.
                auto window = cancel_status_inconclusive_.try_emplace(
                    trade_id, InconclusiveSince{status, current_block}).first;
                if (window->second.status != status) {
                    window->second = InconclusiveSince{status, current_block};
                }
                if (current_block < window->second.block
                    || current_block - window->second.block
                           < strategy_cfg_.confirmation_depth_blocks) {
                    state_->mark_cancel_pending(trade_id);
                    logger_->debug("[FILL-PROOF] {} ({}): the chain's answer proves "
                                   "nothing yet -- asked again every heartbeat until "
                                   "block {}", trade_id.substr(0, 12), po.pair_name,
                                   window->second.block
                                       + strategy_cfg_.confirmation_depth_blocks);
                    continue;
                }
                cancel_status_inconclusive_.erase(window);
                cancel_status_proven_[trade_id] = status;
            } else {
                // NothingToAsk: the record gives the proof nothing to ask, and
                // no retry can change that, so the status stands.
                cancel_status_proven_[trade_id] = status;
            }
        }

        if (status == trade_status::kConfirmed) {
                // [FILL-PROOF 2026-09-23] The wallet's CONFIRMED is its own
                // bookkeeping, not evidence: on 2026-09-22 it reported three
                // offers CONFIRMED that were never taken, and they were booked
                // as fills (execution/fill_proof.hpp).  Nothing below -- the
                // Fill, State, the removal from tracking -- happens until the
                // chain shows every maker coin spent in one block.
                //
                // After one lookup fails, the rest of this call asks nothing:
                // each failed attempt spends its transport retries, and the
                // S14 escalation stops its sweep on the same failure.
                // [review #171 round 15] Not after one the node or wallet
                // refuses -- the wallet refuses a request naming a coin it
                // does not hold.  That costs one round trip and is this
                // offer's alone, and offers are polled in unordered_map
                // order: the same one could come first every heartbeat and
                // keep every fill after it unproven.
                std::string proof_failure;
                FillProofResult proof;
                bool asked_node = false;
                if (reproved) {
                    // [round 11] Proven just above, under a cancel's status.
                    proof      = *reproved;
                    asked_node = reproved_asked_node;
                } else if (proof_lookup_failed) {
                    proof_failure = "an earlier coin lookup this heartbeat failed -- "
                                    "not asked again until the next one";
                } else {
                    ProofLookup lookup = ProofLookup::Answered;
                    proof = co_await prove_fill_on_chain(rec, proof_failure, lookup,
                                                         asked_node);
                    if (lookup == ProofLookup::Failed) {
                        proof_lookup_failed = true;
                    }
                }
                if (proof.verdict != FillProof::Settled) {
                    std::uint64_t claimed_height = 0;
                    if (const auto idx = rec.find("confirmed_at_index");
                        idx != rec.end() && idx->is_number_unsigned()) {
                        claimed_height = idx->get<std::uint64_t>();
                    }
                    handle_unproven_fill(trade_id, po, proof, proof_failure, asked_node,
                                         claimed_height, current_block);
                    continue;
                }
                fill_proof_deferrals_.erase(trade_id);

                // Offer was taken and settled -- this is a fill.
                Fill fill;
                fill.offer_id     = trade_id;
                fill.pair_name    = po.pair_name;
                fill.side         = po.side;
                fill.timestamp    = std::chrono::system_clock::now();

                // [SETTLED-FIX 2026-08-02] Populate price/size from the
                // SETTLED amounts in the wallet record's summary, not the
                // POSTED PendingOffer values.  Copying the posted values
                // silently discarded the executed amounts, so any
                // settled-vs-posted divergence (partial settlement of a
                // splittable offer, taker-side rounding) corrupted the
                // trade_log and P&L.  Fall back to posted values with a
                // WARNING when the summary is absent or malformed --
                // never silently.  Conversion formula + dimensional
                // analysis: see parse_settled_fill below.
                const auto pc_it = pair_config_map_.find(po.pair_name);
                std::optional<SettledFill> settled;
                if (pc_it != pair_config_map_.end()) {
                    settled = parse_settled_fill(rec, po.side, pc_it->second);
                }
                if (settled.has_value()) {
                    fill.price = settled->price;
                    fill.size  = settled->size;
                    if (settled->size != po.size ||
                        settled->price != po.price) {
                        logger_->info(
                            "detect_fills: {} settled amounts differ from "
                            "posted (size {} -> {}, price {} -> {})",
                            trade_id.substr(0, 12), po.size, settled->size,
                            po.price, settled->price);
                    }
                } else {
                    fill.price = po.price;
                    fill.size  = po.size;
                    logger_->warn(
                        "detect_fills: {} settled summary missing or "
                        "unparseable -- falling back to POSTED size={} "
                        "price={}",
                        trade_id.substr(0, 12), po.size, po.price);
                }
                // [FEE-FIX 2026-07-30] Capture the offer-creation fee NOW,
                // while the PendingOffer is still tracked.  The engine used
                // to look the fee up from State after this function had
                // already called remove_offer(), which always returned a
                // default (fee 0) -- trade_log.fee_mojos was silently 0 for
                // every fill since June 2026.
                fill.fee_mojos    = to_mojo_saturating(po.fee_mojos);

                // [FILL-PROOF] The height the chain proved the take at, which
                // the confirmation-depth buffer counts from.  For a take the
                // wallet saw itself its confirmed_at_index is the same
                // number; for an offer it had already mislabelled CONFIRMED
                // and that was then really taken, the wallet's number is the
                // stale one, and would let the buffer book a fresh take at
                // once.
                fill.block_height = static_cast<BlockHeight>(proof.height);
                if (const auto idx = rec.find("confirmed_at_index");
                    idx != rec.end() && idx->is_number_unsigned()
                    && idx->get<std::uint64_t>() != proof.height) {
                    logger_->warn("detect_fills: {} the wallet says confirmed at "
                                  "{}, the chain says taken at {} -- using the chain's",
                                  trade_id.substr(0, 12), idx->get<std::uint64_t>(),
                                  proof.height);
                }

                // T1-08: Update position accounting using canonical asset IDs
                // from the pair config, NOT the human-readable pair_name.
                // State::record_buy/record_sell index positions by AssetId
                // (e.g. "xch", hex CAT ID), so passing pair_name (e.g.
                // "XCH/wUSDC") would create phantom position entries keyed
                // by the pair label rather than updating the actual asset
                // balances.  The pair_config_map_ was added to support this
                // lookup.
                //
                // [T5-02] ISO/IEC 5055: position accounting failures must
                // NOT prevent the fill from being emitted or the offer from
                // being removed.  The wallet's confirmed fill is the
                // authoritative record; inventory discrepancy is correctable
                // but a missed fill is not.
                // (pc_it was resolved above for the settled-amount parse.)
                if (pc_it == pair_config_map_.end()) {
                    logger_->error(
                        "detect_fills: no pair config for '{}' -- "
                        "cannot update position for trade {}",
                        po.pair_name, trade_id.substr(0, 12));
                } else {
                    const auto& pc = pc_it->second;
                    // [SETTLED-FIX 2026-08-02] Position accounting uses the
                    // fill's (settled when available) size/price so that
                    // inventory moves match what actually settled on-chain.
                    const double quote_mojos_dbl = quote_mojos_for(
                        static_cast<double>(fill.size),
                        static_cast<double>(fill.price),
                        static_cast<double>(pc.base_mojos_per_unit),
                        static_cast<double>(pc.quote_mojos_per_unit));
                    const Mojo quote_mojos = static_cast<Mojo>(std::llround(quote_mojos_dbl));

                    if (po.side == Side::Bid) {
                        // Bid fill: we BOUGHT base asset (pc.base_asset_id) and SOLD quote asset (pc.quote_asset_id).
                        state_->record_buy(pc.base_asset_id, fill.size, fill.price);
                        // [GUARD-FIX 2026-07-30] The prior condition
                        // (quote != "xch" || base != "xch") was a tautology
                        // (no pair has xch on both legs), so the intended
                        // XCH-leg exclusion never applied.  Guard per-leg.
                        if (pc.quote_asset_id != "xch") {
                            bool sold_quote = state_->record_sell(pc.quote_asset_id, quote_mojos);
                            if (!sold_quote) {
                                logger_->warn(
                                    "detect_fills: record_sell failed for quote asset {} "
                                    "(size={}) representing spent quote",
                                    pc.quote_asset_id, quote_mojos);
                            }
                        }
                    } else {
                        // Ask fill: we SOLD base asset (pc.base_asset_id) and BOUGHT quote asset (pc.quote_asset_id).
                        bool sold = state_->record_sell(pc.base_asset_id, fill.size);
                        if (!sold) {
                            logger_->error(
                                "record_sell failed for {} (asset={}) "
                                "-- insufficient balance; fill still "
                                "recorded, inventory needs reconciliation",
                                trade_id.substr(0, 12), pc.base_asset_id);
                        }
                        // [GUARD-FIX 2026-07-30] Same tautology fix as the
                        // bid branch: guard the quote leg only.
                        if (pc.quote_asset_id != "xch") {
                            state_->record_buy(pc.quote_asset_id, quote_mojos, Mojo{1});
                        }
                    }
                }

                logger_->info("FILL {} {} {} mojos @ {} mojos [{}]",
                              (po.side == Side::Bid) ? "BID" : "ASK",
                              po.pair_name, fill.size, fill.price,
                              trade_id.substr(0, 12));

                // Remove from pending offers.
                // [T5-02] The fill is emitted regardless of remove_offer
                // result.  If the offer was already removed (e.g. by
                // reconciliation), we still record the fill event.
                if (!state_->remove_offer(trade_id)) {
                    logger_->warn("detect_fills: offer {} already removed "
                                  "from state", trade_id.substr(0, 12));
                }
                fills.push_back(std::move(fill));

            } else if (status == trade_status::kCancelled ||
                       status == trade_status::kFailed) {
                // Offer was cancelled or failed -- remove from tracking.
                // [T5-02] Defensive: log if already removed.
                if (!state_->remove_offer(trade_id)) {
                    logger_->debug("Offer {} already removed from state "
                                   "(status={})", trade_id.substr(0, 12),
                                   status);
                } else {
                    logger_->info("Offer {} removed (status={})",
                                  trade_id.substr(0, 12), status);
                }

                // [S25 2026-08-24] Report it, so the outcome is PERSISTED.
                //
                // Removing from State ends the tracking but writes nothing
                // down, and this class holds no database handle by design
                // -- the engine persists offer outcomes.  Recording the id
                // here lets it write "cancelled" exactly as it writes
                // "filled" from the returned fills.  Reported even when the
                // state entry was already gone: the DB row can still be
                // stale from an earlier process that saw the same terminal
                // status and dropped it, which is how 48 rows reached 17
                // days old.
                last_terminal_offers_.push_back(trade_id);
            }
            // Status PENDING_ACCEPT / PENDING_CONFIRM: still alive, no action.
    }

    if (!fills.empty()) {
        logger_->info("detect_fills: {} new fills detected", fills.size());
    }
    if (!last_terminal_offers_.empty()) {
        logger_->info("detect_fills: {} offer(s) observed terminal",
                      last_terminal_offers_.size());
    }
    if (!last_dead_offers_.empty()) {
        logger_->error("detect_fills: {} offer(s) the wallet reports CONFIRMED "
                       "were proven never taken -- none booked as a fill",
                       last_dead_offers_.size());
    }

    co_return fills;
}

// ---------------------------------------------------------------------------
// [FILL-PROOF 2026-09-23] The chain's word on a CONFIRMED trade
// ---------------------------------------------------------------------------
void OfferManager::set_fill_proof_node(std::shared_ptr<rpc::ChiaFullNodeRPC> node,
                                       std::function<bool()>                node_trusted)
{
    fill_proof_node_         = std::move(node);
    fill_proof_node_trusted_ = std::move(node_trusted);
}

asio::awaitable<FillProofResult>
OfferManager::prove_fill_on_chain(const json& trade_record, std::string& failure,
                                  ProofLookup& lookup, bool& asked_node)
{
    // The escalation's adapter shape: any throw is "cannot hash", which voids
    // the whole name list, which proves nothing.
    const auto name_of = [](const CoinRef& ref) -> std::string {
        try {
            return CoinManager::compute_coin_name(ref.parent_hex, ref.puzzle_hash_hex,
                                                  static_cast<Mojo>(ref.amount));
        } catch (const std::exception&) {
            return {};
        }
    };
    const std::vector<std::string> names =
        coin_names_for(parse_coins_of_interest(trade_record), name_of);
    if (names.empty()) {
        lookup  = ProofLookup::NothingToAsk;   // [review #172]
        failure = "the wallet record carries no readable coins_of_interest";
        co_return FillProofResult{};
    }

    const bool ask_node = fill_proof_node_ && fill_proof_node_trusted_
                          && fill_proof_node_trusted_();
    asked_node = ask_node;
    std::vector<json> records;
    try {
        if (ask_node) {
            records = co_await fill_proof_node_->get_coin_records_by_names(
                names, /*include_spent=*/true);
        } else {
            // Refuses until synced -- the window the wallet mislabels in.
            records = co_await wallet_->get_coin_records_by_names(names);
        }
    } catch (const rpc::ChiaRPCApplicationError& e) {
        // [review #171 round 15] An answer: this request refused, as the
        // wallet refuses one naming a coin it does not hold.  This offer's
        // alone -- the call's other offers are still asked.  [round 16] Only
        // that refusal: a wallet not synced or not connected refuses every
        // lookup the same way, so that one ends the call's lookups.
        lookup = coin_lookup_refusal_is_offer_local(e.what()) ? ProofLookup::Refused
                                                              : ProofLookup::Failed;
        failure = std::string{"the "} + (ask_node ? "full node" : "wallet")
                  + " refused to list its maker coins: " + e.what();
        co_return FillProofResult{};
    } catch (const std::exception& e) {
        lookup = ProofLookup::Failed;
        failure = std::string{"the "} + (ask_node ? "full node" : "wallet")
                  + " could not list its maker coins: " + e.what();
        co_return FillProofResult{};
    }
    const FillProofResult coins = prove_fill(names, records, name_of);
    if (coins.verdict == FillProof::Unknown) {
        failure = std::string{"the "} + (ask_node ? "full node" : "wallet")
                  + "'s answer did not cover every maker coin, or a record was "
                    "unreadable or contradictory";
    }
    if (coins.verdict != FillProof::SpentTogether) {
        co_return coins;
    }

    // [review #171] Every maker coin was spent in one block.  A take does
    // that, and so does a cancel or a stray spend of a one-coin offer, so
    // only the take's own mark in that block books a fill: the settlement
    // coin, from the node; the payment of what we asked for, from the wallet.
    //
    // [review #171, round 5] The settlement coin is known by its puzzle as
    // well as its amount: each offered asset's settlement puzzle, named here.
    const auto puzzle_of = [](const std::string& asset) -> std::string {
        try {
            return CoinManager::settlement_puzzle_hash(asset).value_or(std::string{});
        } catch (const std::exception&) {
            return {};
        }
    };
    const std::vector<SettlementCoin> settlements =
        ask_node ? offered_settlements(trade_record, puzzle_of) : std::vector<SettlementCoin>{};
    const std::vector<std::uint64_t> amounts =
        ask_node ? std::vector<std::uint64_t>{} : summary_amounts(trade_record, "requested");
    if (ask_node ? settlements.empty() : amounts.empty()) {
        lookup  = ProofLookup::NothingToAsk;   // [review #172]
        failure = std::string{"every maker coin was spent at block "}
                  + std::to_string(coins.height) + ", but the trade record lists no "
                  + (ask_node ? "offered asset whose settlement coin can be named"
                              : "requested amounts to look for");
        co_return FillProofResult{};
    }
    FillProofResult proof;
    try {
        if (ask_node) {
            const std::vector<json> children =
                co_await fill_proof_node_->get_coin_records_by_parent_ids(
                    names, /*include_spent=*/true);
            proof = prove_take_from_children(coins, names, children, settlements);
        } else {
            const std::vector<json> payments =
                co_await wallet_->get_coin_records_at_height(coins.height, amounts);
            proof = prove_take_from_payments(coins, names, payments, amounts);
        }
    } catch (const rpc::ChiaRPCApplicationError& e) {
        // [review #171 round 15] Refused, as at the first stage.
        lookup = coin_lookup_refusal_is_offer_local(e.what()) ? ProofLookup::Refused
                                                              : ProofLookup::Failed;
        failure = std::string{"every maker coin was spent at block "}
                  + std::to_string(coins.height) + ", but the "
                  + (ask_node ? "full node refused to list their children"
                              : "wallet refused to list the payments at that height")
                  + ": " + e.what();
        co_return FillProofResult{};
    } catch (const std::exception& e) {
        lookup = ProofLookup::Failed;
        failure = std::string{"every maker coin was spent at block "}
                  + std::to_string(coins.height) + ", but the "
                  + (ask_node ? "full node could not list their children"
                              : "wallet could not list the payments at that height")
                  + ": " + e.what();
        co_return FillProofResult{};
    }
    if (proof.verdict == FillProof::Unknown) {
        failure = std::string{"every maker coin was spent at block "}
                  + std::to_string(coins.height)
                  + (ask_node ? ", and the node's list of their children could not be read"
                              : ", and the wallet shows no payment of a requested "
                                "amount in that block -- which proves nothing either way");
    }
    co_return proof;
}

void OfferManager::handle_unproven_fill(const std::string& trade_id,
                                        const PendingOffer& po,
                                        const FillProofResult& proof,
                                        const std::string& failure,
                                        bool from_node,
                                        std::uint64_t claimed_height,
                                        BlockHeight current_block)
{
    if (dead_offer_closable(proof, current_block,
                            strategy_cfg_.confirmation_depth_blocks)) {
        // Never taken, and the spend that killed it is as deep as a fill must
        // be: stop tracking it, and report it -- never as a fill.  The engine
        // records the outcome; proven_dead_ keeps recheck_terminal() from
        // re-adopting what the wallet will go on calling CONFIRMED.
        state_->remove_offer(trade_id);
        proven_dead_.insert(trade_id);
        fill_proof_deferrals_.erase(trade_id);
        last_dead_offers_.push_back(DeadOffer{trade_id, po.pair_name, proof.height,
                                              proof.coins, proof.unspent,
                                              proof.spent_together});
        if (proof.spent_together) {
            logger_->error("[FILL-PROOF] {} ({} {}): the wallet reports CONFIRMED, "
                           "but block {} -- where all {} maker coins were spent -- "
                           "holds no settlement coin for it: a cancel or another "
                           "spend consumed them, not a take; NOT booked as a fill",
                           trade_id.substr(0, 12), po.pair_name, to_string(po.side),
                           proof.height, proof.coins);
        } else {
            logger_->error("[FILL-PROOF] {} ({} {}): the wallet reports CONFIRMED, "
                           "but its {} maker coins were not spent together ({} still "
                           "unspent, the first spent at block {}) -- the offer died "
                           "without being taken; NOT booked as a fill",
                           trade_id.substr(0, 12), po.pair_name, to_string(po.side),
                           proof.coins, proof.unspent, proof.height);
        }
        return;
    }

    // Not booked, still tracked: the next heartbeat asks again.  The latest
    // proof is kept for cancel_offer_charged() (review round 2).  Logged on
    // the first deferral and every 20th after it -- a CONFIRMED offer is
    // polled every heartbeat, so an unthrottled line would repeat each one.
    FillProofDeferral& deferral = fill_proof_deferrals_[trade_id];
    const std::uint32_t deferrals = ++deferral.count;
    deferral.verdict        = proof.verdict;
    deferral.from_node      = from_node;
    deferral.claimed_height = claimed_height;
    deferral.proved_block   = current_block;
    deferral.proof_call     = fill_poll_heartbeat_;
    if (deferrals != 1U && deferrals % 20U != 0U) {
        return;
    }
    std::string why = failure.empty() ? std::string{"no usable answer"} : failure;
    if (proof.verdict == FillProof::Live) {
        why = !cancel_withheld_for_proof(trade_id)
                  ? "every maker coin is still unspent, "
                    + std::to_string(strategy_cfg_.confirmation_depth_blocks)
                    + "+ blocks past the height the wallet claims -- the offer is "
                      "live again and may be cancelled"
                  : std::string{"every maker coin is still unspent -- a node that "
                                "has not yet seen the take, or an offer live "
                                "again; its cancels are withheld until the node "
                                "is at confirmation depth past the wallet's claim"};
    } else if (proof.verdict == FillProof::Dead) {
        why = proof.spent_together
                  ? "its maker coins were spent in one block that holds no "
                    "settlement coin for it (the offer died), but that block is "
                    "not yet at confirmation depth"
                  : "its maker coins were not spent together (the offer died), but "
                    "the first spend is not yet at confirmation depth";
    }
    // [round 11] "reported": an offer re-proved under a cancel's status is
    // deferred here too, and the wallet no longer reports it CONFIRMED.
    logger_->warn("[FILL-PROOF] {} ({}): the wallet reported CONFIRMED, the "
                  "chain says {}: {} -- not booked; asked again next heartbeat "
                  "(deferral {})", trade_id.substr(0, 12), po.pair_name,
                  fill_proof_name(proof.verdict), why, deferrals);
}

bool OfferManager::cancel_withheld_for_proof(const std::string& trade_id) const
{
    const auto held = fill_proof_deferrals_.find(trade_id);
    if (held == fill_proof_deferrals_.end()) {
        return false;   // not under proof
    }
    const FillProofDeferral& latest = held->second;
    return !live_offer_cancellable(latest.verdict, latest.from_node, latest.claimed_height,
                                   latest.proved_block, latest.proof_call, fill_poll_heartbeat_,
                                   strategy_cfg_.confirmation_depth_blocks);
}

// ---------------------------------------------------------------------------
// [S25 2026-08-24] recheck_terminal
// ---------------------------------------------------------------------------
asio::awaitable<TerminalRecheck>
OfferManager::recheck_terminal(const std::string& trade_id,
                               BlockHeight        current_block,
                               bool*              wallet_cancelled_out)
{
    if (wallet_cancelled_out != nullptr) {
        *wallet_cancelled_out = false;
    }
    json rec;
    try {
        rec = co_await wallet_->get_offer(trade_id, /*file_contents=*/false);
    } catch (const std::exception& e) {
        // Unreachable wallet is not evidence either way.  Say so and let
        // the caller keep the entry buffered.
        logger_->warn("[S25] recheck_terminal: get_offer failed for {} -- "
                      "no verdict, caller retries: {}",
                      trade_id.substr(0, 12), e.what());
        co_return TerminalRecheck::NoVerdict;
    }

    if (!rec.contains("status")) {
        logger_->warn("[S25] recheck_terminal: wallet record for {} carries "
                      "no status field -- no verdict",
                      trade_id.substr(0, 12));
        co_return TerminalRecheck::NoVerdict;
    }

    const int status = trade_status::parse(rec["status"]);

    if (status == trade_status::kCancelled || status == trade_status::kFailed) {
        // [review #163] Both are terminal, but only CANCELLED says a cancel
        // spend confirmed.
        if (wallet_cancelled_out != nullptr) {
            *wallet_cancelled_out = (status == trade_status::kCancelled);
        }
        co_return TerminalRecheck::StillTerminal;
    }

    // Anything below means the offer is NOT over, and the offer is no
    // longer in State -- detect_fills() calls remove_offer() the moment it
    // sees a terminal status.  Declining to write the cancellation is
    // therefore only half an answer: nothing else puts the offer back.
    // reconcile_offers() adopts untracked records only when they are
    // PENDING_ACCEPT, and it does not run at all when reconciliation is
    // disabled or Step 8 is gated, so PENDING_CONFIRM and PENDING_CANCEL
    // would never be restored and a later fill would be missed.  Re-adopt
    // here, for every live status.
    const auto readopt = [&](const char* why) {
        // Never overwrite a live entry.  Something else may have taken the
        // offer back under management while this observation sat buffered
        // -- reconciliation's reverse adoption, or a restart -- and that
        // entry carries state this record does not: cancel_pending, tier,
        // the original post price.  Clobbering it would make cancel_stale
        // pay a second cancellation fee on an offer it has already
        // cancelled.
        if (!state_->get_offer(trade_id).offer_id.empty()) {
            // [S14] debug: the S46 intent sweep re-asks every heartbeat, and
            // this line was 3,065 of the last 60,000 in engine.log.
            logger_->debug("[S25] recheck_terminal: {} reports {} and is "
                           "already tracked in State -- leaving the live "
                           "entry alone",
                           trade_id.substr(0, 12), why);
            return true;
        }
        auto parsed = try_parse_wallet_offer(rec, current_block);
        if (parsed) {
            state_->upsert_offer(*parsed);
            logger_->warn("[S25] recheck_terminal: {} was observed terminal "
                          "but the wallet now reports {} -- re-adopted into "
                          "State ({} {} on {})",
                          trade_id.substr(0, 12), why,
                          (parsed->side == Side::Bid) ? "BID" : "ASK",
                          parsed->size, parsed->pair_name);
            return true;
        }
        // Unparseable record.  Adopt with minimal metadata anyway, the
        // same fallback reconcile_offers uses: an untracked live offer
        // holds its coins locked invisibly, and cancel_stale or UTXO
        // liberation can still free them once the id is tracked.
        PendingOffer po;
        po.offer_id         = trade_id;
        po.pair_name        = "UNKNOWN";
        po.side             = Side::Bid;
        po.price            = 0;
        po.size             = 0;
        po.tier             = 0;
        po.fee_mojos        = 0;
        po.created_at_block = 0;
        state_->upsert_offer(po);
        logger_->warn("[S25] recheck_terminal: {} reports {} but its wallet "
                      "record could not be parsed -- adopted with minimal "
                      "metadata to prevent a coin-lock deadlock",
                      trade_id.substr(0, 12), why);
        return false;
    };

    if (status == trade_status::kConfirmed) {
        // [FILL-PROOF 2026-09-23] Not for an offer the chain proved dead: the
        // wallet goes on reporting it CONFIRMED, and re-adopting it would send
        // it round detect_fills again on every recheck.
        if (proven_dead_.count(trade_id) > 0) {
            co_return TerminalRecheck::StillTerminal;
        }
        // The terminal observation was reorged into a FILL.  Re-adoption
        // is what makes the fill recordable: detect_fills() only inspects
        // offers still in State, so without this the fill would be
        // recorded nowhere -- an accounting loss, not a reporting one.
        if (!readopt("CONFIRMED")) {
            // Minimal metadata cannot carry a fill: pair, side, price and
            // size are all unknown, so detect_fills has nothing to record
            // against.  Loud, because no later pass will find it.
            logger_->error("[S25] recheck_terminal: {} is CONFIRMED but "
                           "unparseable -- the FILL cannot be reconstructed "
                           "and will not be recorded; reconcile inventory "
                           "for this trade by hand",
                           trade_id.substr(0, 12));
        }
        co_return TerminalRecheck::Confirmed;
    }

    if (status == trade_status::kPendingAccept
        || status == trade_status::kPendingConfirm
        || status == trade_status::kPendingCancel) {
        readopt("a pending status");
        if (status == trade_status::kPendingCancel) {
            // The wallet is already cancelling this offer.  A re-adopted
            // entry defaults to cancel_pending=false, and Step 8 would
            // read that as "cancellable" and submit a SECOND secure
            // cancellation, paying a second fee for a cancellation
            // already in flight.  Applied after readopt so it covers the
            // parsed entry, the minimal-metadata fallback, and the
            // already-tracked case alike -- mark_cancel_pending only sets
            // the flag on whatever entry is in State.
            state_->mark_cancel_pending(trade_id);
        }
        co_return TerminalRecheck::Revived;
    }

    // Unrecognised code (trade_status::parse yields -1 for anything it does
    // not know).  This is NOT evidence the offer is live, and treating it
    // as such would discard a one-way cancellation on the strength of a
    // string nobody has seen before.  No verdict: keep it buffered.
    logger_->warn("[S25] recheck_terminal: wallet reports unrecognised "
                  "status {} for {} -- no verdict, caller retries",
                  status, trade_id.substr(0, 12));
    co_return TerminalRecheck::NoVerdict;
}

// ---------------------------------------------------------------------------
// cancel_stale -- cancel offers exceeding their block-based TTL
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>> OfferManager::cancel_stale(
    const std::string& pair_name,
    BlockHeight        current_block,
    BlockHeight        ttl_blocks,
    bool               spare_expiring)
{
    auto all_offers = state_->get_all_offers();
    std::vector<std::string> cancelled_ids;

    const bool expire_mode =
        strategy_cfg_.ttl_cancel_mode == TtlCancelMode::Expire;

    for (const auto& po : all_offers) {
        // Filter by pair name.
        if (po.pair_name != pair_name) {
            continue;
        }

        // [S70] The stuck pass retries a hard-TTL cancel that failed.  An
        // offer left to its on-chain expiry never had one, so it is not
        // "stuck" for being old; retire_expired_offers owns it.
        if (spare_expiring
            && !age_limit_cancel_applies(expire_mode, po.expiry_max_time)) {
            continue;
        }

        // [S14] One predicate for the forced-cancel set, shared with the
        // Step 8 stuck counter and the STOPDRAIN eligibility count.  Offers
        // already awaiting cancel confirmation belong to the escalation, and
        // an offer is due at created + ttl (written without the overflow).
        if (!is_forced_cancel_candidate(po.cancel_pending, po.created_at_block,
                                        current_block, ttl_blocks)) {
            continue;
        }

        // Cancel via wallet RPC (secure = true to spend the locked coin).
        bool cancel_ok = false;
        bool needs_emergency = false;
        try {
            co_await cancel_offer_charged(
                po.offer_id, cancel_fee_for(po.offer_id), /*secure=*/true);
            cancel_ok = true;
        } catch (const rpc::ChiaRPCError& e) {
            const std::string_view msg{e.what()};
            needs_emergency =
                msg.find("insufficient funds") != std::string_view::npos ||
                msg.find("spendable balance") != std::string_view::npos;
            if (!needs_emergency) {
                logger_->error("Failed to cancel offer {}: {}",
                               po.offer_id.substr(0, 12), e.what());
            }
        }
        if (needs_emergency) {
            cancel_ok = co_await emergency_cancel(
                po.offer_id, "cancel_stale");
        }
        if (cancel_ok) {
            state_->mark_cancel_pending(po.offer_id);
            cancelled_ids.push_back(po.offer_id);

            logger_->info("Cancelled stale offer {} ({} tier {}, age {} blocks)",
                          po.offer_id.substr(0, 12),
                          pair_name, po.tier,
                          current_block - po.created_at_block);
        }
    }

    if (!cancelled_ids.empty()) {
        logger_->info("cancel_stale({}): {} offers cancelled", pair_name,
                      cancelled_ids.size());
    }

    co_return cancelled_ids;
}

// ---------------------------------------------------------------------------
// cancel_all -- shutdown: cancel every pending offer
// ---------------------------------------------------------------------------

asio::awaitable<OfferManager::CancelOutcome> OfferManager::cancel_all(
    std::chrono::steady_clock::time_point deadline)
{
    logger_->info("cancel_all: initiating bulk cancellation");

    CancelOutcome out;

    auto all_offers = state_->get_all_offers();
    // [S33 2026-09-12] NO EARLY RETURN ON AN EMPTY LOCAL BOOK.
    //
    // This used to co_return here whenever state_->get_all_offers() was empty,
    // which meant the sweep never went out for exactly the book it exists to
    // clear: cancel_all:true cancels every pending offer IN THE WALLET, and a
    // book left resting by a previous instance is in the wallet and in nobody's
    // memory.  LOCAL EMPTINESS IS NOT WALLET EMPTINESS.
    //
    // The reservation cannot come from that list either -- offers we never
    // tracked are extra BATCHES and batch_fee is charged per batch -- so an
    // empty list is sized from the fixed conservative bound below instead.
    const auto tracked_n = static_cast<std::int64_t>(all_offers.size());

    // [S33 2026-09-12] THE FLOOR FOR A BOOK WE CANNOT SEE.
    //
    // The sweep goes out even with an empty local book -- that is the whole
    // point of the removed early return above -- and cancel_all:true then
    // cancels offers this process never tracked.  batch_fee is charged PER
    // BATCH, so those offers are extra BATCHES, and reserve_bulk_cancel
    // models each batch as a WHOLE fee coin leaving the cycle ledger.
    // Reserving the tracked count of ZERO there is the 2026-08-23
    // zero-spendable shape: real coin locks the ledger does not know about,
    // and every later try_lock admitting against coins already spent.
    //
    // 25 offers is HEADROOM over the largest book this deployment's ladder
    // can rest, not a derivation from it: the live config runs num_tiers 6
    // with one enabled pair, so a full ladder is 1 x 6 x 2 = 12 offers, and
    // 25 leaves room for a second pair being enabled without revisiting this
    // number.  A previous instance cannot have left more than one full ladder
    // resting unless cancel_stale and the on-chain reconciler had BOTH failed
    // as well.
    //
    // [BULKCANCEL-B 2026-09-13] At rpc::kCancelOffersSingleBatchSize (50)
    // the floor is ONE batch and so ONE whole coin; at the old batch size of
    // 5 it was five of each.  The live wallet held 22 open offers of its own
    // on 2026-09-13, inside the floor.
    //
    // [S33 2026-09-12] Deliberately NOT computed from config at compile time.
    // An earlier draft of this comment derived 24 from config.example.yaml's
    // 3 pairs x 4 tiers, which is not what the bot runs -- a derivation that
    // silently drifts with a file nobody edits is worse than a stated floor
    // with its headroom written down.  If the enabled-pair count grows past
    // two full ladders, raise this and re-read the availability argument
    // below, which is what actually caps it.
    //
    // WHY NOT HIGHER.  cancel_all also runs from the operator Cancel All
    // flag while the bot is STILL TRADING (engine.cpp check_cancel_all_flag),
    // and every reserved batch drains a whole coin from the live cycle
    // ledger.  At the old batch size of 5, 50 offers was 10 coins -- more
    // than the incident wallet HELD (14.59 XCH in ~2-XCH coins), and
    // note_lock with a need it cannot cover CLEARS the pool outright, so
    // try_lock refuses every offer for the rest of the cycle.  That cost is
    // bounded, since begin_xch_lock_cycle rebuilds the ledger each ~1-minute
    // cycle, but it is paid on EVERY empty-book cancel_all and it is paid
    // while quoting.  [BULKCANCEL-B 2026-09-13] At the single-batch size any
    // floor up to kCancelOffersSingleBatchSize offers drains ONE coin, so this
    // argument binds again only if the batch size is lowered.
    //
    // WHY NOT LOWER.  One batch is what this code did before: at the old batch
    // size of 5 it under-modelled by exactly the untracked offers the sweep
    // exists for.  [BULKCANCEL-B 2026-09-13] At the single-batch size the
    // floor already IS one batch; it matters again only past
    // kCancelOffersSingleBatchSize offers or if the batch size is lowered.
    //
    // WHY NOT AN RPC.  A wallet-wide count was tried and cut.  It asked
    // get_all_offers with the wallet-default sort_key, under which PENDING
    // offers sort LAST, so a single 50-row page would usually have counted
    // ZERO pending offers and reported a COMPLETE scan -- collapsing this
    // reservation to the >= 1 clamp, which is worse than the fixed bound it
    // replaced.  Both other wallet scanners in this repo pass "RELEVANCE"
    // for that exact reason (reconcile_offers here, and
    // on_chain_reconciler.cpp).  A bound that is right by construction beats
    // a number computed wrong at ~1.6-2 s per page on a shutdown path.
    constexpr std::int64_t kUnknownWalletBookBound = 25;

    // [S33 2026-09-12] A FLOOR FOR EVERY BOOK SHAPE, NOT ONLY THE EMPTY ONE.
    // cancel_all:true sweeps the whole WALLET book whether or not we track
    // any of it, so untracked offers add BATCHES to a mixed book exactly as
    // they do to an empty one. Selecting tracked_n alone reserved 1 batch for
    // 1 tracked + 12 untracked while the daemon charged 3 at the old batch
    // size of 5. max() keeps the bound the FLOOR that reserve_bulk_cancel's
    // contract already says it is.
    const std::int64_t reserve_n =
        std::max(tracked_n, kUnknownWalletBookBound);

    logger_->info("cancel_all: {} tracked offer(s) -- reserving fees for {} "
                  "offer(s) ({})", tracked_n, reserve_n,
                  reserve_n == tracked_n
                      ? "the tracked book"
                      : "the conservative bound for offers cancel_all also "
                        "sweeps but this process never tracked");

    // Attempt bulk cancellation first (wallet cancel_offers endpoint).
    bool bulk_ok = false;
    // [review 2026-09-13, round 2] The sweep failed AFTER it may have reached
    // the wallet (rpc::cancel_possibly_submitted).
    bool bulk_possibly_submitted = false;
    std::string bulk_err;
    try {
        co_await cancel_offers_charged(
            current_fee_mojos_, /*secure=*/true, reserve_n);
        bulk_ok = true;
    } catch (const rpc::ChiaRPCTransportError& e) {
        // BEFORE the base-class handler, which would take this too.  A
        // transport failure is not one thing: a connect failure never reached
        // the wallet, while a timeout or a 5xx may have -- and so did a 2xx
        // reply whose body rpc_post could not parse [review 2026-09-13,
        // round 3], which it reports as a transport failure with that status.
        bulk_err = e.what();
        bulk_possibly_submitted =
            rpc::cancel_possibly_submitted(e.curl_code(), e.http_code());
    } catch (const rpc::ChiaRPCError& e) {
        bulk_err = e.what();
    }

    if (bulk_ok) {
        // Bulk success: one RPC accepted, so every id is SUBMITTED. Not
        // confirmed -- a secure cancel spends the offer coins on-chain and
        // the book is not empty until those spends land. `bulk_submitted`
        // carries that qualification to the caller instead of leaving the
        // two shutdown paths in engine.cpp to disagree about it in prose.
        logger_->info("cancel_all: bulk cancel_offers succeeded");
        out.bulk_submitted = true;
        out.cancelled.reserve(all_offers.size());
        // [FILL-PROOF, review #171 round 3] ...except for an offer the fill
        // proof holds.  The sweep skips every trade the wallet calls completed
        // -- CONFIRMED included -- so it did not cancel that one, and
        // reporting it cancelled would mark it cancel_pending: shutdown would
        // claim success over a quote that may still be takeable, and every
        // later per-offer path would skip it.  It goes through the guarded
        // per-offer path instead: cancelled there if the node has proven it
        // live again, reported outstanding while its cancel stays withheld.
        //
        // [review #171 round 16] ...and except for an offer State already
        // marks cancel_pending.  The wallet may call it CANCELLED -- a row
        // boot restored, an offer reconcile_offers or detect_fills keeps
        // tracked -- which the sweep skips as it skips CONFIRMED, or its own
        // cancel may be in flight.  Either way this call did not cancel it,
        // so it is read again after the sweep with the held ones, and
        // reported as the wallet then says.
        std::vector<std::string> reread;
        for (const auto& po : all_offers) {
            if (fill_proof_deferrals_.count(po.offer_id) > 0U || po.cancel_pending) {
                reread.push_back(po.offer_id);
            } else {
                out.cancelled.push_back(po.offer_id);
            }
        }
        if (!reread.empty()) {
            // [review #171 round 10] RE-READ, AFTER THE SWEEP, WHAT IT DID TO
            // EACH HELD OFFER.  A hold records the status of the LAST POLL; the
            // sweep acted on the status each trade had when it ran.  In chia
            // 2.7.4 it cancels every PENDING_ACCEPT, PENDING_CONFIRM and
            // PENDING_CANCEL trade and marks each PENDING_CANCEL before it
            // returns -- and also every trade not yet CANCELLED that shares a
            // cancellation coin with one, a held CONFIRMED trade included
            // (trade_manager.cancel_pending_offers, get_trades_by_coin).  So the
            // sweep may already cover a held offer, and sending it down the
            // per-offer path as well would cancel it a second time or report it
            // outstanding.  Only an offer the wallet still reports CONFIRMED,
            // which the sweep skipped, is left to the guard:
            //   - CONFIRMED: the guarded per-offer path decides, as before;
            //   - PENDING_CANCEL: a cancel is in flight, this sweep's or an
            //     earlier one's -- nothing more is sent;
            //   - FAILED: nothing is left to cancel;
            //   - [round 18] CANCELLED: closed only once the chain shows it dead
            //     or taken.  A local cancel leaves every maker coin unspent and
            //     the offer takeable, and the sweep skips it as it skips every
            //     completed trade, so one the chain shows Live, or cannot
            //     prove, is reported outstanding, with nothing sent;
            //   - PENDING_ACCEPT or PENDING_CONFIRM: live, and not in the sweep,
            //     which would have left it PENDING_CANCEL -- cancelled here;
            //   - no answer, or no status the wallet really reports: the sweep
            //     may or may not have cancelled it, so nothing is sent and it
            //     is reported outstanding.
            // [round 11] Only a cancel this call's RPCs had accepted is reported
            // `cancelled`: its callers persist those as submitted with their
            // own cause.  A cancel in flight is `already_pending`, and a closed
            // offer is `closed`, which no caller persists or retries.  And a
            // status a cancel writes keeps the hold: it may hide a take, and
            // detect_fills proves that on-chain before letting it go.  Any
            // other status the wallet really reports releases it.
            std::vector<std::string> to_cancel;
            std::vector<std::string> unread;
            std::vector<std::string> takeable;   // [round 18] CANCELLED, not proven closed
            std::string takeable_why;
            bool proof_lookup_failed = false;
            std::size_t in_flight = 0;
            std::size_t closed = 0;
            std::string read_error;
            for (std::size_t i = 0; i < reread.size(); ++i) {
                const auto& oid = reread[i];
                if (std::chrono::steady_clock::now() >= deadline) {
                    for (std::size_t j = i; j < reread.size(); ++j) {
                        unread.push_back(reread[j]);
                    }
                    out.deadline_hit = true;
                    break;
                }
                int status = -1;
                json rec;
                try {
                    rec = co_await wallet_->get_offer(oid, /*file_contents=*/false);
                    if (const auto st = rec.find("status"); st != rec.end()) {
                        status = trade_status::parse(*st);
                    }
                } catch (const std::exception& e) {
                    read_error = e.what();
                }
                if (status == trade_status::kConfirmed) {
                    to_cancel.push_back(oid);
                } else if (status == trade_status::kPendingCancel) {
                    out.already_pending.push_back(oid);
                    ++in_flight;
                } else if (status == trade_status::kCancelled) {
                    // [review #171 round 18] Proven before it is called closed.
                    std::string proof_failure;
                    ProofLookup lookup = ProofLookup::Answered;
                    bool asked_node = false;
                    FillProofResult proof;
                    if (proof_lookup_failed) {
                        proof_failure = "an earlier coin lookup in this sweep failed";
                    } else {
                        proof = co_await prove_fill_on_chain(rec, proof_failure, lookup,
                                                             asked_node);
                        if (lookup == ProofLookup::Failed) {
                            proof_lookup_failed = true;
                        }
                    }
                    // [review #171 round 19] Dead only at confirmation depth, as
                    // everywhere else: a shallower spend could be reorganised out.
                    if (proof.verdict == FillProof::Settled
                        || dead_offer_closable(proof, latest_fill_poll_block_,
                                               strategy_cfg_.confirmation_depth_blocks)) {
                        local_cancel_live_.erase(oid);
                        out.closed.push_back(oid);
                        ++closed;
                    } else {
                        if (proof.verdict == FillProof::Live) {
                            local_cancel_live_.insert(oid);
                            takeable_why = "the wallet cancelled it only locally: every "
                                           "maker coin is unspent, so it can still be taken";
                        } else if (proof.verdict == FillProof::Dead) {
                            // [round 19] Remembered, so every retry keeps it
                            // outstanding until the spend is deep enough.
                            local_cancel_live_.insert(oid);
                            takeable_why = "dead at block " + std::to_string(proof.height)
                                + ", not yet "
                                + std::to_string(strategy_cfg_.confirmation_depth_blocks)
                                + " blocks deep: a reorganisation could still undo the spend";
                        } else {
                            takeable_why = "the wallet reports it CANCELLED, but the chain "
                                           "did not show it dead or taken: " + proof_failure;
                        }
                        takeable.push_back(oid);
                    }
                } else if (status == trade_status::kFailed) {
                    fill_proof_deferrals_.erase(oid);
                    out.closed.push_back(oid);
                    ++closed;
                } else if (trade_status::is_known(status)) {
                    fill_proof_deferrals_.erase(oid);
                    to_cancel.push_back(oid);
                } else {
                    unread.push_back(oid);
                }
            }
            CancelOutcome per_offer = co_await cancel_ids(to_cancel, deadline);
            out.cancelled.insert(out.cancelled.end(), per_offer.cancelled.begin(),
                                 per_offer.cancelled.end());
            out.failed.insert(out.failed.end(), per_offer.failed.begin(),
                              per_offer.failed.end());
            out.already_pending.insert(out.already_pending.end(),
                                       per_offer.already_pending.begin(),
                                       per_offer.already_pending.end());
            if (!per_offer.failed.empty()) {
                out.last_error  = per_offer.last_error;
                out.worst_class = per_offer.worst_class;
            }
            if (!unread.empty()) {
                out.failed.insert(out.failed.end(), unread.begin(), unread.end());
                // [review #171 round 18] Either kind of re-read offer.
                std::string why = read_error.empty()
                    ? std::string{"the status of an offer the fill proof holds, or "
                                  "one State had flagged cancel_pending, could not "
                                  "be read after the sweep"}
                    : read_error;
                const auto cls = execution::classify_take_failure(why);
                out.worst_class = per_offer.failed.empty()
                    ? cls
                    : execution::more_retryable(out.worst_class, cls);
                if (per_offer.failed.empty()) {
                    out.last_error = std::move(why);
                }
            }
            if (!takeable.empty()) {
                // [review #171 round 18] Outstanding: CANCELLED in the wallet,
                // and not proven closed on-chain.
                const bool first_failure = per_offer.failed.empty() && unread.empty();
                out.failed.insert(out.failed.end(), takeable.begin(), takeable.end());
                const auto cls = execution::classify_take_failure(takeable_why);
                out.worst_class = first_failure
                    ? cls
                    : execution::more_retryable(out.worst_class, cls);
                if (first_failure) {
                    out.last_error = takeable_why;
                }
            }
            out.deadline_hit = out.deadline_hit || per_offer.deadline_hit;
            logger_->warn("cancel_all: {} offer(s) the fill proof held or State "
                          "had cancel_pending, re-read "
                          "after the sweep: {} with a cancel already in flight "
                          "and {} closed, nothing sent for either; {} CANCELLED "
                          "but not proven closed on-chain, reported outstanding; {} still "
                          "CONFIRMED or live and not swept -- {} cancelled one "
                          "by one, {} still outstanding; {} unreadable, "
                          "reported outstanding with nothing sent",
                          reread.size(), in_flight, closed, takeable.size(), to_cancel.size(),
                          per_offer.cancelled.size(), per_offer.failed.size(),
                          unread.size());
        }
    } else if (bulk_possibly_submitted) {
        // [review 2026-09-13, round 2] NO ANSWER IS NOT A REFUSAL.  The sweep
        // may have run, may still be running inside the wallet, or may never
        // have arrived, and nothing here says which.  In chia 2.7.4
        // cancel_offers and cancel_offer both take the wallet state lock, and
        // cancel_pending_offers does not check trade status, so the per-offer
        // fallback further down would queue behind a sweep that is still
        // running and build a SECOND, conflicting spend of every offer it
        // already cancelled.  So send nothing more: every tracked id is
        // reported still live, and the caller waits at least one request
        // timeout and re-checks each offer before it cancels any
        // (CancelLadder::needs_recheck at shutdown, and the operator path in
        // Engine::check_cancel_all_flag).
        //
        // THE EMPTY LOCAL BOOK.  `failed` is then empty -- the bulk endpoint
        // names no offer id -- so bulk_possibly_submitted is the only field
        // that says anything went wrong, exactly as sweep_refused is for a
        // refusal, and all_cancelled() reads it.  It is NOT folded into
        // sweep_refused: a refusal is an answer, after which sending the
        // sweep again at once is safe.  After this, sending again at once is
        // the duplicate.
        logger_->warn("cancel_all: bulk cancel_offers got NO USABLE ANSWER ({}) -- "
                      "the wallet-wide sweep may still be running.  NOT "
                      "falling back to individual cancellation: nothing is "
                      "sent again until each of the {} tracked offer(s) is "
                      "re-checked", bulk_err, all_offers.size());
        out.bulk_possibly_submitted = true;
        out.last_error  = bulk_err;
        out.worst_class = execution::classify_take_failure(bulk_err);
        out.failed.reserve(all_offers.size());
        for (const auto& po : all_offers) {
            out.failed.push_back(po.offer_id);
        }
    } else if (all_offers.empty()) {
        // [S33 2026-09-12] There is no per-offer fallback to take: the ids the
        // sweep would have cancelled are the wallet's, not ours, and the bulk
        // endpoint takes no offer id at all.  REPORT the refusal rather than
        // returning a default-constructed outcome, which reads as "nothing to
        // do" -- that misreading is the whole defect being fixed here.
        logger_->error("cancel_all: wallet-wide sweep REFUSED ({}) and no "
                       "locally tracked ids exist to retry individually -- "
                       "anything resting in the wallet is STILL LIVE",
                       bulk_err);
        out.last_error  = bulk_err;
        out.worst_class = execution::classify_take_failure(bulk_err);
        // [S33 2026-09-12] THE FLAG IS THE WHOLE POINT OF THIS ARM. Setting
        // last_error and worst_class while leaving `failed` empty still read
        // as success at every consumer: CancelLadder::record() sets
        // outstanding_ empty and stop_reason Done, clean() returns true, the
        // shutdown logs "All outstanding offers cancelled", and the operator
        // Cancel All path skips its own error block. A refusal must not be
        // reported through fields that only failures happen to fill.
        out.sweep_refused = true;
    } else {
        // Bulk cancel failed -- fall back to individual cancellation.
        logger_->warn("Bulk cancel_offers failed: {} -- falling back to "
                      "individual cancellation", bulk_err);

        std::vector<std::string> ids;
        ids.reserve(all_offers.size());
        for (const auto& po : all_offers) ids.push_back(po.offer_id);

        // [S33 2026-09-12] ROUTED, NOT ASSIGNED.  cancel_ids returns a FRESH
        // CancelOutcome, so assigning it here DISCARDED the wallet-wide
        // sweep's refusal wholesale: a per-id fallback that then succeeded
        // reported `failed` empty and sweep_refused false, the ladder took
        // its Done branch, and shutdown logged "All outstanding offers
        // cancelled" after a sweep the wallet had REFUSED.  fold_refused_sweep
        // cannot be called without setting the flag, and it keeps the [S46]
        // carry of the refusal text.  Pinned by CancelOutcomeFoldTest.
        CancelOutcome fallback = co_await cancel_ids(ids, deadline);
        out = fold_refused_sweep(std::move(fallback), bulk_err);
    }

    // Mark offers whose cancellation succeeded as cancel_pending.
    // detect_fills will handle final removal when the wallet confirms.
    for (const auto& id : out.cancelled) {
        state_->mark_cancel_pending(id);
    }

    if (all_offers.empty()) {
        // [S33 2026-09-12] Say only what we know.  The sweep was accepted
        // wallet-wide, but the ids it touched were never ours, so a count of
        // "offers cancelled" here would be invented.  bulk_submitted carries
        // the acceptance; `cancelled` stays empty on purpose.
        // [review 2026-09-13, round 2] Three outcomes, not two: a sweep that
        // got no answer was not refused.
        logger_->info("cancel_all: no locally tracked offers -- wallet-wide "
                      "sweep {}; the ids it covered are not known to this "
                      "process",
                      bulk_ok                   ? "SUBMITTED"
                      : bulk_possibly_submitted ? "UNANSWERED (it may still "
                                                  "be running)"
                                                : "REFUSED");
    } else {
        logger_->info("cancel_all: {}/{} offers cancelled successfully",
                      out.cancelled.size(), all_offers.size());
    }
    co_return out;
}

// ---------------------------------------------------------------------------
// [S46] cancel_ids -- cancel exactly this set, and report what did not go.
//
// This is the retry leg. It exists because the wallet's bulk endpoint takes
// no offer id (chia_rpc.cpp:883-891), so "cancel the three that failed" is
// not expressible through it, and re-running cancel_all() would re-charge a
// fee against the four that already succeeded.
//
// The loop tries EVERY id -- a failure on one offer says nothing about the
// next -- and the ids that did not go are RETURNED rather than logged and
// dropped. The single exception is the wall-clock deadline, and even that
// returns the untried remainder as `failed` rather than losing it.
// ---------------------------------------------------------------------------

asio::awaitable<OfferManager::CancelOutcome> OfferManager::cancel_ids(
    const std::vector<std::string>& offer_ids,
    std::chrono::steady_clock::time_point deadline)
{
    CancelOutcome out;
    if (offer_ids.empty()) co_return out;

    out.cancelled.reserve(offer_ids.size());

    // The most-retryable class seen so far. Tracked with an explicit "have we
    // seen one" flag rather than seeded from the default, because the default
    // is Other and more_retryable(Other, Funding) is Other -- an all-funding
    // batch would then never report Funding and would never escalate.
    bool have_class = false;

    for (std::size_t i = 0; i < offer_ids.size(); ++i) {
        const auto& oid = offer_ids[i];

        if (std::chrono::steady_clock::now() >= deadline) {
            // Budget spent mid-loop. Everything untried is STILL LIVE and is
            // reported as such: "we ran out of time" is not "it is gone".
            for (std::size_t j = i; j < offer_ids.size(); ++j) {
                out.failed.push_back(offer_ids[j]);
            }
            out.deadline_hit = true;
            logger_->error("cancel_ids: wall-clock deadline reached with {} "
                           "of {} offer(s) never attempted -- returned as "
                           "STILL LIVE",
                           offer_ids.size() - i, offer_ids.size());
            break;
        }

        // An offer that left State between attempts settled, was already
        // cancelled, or was reconciled away. Charging a secure cancel
        // against it spends a fee on nothing.
        const PendingOffer po = state_->get_offer(oid);
        if (po.offer_id.empty()) {
            logger_->info("cancel_ids: {} is no longer tracked -- skipping",
                          oid.substr(0, 12));
            continue;
        }

        // [review] The guard every other cancel path in this file already
        // carries and this one did not: cancel_stale skips cancel_pending
        // offers, classify_tier_staleness skips them ("Already being
        // cancelled"), the peg-suspend drain skips them, and the Step 8
        // draining gate's safety argument is literally "cancel_stale skips
        // cancel_pending offers, so no double-spend".
        //
        // Without it, sweep_cancel_intent charged a fresh secure cancel here
        // every heartbeat against offers whose cancel spend was already in
        // flight. recheck_terminal returns Revived for PENDING_CANCEL and
        // calls mark_cancel_pending() precisely to stop this -- but only
        // callers that READ the flag are stopped by it.
        if (po.cancel_pending && local_cancel_live_.count(oid) > 0U) {
            // [review #171 round 18] Not a cancel in flight: the wallet
            // cancelled it only locally, and the chain last showed every
            // maker coin unspent.  It can still be taken, so it is reported
            // outstanding, never already pending -- or a retry would close
            // the book over it.  Nothing is sent.
            const std::string why = "the wallet cancelled " + oid.substr(0, 12)
                + " only locally, and the chain last showed it still takeable";
            logger_->warn("cancel_ids: {} -- reported outstanding, nothing sent", why);
            const auto cls = execution::classify_take_failure(why);
            out.worst_class = have_class ? execution::more_retryable(out.worst_class, cls)
                                         : cls;
            have_class = true;
            out.last_error = why;
            out.failed.push_back(oid);
            continue;
        }
        if (po.cancel_pending) {
            logger_->info("cancel_ids: {} already has a cancel in flight -- "
                          "skipping (a second secure cancel would pay a "
                          "second fee for the same spend)",
                          oid.substr(0, 12));
            out.already_pending.push_back(oid);
            continue;
        }

        bool cancel_ok       = false;
        bool needs_emergency = false;
        // [S46] Per-offer, NOT written straight into out.last_error. An
        // error from an offer that then succeeded through the emergency
        // ladder must not survive to be classified: `last_error` is the
        // retry policy's only evidence, and an empty-or-wrong string there
        // puts the dominant sync refusal on the short unmodelled leash.
        // out.last_error is therefore assigned ONLY on the failed branch.
        std::string err;
        try {
            co_await cancel_offer_charged(oid, cancel_fee_for(oid),
                                          /*secure=*/true);
            logger_->debug("Cancelled offer {}", oid.substr(0, 12));
            cancel_ok = true;
        } catch (const rpc::ChiaRPCError& inner_e) {
            err = inner_e.what();
            const std::string_view msg{err};
            needs_emergency =
                msg.find("insufficient funds") != std::string_view::npos ||
                msg.find("spendable balance") != std::string_view::npos;
            if (!needs_emergency) {
                logger_->error("Failed to cancel offer {}: {}",
                               oid.substr(0, 12), err);
            }
        }
        if (needs_emergency) {
            // The emergency ladder is the answer to a funding refusal.
            cancel_ok = co_await emergency_cancel(oid, "cancel_ids");
        }

        if (cancel_ok) {
            out.cancelled.push_back(oid);
            state_->mark_cancel_pending(oid);
        } else {
            out.failed.push_back(oid);
            if (!err.empty()) {
                // Fold the class across the WHOLE batch before anyone gets to
                // classify one arbitrary sample of it.
                const auto cls = execution::classify_take_failure(err);
                out.worst_class = have_class
                    ? execution::more_retryable(out.worst_class, cls)
                    : cls;
                have_class = true;
                out.last_error = std::move(err);
            }
        }
    }

    co_return out;
}

// ---------------------------------------------------------------------------
// evaluate_rebalance -- check all five rebalance triggers
// ---------------------------------------------------------------------------

RebalanceReason OfferManager::evaluate_rebalance(
    const std::string& pair_name,
    Mojo               current_mid,
    BlockHeight        current_block,
    double             current_vol,
    double             current_volume) const
{
    auto it = rebalance_baselines_.find(pair_name);
    if (it == rebalance_baselines_.end()) {
        // No baseline recorded yet -- always trigger initial posting.
        return RebalanceReason::PriceMove;
    }

    const RebalanceSnapshot& base = it->second;
    RebalanceReason result = RebalanceReason::None;

    // Trigger 1: Price deviation > 2% from last rebalance mid.
    if (base.mid_price > 0) {
        double deviation = std::abs(
            static_cast<double>(current_mid - base.mid_price)
            / static_cast<double>(base.mid_price));
        if (deviation > kPriceDeviationThreshold) {
            result = result | RebalanceReason::PriceMove;
            logger_->debug("Rebalance trigger: price deviation {:.2f}% for {}",
                           deviation * 100.0, pair_name);
        }
    }

    // Trigger 2: Inventory skew > 60%.
    // Resolve the pair's base and quote asset IDs from the pair config map
    // so that inventory_skew() receives the correct position keys.
    // ISO/IEC 5055: previous code passed pair_name for both arguments,
    // which produced a degenerate skew of 0 (same numerator and denominator).
    double skew = 0.0;
    auto pair_it = pair_config_map_.find(pair_name);
    if (pair_it != pair_config_map_.end()) {
        const auto& pc = pair_it->second;
        skew = state_->inventory_skew(pc.base_asset_id, pc.quote_asset_id);
    } else {
        logger_->warn("evaluate_rebalance: no pair config for '{}' -- "
                       "skew defaults to 0.0", pair_name);
    }
    if (std::abs(skew) > kInventorySkewThreshold) {
        result = result | RebalanceReason::InventorySkew;
        logger_->debug("Rebalance trigger: inventory skew {:.2f} for {}",
                       skew, pair_name);
    }

    // Trigger 3: Time decay > ~69 blocks (1 hour at 52 s/block).
    if (current_block >= base.block_height + kTimeDecayBlocks) {
        result = result | RebalanceReason::TTLExpired;
        logger_->debug("Rebalance trigger: time decay ({} blocks stale) for {}",
                       current_block - base.block_height, pair_name);
    }

    // Trigger 4: Volume spike > 3x rolling average.
    if (base.volume_avg > 0.0 &&
        current_volume > kVolumeSpikeMultiplier * base.volume_avg) {
        result = result | RebalanceReason::RegimeChange;
        logger_->debug("Rebalance trigger: volume spike ({:.0f} vs avg {:.0f}) "
                       "for {}", current_volume, base.volume_avg, pair_name);
    }

    // Trigger 5: Volatility spike > 2x 7-day average.
    if (base.volatility_7d > 0.0 &&
        current_vol > kVolSpikeMult * base.volatility_7d) {
        result = result | RebalanceReason::ForcedRefresh;
        logger_->debug("Rebalance trigger: vol spike ({:.4f} vs 7d avg {:.4f}) "
                       "for {}", current_vol, base.volatility_7d, pair_name);
    }

    return result;
}

// ---------------------------------------------------------------------------
// record_rebalance -- update the baseline snapshot for a pair
// ---------------------------------------------------------------------------

void OfferManager::record_rebalance(const std::string&       pair_name,
                                    const RebalanceSnapshot& snap)
{
    rebalance_baselines_[pair_name] = snap;
    logger_->debug("Recorded rebalance baseline for {} at block {}, mid={}",
                   pair_name, snap.block_height, snap.mid_price);
}

// ---------------------------------------------------------------------------
// build_tier_ladder -- expand strategy output into multi-tier bid+ask quotes
// ---------------------------------------------------------------------------

std::vector<TierQuote> OfferManager::build_tier_ladder(
    Mojo   mid_price,
    Mojo   total_base,
    Mojo   total_quote,
    Mojo   cost_basis,
    double inv_skew) const
{
    std::vector<TierQuote> ladder;
    ladder.reserve(strategy_cfg_.num_tiers * 2);

    // Compute the never-sell-at-loss floor for ask prices.
    // Floor = cost_basis * (1 + min_profit_margin_bps / 10000).
    const double margin_mult =
        1.0 + strategy_cfg_.min_profit_margin_bps / 10000.0;
    const Mojo ask_floor = (cost_basis > 0)
        ? static_cast<Mojo>(std::ceil(
              static_cast<double>(cost_basis) * margin_mult))
        : 0;

    // Inventory skew adjusts relative sizing between bid and ask sides.
    // Positive skew = long base -> DECREASE bid size (buy less),
    //                               INCREASE ask size (sell more to reduce).
    // Negative skew = short base -> INCREASE bid size (buy more to rebuild),
    //                                DECREASE ask size (sell less).
    // This is the standard market-making inventory-reduction convention:
    // lean against the accumulated position to revert toward neutral.
    // Clamped to [-1, +1] for safety.
    const double clamped_skew = std::clamp(inv_skew, -1.0, 1.0);

    // Skew multiplier: at skew=+1 (long base), bid gets 0.5x (buy less),
    // ask gets 1.5x (sell more).  At skew=0, both get 1.0x (symmetric).
    // ISO/IEC 5055: signs inverted relative to original code which
    // incorrectly reinforced the existing position instead of reducing it.
    const double bid_size_mult = 1.0 - 0.5 * clamped_skew;
    const double ask_size_mult = 1.0 + 0.5 * clamped_skew;

    for (std::uint32_t i = 0; i < strategy_cfg_.num_tiers; ++i) {
        const double spacing_frac =
            strategy_cfg_.tier_spacing_bps[i] / 10000.0;
        const double size_frac = strategy_cfg_.tier_size_pct[i];

        // --- BID tier ---
        // Bid price = mid * (1 - tier_spacing)
        const Mojo bid_price = static_cast<Mojo>(
            std::floor(static_cast<double>(mid_price) * (1.0 - spacing_frac)));

        // Bid size = total_quote allocation, converted to base at bid_price,
        // adjusted by skew.  Floored to prevent zero-size offers.
        Mojo bid_size = 0;
        if (bid_price > 0) {
            bid_size = static_cast<Mojo>(std::floor(
                static_cast<double>(total_quote) * size_frac
                * bid_size_mult
                / static_cast<double>(bid_price)));
        }

        if (bid_price > 0 && bid_size > 0) {
            TierQuote bq;
            bq.side       = Side::Bid;
            bq.tier_index = static_cast<std::uint8_t>(i);
            bq.price      = bid_price;
            bq.size       = bid_size;
            bq.spread_bps = strategy_cfg_.tier_spacing_bps[i];
            ladder.push_back(bq);
        }

        // --- ASK tier ---
        // Ask price = mid * (1 + tier_spacing), but no lower than ask_floor
        // (never-sell-at-loss constraint).
        Mojo ask_price = static_cast<Mojo>(
            std::ceil(static_cast<double>(mid_price) * (1.0 + spacing_frac)));

        // Enforce the never-sell-at-loss floor.
        if (ask_floor > 0 && ask_price < ask_floor) {
            ask_price = ask_floor;
            logger_->debug("Tier {} ask raised to floor {} (cost basis {})",
                           i, ask_floor, cost_basis);
        }

        // Ask size = total_base allocation, adjusted by skew.
        Mojo ask_size = static_cast<Mojo>(std::floor(
            static_cast<double>(total_base) * size_frac * ask_size_mult));

        if (ask_price > 0 && ask_size > 0) {
            TierQuote aq;
            aq.side       = Side::Ask;
            aq.tier_index = static_cast<std::uint8_t>(i);
            aq.price      = ask_price;
            aq.size       = ask_size;
            aq.spread_bps = strategy_cfg_.tier_spacing_bps[i];
            ladder.push_back(aq);
        }
    }

    logger_->debug("build_tier_ladder: {} entries (mid={}, base={}, quote={}, "
                   "cost_basis={}, skew={:.2f})",
                   ladder.size(), mid_price, total_base, total_quote,
                   cost_basis, inv_skew);

    return ladder;
}

// ---------------------------------------------------------------------------
// pending_count -- accessor for current pending offer count
// ---------------------------------------------------------------------------

std::size_t OfferManager::pending_count() const
{
    return state_->offer_count();
}

// ---------------------------------------------------------------------------
// set_dynamic_fee / current_fee -- dynamic fee override
// ---------------------------------------------------------------------------

void OfferManager::set_dynamic_fee(std::uint64_t fee_mojos) noexcept
{
    current_fee_mojos_ = fee_mojos;
}

void OfferManager::set_abort_predicate(std::function<bool()> predicate)
{
    abort_predicate_ = std::move(predicate);
}

void OfferManager::set_stop_creating_predicate(std::function<bool()> predicate)
{
    stop_creating_predicate_ = std::move(predicate);
}

void OfferManager::set_escalation(
    std::function<void(const std::string&)> escalate)
{
    escalate_ = std::move(escalate);
}

void OfferManager::set_posting_in_flight_flag(bool* flag) noexcept
{
    posting_in_flight_flag_ = flag;
}

void OfferManager::set_create_outcome_unknown_flag(bool* flag) noexcept
{
    create_outcome_unknown_flag_ = flag;
}

void OfferManager::note_create_outcome_unknown(
    const rpc::ChiaRPCTransportError& e,
    const std::string&                pair_name,
    const char*                       context)
{
    // A connect or TLS failure happens before the request is written, so the
    // wallet cannot have made an offer. Everything else may follow a request
    // the handler received -- a timeout, an empty or garbled reply, a 5xx, or
    // a 2xx whose body could not be parsed.
    if (!rpc::request_possibly_submitted(e.curl_code(), e.http_code())) {
        return;
    }
    logger_->error("{} create for {} failed with NO ANSWER ({}): this does "
                   "not prove the wallet refused it. If the handler built the "
                   "offer, this process holds no record of it and the next "
                   "start meets it as an ORPHAN.",
                   context, pair_name, e.what());
    if (create_outcome_unknown_flag_ != nullptr) {
        *create_outcome_unknown_flag_ = true;
    }
}

std::uint64_t OfferManager::current_fee() const noexcept
{
    return current_fee_mojos_;
}

// ---------------------------------------------------------------------------
// [S67] Class-aware cancel fees
// ---------------------------------------------------------------------------

void OfferManager::set_cancel_fees(std::uint64_t xch_offered_mojos,
                                   std::uint64_t cat_offered_mojos) noexcept
{
    cancel_fees_active_   = true;
    cancel_fee_xch_mojos_ = xch_offered_mojos;
    cancel_fee_cat_mojos_ = cat_offered_mojos;
}

void OfferManager::clear_cancel_fees() noexcept
{
    cancel_fees_active_ = false;
}

void OfferManager::set_cancel_observer(
    std::function<void(const std::string&, std::uint64_t)> observer)
{
    cancel_observer_ = std::move(observer);
}

std::uint64_t OfferManager::cancel_fee_for(const std::string& offer_id) const
{
    if (!cancel_fees_active_) {
        return current_fee_mojos_;   // the pre-S67 behaviour, untouched
    }
    // A bid offers the QUOTE asset, an ask the BASE: those are the coins a
    // secure cancel spends.
    bool known          = false;
    bool offered_is_xch = false;
    const PendingOffer po = state_->get_offer(offer_id);
    if (!po.offer_id.empty()) {
        const auto it = pair_config_map_.find(po.pair_name);
        if (it != pair_config_map_.end()) {
            const std::string& offered = (po.side == Side::Bid)
                ? it->second.quote_asset_id : it->second.base_asset_id;
            known          = true;
            offered_is_xch = (offered == "xch");
        }
    }
    return strategy::fee::cancel_fee_for(cancel_fees_active_, current_fee_mojos_,
                                         cancel_fee_xch_mojos_, cancel_fee_cat_mojos_,
                                         known, offered_is_xch);
}

std::uint32_t OfferManager::take_fee_rejections_seen() noexcept
{
    const std::uint32_t seen = fee_rejections_seen_;
    fee_rejections_seen_ = 0;
    return seen;
}

std::uint64_t OfferManager::take_cancel_fees_accepted() noexcept
{
    const std::uint64_t paid = cancel_fees_accepted_;
    cancel_fees_accepted_ = 0;
    return paid;
}

// ---------------------------------------------------------------------------
// invalidate_wallet_ids -- force the wallet-ID cache to be rebuilt
// ---------------------------------------------------------------------------

void OfferManager::invalidate_wallet_ids() noexcept
{
    wallet_ids_resolved_ = false;
    wallet_id_map_.clear();
    logger_->info("Wallet ID cache invalidated -- will re-query on next "
                  "post_quotes()");
}

// ---------------------------------------------------------------------------
// ensure_wallet_ids -- public one-shot cache population
// ---------------------------------------------------------------------------

asio::awaitable<void> OfferManager::ensure_wallet_ids()
{
    if (!wallet_ids_resolved_) {
        co_await init_wallet_id_map();
    }
}

// ---------------------------------------------------------------------------
// [T5-01] classify_tier_staleness -- direction-aware price-deviation check
//
// Scholarly basis:
//   - Gao & Wang (2020): optimal cancel threshold is a function of the
//     fractional deviation from the current fair price.
//   - Ait-Sahalia & Saglam (2017): stale-quote risk increases with the
//     magnitude of price deviation, not uniformly across all tiers.
//
// Direction-aware approach:
//   Only *adverse* deviations trigger cancellation.  An adverse move is
//   one that makes our offer more generous than intended:
//     - Bid: new optimal < old price -> our bid is too high (overpaying)
//     - Ask: new optimal > old price -> our ask is too low (underselling)
//   Favorable deviations (bid drifted below optimal, ask drifted above)
//   make the offer more conservative and are safe to leave live -- with one
//   exception: in the normal zone [S33] refreshes a favorable drift past 3x
//   the tier threshold (kFavorableDriftMultiplier, cross_guard.hpp) as a
//   disconnected quote.  Past the soft TTL, direction alone decides.
//
//   Additionally, if the offer has crossed the mid-price it is flagged
//   for urgent cancellation regardless of threshold.
// ---------------------------------------------------------------------------

std::vector<TierClassification> OfferManager::classify_tier_staleness(
    const std::string&             pair_name,
    const std::vector<TierQuote>&  new_ladder,
    BlockHeight                    current_block,
    BlockHeight                    ttl_blocks,
    Mojo                           mid_price,
    bool                           anchor_active,
    bool                           can_bid,
    bool                           can_ask,
    double                         margin_centre,
    double                         margin_min_edge_bps,
    double                         margin_fair_centre) const
{
    std::vector<TierClassification> results;

    // [S70] Whether the unconditional hard-TTL cancel below is in force for
    // an offer is age_limit_cancel_applies(expire_mode, its verified expiry).
    const bool expire_mode =
        strategy_cfg_.ttl_cancel_mode == TtlCancelMode::Expire;

    // [S72] Margin mode replaces the deviation zones with ONE edge test
    // against Step 7's own centre and floor (cross_guard.hpp).  With no
    // usable reference -- the pace pass sends none, and Step 7 leaves both at
    // 0 until it reaches ladder generation -- every offer falls back to the
    // deviation zones, i.e. to today's rule, never to "keep regardless".
    const bool margin_mode =
        strategy_cfg_.price_cancel_mode == PriceCancelMode::Margin;
    const double edge_retain = strategy_cfg_.price_cancel_edge_retain;

    auto pending = state_->get_all_offers();
    if (pending.empty()) return results;

    // Build lookups: (side, tier_index) -> new optimal price/size.
    // This allows O(1) comparison for each pending offer.
    std::unordered_map<std::string, Mojo> optimal_prices;
    std::unordered_map<std::string, Mojo> optimal_sizes;
    for (const auto& tq : new_ladder) {
        std::string key = std::to_string(static_cast<int>(tq.side))
                        + "_" + std::to_string(tq.tier_index);
        optimal_prices[key] = tq.price;
        optimal_sizes[key]  = tq.size;
    }

    const double mid_p = static_cast<double>(mid_price);

    // Precompute hard TTL: absolute safety cap beyond which all offers
    // are expired regardless of price accuracy.
    const BlockHeight hard_ttl = ttl_blocks * kHardTtlMultiplier;

    for (const auto& po : pending) {
        if (po.pair_name != pair_name) continue;
        if (po.cancel_pending) continue;  // Already being cancelled.

        TierClassification tc;
        tc.offer_id   = po.offer_id;
        tc.tier_index = po.tier;
        tc.side       = po.side;

        // [v0.7.48] Side-aware balance/rebalance check.
        // If a side is completely forbidden due to one-sided rebalancing
        // (xch_buy_only_mode or ratio_force_one_sided), we must cancel
        // active offers on that side immediately to prevent sudden market
        // swings from filling them and further worsening the imbalance.
        if ((!can_bid && po.side == Side::Bid) || (!can_ask && po.side == Side::Ask)) {
            tc.staleness       = TierStaleness::Stale;
            tc.price_deviation = 1.0;  // maximal to represent forced rebalance
            results.push_back(std::move(tc));
            logger_->info("classify_tier_staleness({}): tier {} {} "
                          "side forbidden because the trade type/amount violates "
                          "balance rebalancing constraints -- marking stale for cancellation",
                          pair_name, po.tier, to_string(po.side));
            continue;
        }

        const BlockHeight age = (current_block > po.created_at_block)
            ? (current_block - po.created_at_block) : 0;
        const bool past_soft_ttl = (age >= ttl_blocks);
        const bool past_hard_ttl = (age >= hard_ttl);

        // Hard TTL: absolute expiration regardless of price.
        // Safety backstop -- offers should never live indefinitely.
        //
        // [S70] Under ttl_cancel_mode: expire an offer that VERIFIABLY
        // carries an on-chain expiry already has that backstop, enforced by
        // the chain for free, so it is not cancelled merely for its age; it
        // falls through to every price rule below, and
        // retire_expired_offers frees its coins once a TRUSTED peer's chain
        // clock is past its max_time.  An offer with no verified expiry (0)
        // keeps the hard TTL.
        if (past_hard_ttl
            && age_limit_cancel_applies(expire_mode, po.expiry_max_time)) {
            tc.staleness       = TierStaleness::Expired;
            tc.price_deviation = 1.0;  // maximal
            results.push_back(std::move(tc));
            continue;
        }

        // [S72] The margin verdict for THIS offer, shared by both branches
        // below.  `crossed` is filled in by each branch first.
        const auto margin_verdict = [&](bool crossed) {
            return classify_tier_refresh_margin(
                crossed, age < kMinRefreshAgeBlocks, po.side == Side::Ask,
                static_cast<double>(po.price), margin_centre,
                margin_fair_centre, margin_min_edge_bps, edge_retain);
        };
        const auto note_margin_breach = [&](TierClassification& out) {
            out.margin_breach     = true;
            out.edge_bps          = margin_edge_bps(
                po.side == Side::Ask, static_cast<double>(po.price),
                margin_centre, margin_fair_centre);
            out.required_edge_bps = margin_min_edge_bps * edge_retain;
        };

        // Look up the optimal price for this tier.
        std::string key = std::to_string(static_cast<int>(po.side))
                        + "_" + std::to_string(po.tier);
        auto opt_it = optimal_prices.find(key);
        if (opt_it == optimal_prices.end()) {
            // Tier no longer in the new ladder (budget constraints,
            // sub-unit minimum, or fee gating removed it).
            //
            // When no replacement tier can be posted, cancelling the
            // existing offer wastes cancel fees and removes liquidity
            // from the book.  Fall back to a mid-price sanity check:
            // only cancel if the offer has crossed the mid-price
            // (immediate adverse selection risk).  Hard TTL is already
            // handled above and takes precedence.
            const double old_p = static_cast<double>(po.price);

            if (mid_price > 0) {
                tc.price_deviation = std::abs(old_p - mid_p) / mid_p;
            }
            {
                // Use BBO for crossing check (same logic as main branch).
                // [S33 2026-09-12] Gated on the OPPOSITE touch alone -- see
                // the twin block below, and cross_guard.hpp for why.
                const auto bbo_snap  = state_->get_market(pair_name);
                const double bbo_ask = static_cast<double>(bbo_snap.best_ask);
                const double bbo_bid = static_cast<double>(bbo_snap.best_bid);
                const bool have_opposite = (po.side == Side::Bid)
                    ? (bbo_ask > 0.0) : (bbo_bid > 0.0);
                if (have_opposite) {
                    if (po.side == Side::Bid && old_p >= bbo_ask) {
                        tc.crossed = true;
                    } else if (po.side == Side::Ask && old_p <= bbo_bid) {
                        tc.crossed = true;
                    }
                } else if (mid_price > 0) {
                    constexpr double kCrossBuffer = 0.05;
                    if (po.side == Side::Bid && old_p > mid_p * (1.0 + kCrossBuffer)) {
                        tc.crossed = true;
                    } else if (po.side == Side::Ask && old_p < mid_p * (1.0 - kCrossBuffer)) {
                        tc.crossed = true;
                    }
                }
            }

            if (tc.crossed) {
                tc.staleness       = TierStaleness::Stale;
                tc.adverse         = true;
            } else {
                tc.staleness       = TierStaleness::Fresh;
                tc.adverse         = false;
            }

            // [S72] The edge test needs no replacement tier: it asks about
            // THIS offer's fill, and a tier the budget dropped is still
            // takeable at its resting price.  Deviation mode keeps such an
            // offer unless crossed, as it always did.
            if (margin_mode && !tc.crossed
                && margin_verdict(false) == MarginRefresh::Stale) {
                tc.staleness = TierStaleness::Stale;
                tc.adverse   = true;
                note_margin_breach(tc);
            }

            results.push_back(std::move(tc));
            continue;
        }

        // Compute signed price deviation.
        const double old_p = static_cast<double>(po.price);
        const double new_p = static_cast<double>(opt_it->second);
        const double signed_dev = (old_p > 0.0)
            ? (new_p - old_p) / old_p
            : 1.0;

        tc.price_deviation = std::abs(signed_dev);

        // Determine if the deviation is adverse (makes our offer
        // more generous than intended) or favorable (more conservative).
        //   Bid: adverse when new_p < old_p (signed_dev < 0)
        //        -> our old bid is higher than the new optimal, overpaying.
        //   Ask: adverse when new_p > old_p (signed_dev > 0)
        //        -> our old ask is lower than the new optimal, underselling.
        tc.adverse = (po.side == Side::Bid)
            ? (signed_dev < 0.0)
            : (signed_dev > 0.0);

        // Crossing detection: a Bid at or above best_ask (or Ask at or below
        // best_bid) faces immediate adverse selection and must be cancelled
        // urgently.  Using model mid as the threshold is too conservative --
        // a bid between mid and best_ask is a valid competitive bid, not a
        // crossed offer.  When BBO is unavailable, fall back to mid with a
        // generous buffer (5%) to avoid false positives.
        //
        // [S33 2026-09-12] Each side is gated on the OPPOSITE touch ALONE.
        // Requiring both touches meant a one-sided book -- a standing bid
        // with nothing offered, routine on dexie -- fell through to the
        // +/-5% band, and that band is far too loose to catch a cross: an
        // ask at 99 sitting on a bid of 100 passed as 99 >= 100*0.95.  The
        // missing half of the book must not disable the half that is
        // present.  Kept bit-identical to classify_cross_bbo in
        // cross_guard.hpp, the pre-post guard for this same rule -- those
        // two disagreeing is what S33 is about.
        {
            const auto bbo_snap  = state_->get_market(pair_name);
            const double bbo_ask = static_cast<double>(bbo_snap.best_ask);
            const double bbo_bid = static_cast<double>(bbo_snap.best_bid);
            const bool have_opposite = (po.side == Side::Bid)
                ? (bbo_ask > 0.0) : (bbo_bid > 0.0);
            if (have_opposite) {
                // The touch that could take this offer exists: real check.
                if (po.side == Side::Bid && old_p >= bbo_ask) {
                    tc.crossed = true;
                } else if (po.side == Side::Ask && old_p <= bbo_bid) {
                    tc.crossed = true;
                }
            } else if (mid_price > 0) {
                // No opposite touch: fall back to mid +/-5% buffer.
                constexpr double kCrossBuffer = 0.05;
                if (po.side == Side::Bid && old_p > mid_p * (1.0 + kCrossBuffer)) {
                    tc.crossed = true;
                } else if (po.side == Side::Ask && old_p < mid_p * (1.0 - kCrossBuffer)) {
                    tc.crossed = true;
                }
            }
        }

        // Classification logic (cancel-reduction optimisations):
        //
        //   1. Crossed mid-price -> always Stale (urgent cancel, no age guard).
        //
        //   2. Soft TTL zone (soft TTL <= age < hard TTL):
        //      The offer is old.  Apply a gentler adverse threshold
        //      (kSoftTtlAdverseThreshold = 0.2%) -- even a small drift
        //      on an aged offer should trigger a refresh.  But if the
        //      offer is still perfectly priced, keep it.
        //
        //   3. Minimum age guard (age < kMinRefreshAgeBlocks):
        //      Very young offers are protected from cancel churn because
        //      the round-trip fee (cancel + recreate) exceeds the adverse
        //      selection risk for small deviations.  Only crossed-mid
        //      bypasses this.
        //
        //   4. Tier-scaled threshold (normal zone):
        //      Outer tiers tolerate more movement because they are further
        //      from mid and capture larger spreads.
        //      threshold = kSelectiveRefreshThreshold x (1 + tier x scale)
        //        tier 0 -> 0.50%   tier 1 -> 0.75%
        //        tier 2 -> 1.00%   tier 3 -> 1.25%

        // [S33 2026-09-12] The four zones above are now selected by
        // classify_tier_refresh (cross_guard.hpp) -- moved only so xop_tests
        // can drive them: OfferManager cannot be constructed without a wallet
        // RPC client, so nothing was covering this.  The extraction itself is
        // behavior-preserving, but this block is NOT identical to main: the
        // normal zone's 3x favorable multiplier was added by 922b183 earlier
        // in this same PR.
        //
        // [S72] In margin mode the zones are replaced by the edge test, and
        // only a missing reference (NoReference) reaches them.
        bool margin_decided = false;
        if (margin_mode) {
            switch (margin_verdict(tc.crossed)) {
                case MarginRefresh::Fresh:
                    tc.staleness   = TierStaleness::Fresh;
                    margin_decided = true;
                    break;
                case MarginRefresh::Stale:
                    tc.staleness   = TierStaleness::Stale;
                    margin_decided = true;
                    if (!tc.crossed) {
                        note_margin_breach(tc);
                    }
                    break;
                case MarginRefresh::NoReference:
                    break;
            }
        }
        if (!margin_decided) {
            const double tier_threshold = kSelectiveRefreshThreshold
                * (1.0 + static_cast<double>(po.tier) * kTierThresholdScale);
            switch (classify_tier_refresh(tc.crossed,
                                          past_soft_ttl,
                                          age < kMinRefreshAgeBlocks,
                                          tc.adverse,
                                          tc.price_deviation,
                                          tier_threshold,
                                          kSoftTtlAdverseThreshold)) {
                case TierRefresh::Fresh:
                    tc.staleness = TierStaleness::Fresh;
                    break;
                case TierRefresh::Stale:
                    tc.staleness = TierStaleness::Stale;
                    break;
                case TierRefresh::Expired:
                    tc.staleness = TierStaleness::Expired;
                    break;
            }
        }

        // [v0.7.37] Size-based staleness override.
        // When the market allocator changes allocation fractions, existing
        // offers may be significantly oversized relative to the new optimal.
        // If the pending offer's size is > 2x the new ladder's optimal size,
        // override Fresh->Stale to trigger a cancel+repost with correct sizing.
        // Respects the minimum age guard: only apply past kMinRefreshAgeBlocks.
        if (tc.staleness == TierStaleness::Fresh
            && age >= kMinRefreshAgeBlocks) {
            auto sz_it = optimal_sizes.find(key);
            if (sz_it != optimal_sizes.end() && sz_it->second > 0) {
                const double size_ratio =
                    static_cast<double>(po.size)
                    / static_cast<double>(sz_it->second);
                if (size_ratio > kSizeStaleThreshold) {
                    tc.staleness = TierStaleness::Stale;
                    logger_->debug("classify_tier_staleness({}): tier {} {} "
                                   "size oversized {:.1f}x ({}->{}) -- marking "
                                   "stale", pair_name, po.tier,
                                   to_string(po.side), size_ratio,
                                   po.size, sz_it->second);
                }
            }
        }

        // [v0.7.42] Competitive anchor repricing override.
        // When competitive anchor pricing is active, the anchor repositions
        // offers to track the BBO.  A "favorable" drift (bid drifted low,
        // ask drifted high) is normally safe, but under anchor mode it
        // means we're deep in the book instead of near the top.  Override
        // Fresh->Stale when the ABSOLUTE deviation exceeds the tier-scaled
        // threshold, regardless of direction.
        //
        // [S72] Not in margin mode: this override exists to cancel a
        // FAVOURABLE drift, which is the one thing the margin rule promises
        // never to do.  It still runs when the margin rule had no reference
        // and the deviation zones decided instead.
        if (anchor_active
            && !margin_decided
            && tc.staleness == TierStaleness::Fresh
            && age >= kMinRefreshAgeBlocks) {
            const double tier_threshold = kSelectiveRefreshThreshold
                * (1.0 + static_cast<double>(po.tier) * kTierThresholdScale);
            if (tc.price_deviation > tier_threshold) {
                tc.staleness = TierStaleness::Stale;
                logger_->debug("classify_tier_staleness({}): tier {} {} "
                               "anchor repricing dev={:.3f}% > {:.3f}% -- "
                               "marking stale",
                               pair_name, po.tier, to_string(po.side),
                               tc.price_deviation * 100.0,
                               tier_threshold * 100.0);
            }
        }

        results.push_back(std::move(tc));
    }

    // Log summary.
    int fresh = 0, stale = 0, expired = 0;
    for (const auto& tc : results) {
        switch (tc.staleness) {
            case TierStaleness::Fresh:   ++fresh;   break;
            case TierStaleness::Stale:   ++stale;   break;
            case TierStaleness::Expired: ++expired; break;
        }
    }
    logger_->debug("classify_tier_staleness({}): {} fresh, {} stale, "
                   "{} expired", pair_name, fresh, stale, expired);

    return results;
}

// ---------------------------------------------------------------------------
// [T5-01] selective_cancel -- cancel only stale/expired tiers
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>> OfferManager::selective_cancel(
    const std::vector<std::string>& stale_ids)
{
    std::vector<std::string> cancelled_ids;
    if (stale_ids.empty()) co_return cancelled_ids;

    cancelled_ids.reserve(stale_ids.size());

    for (const auto& offer_id : stale_ids) {
        // Skip offers already awaiting cancel confirmation.
        auto existing = state_->get_offer(offer_id);
        if (existing.cancel_pending) {
            continue;
        }
        bool cancel_ok = false;
        bool needs_emergency = false;
        try {
            co_await cancel_offer_charged(
                offer_id, cancel_fee_for(offer_id), /*secure=*/true);
            cancel_ok = true;
        } catch (const rpc::ChiaRPCError& e) {
            const std::string_view msg{e.what()};
            needs_emergency =
                msg.find("insufficient funds") != std::string_view::npos ||
                msg.find("spendable balance") != std::string_view::npos;
            if (!needs_emergency) {
                logger_->error("selective_cancel: failed to cancel {}: {}",
                               offer_id.substr(0, 12), e.what());
            }
        }
        if (needs_emergency)
            cancel_ok = co_await emergency_cancel(
                offer_id, "selective_cancel");
        if (cancel_ok) {
            if (!state_->mark_cancel_pending(offer_id)) {
                logger_->warn("selective_cancel: offer {} already removed "
                              "from state", offer_id.substr(0, 12));
            }
            cancelled_ids.push_back(offer_id);
            logger_->debug("selective_cancel: cancelled {}",
                           offer_id.substr(0, 12));
        }
    }

    if (!cancelled_ids.empty()) {
        logger_->info("selective_cancel: {}/{} offers cancelled",
                      cancelled_ids.size(), stale_ids.size());
    }

    co_return cancelled_ids;
}

// ---------------------------------------------------------------------------
// [T4-11] reconcile_offers -- Full state reconciliation against wallet.
//
// Detects and corrects three classes of discrepancy:
//   1. Orphans: offers in State but not in wallet (wallet may have cancelled
//      or timed out without our knowledge).
//   2. Phantoms: offers in wallet matching our pending set that have
//      transitioned to a terminal status we missed.
//   3. Status mismatches: offers that changed state between polls.
//
// This is intentionally a heavyweight operation (full wallet scan) and
// should only be called periodically (e.g. every 20 blocks).
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>> OfferManager::reconcile_offers(
    BlockHeight current_block)
{
    std::vector<std::string> removed_ids;

    auto pending_offers = state_->get_all_offers();

    // Build a lookup of all pending offer IDs from our state.
    std::unordered_map<std::string, PendingOffer> pending_map;
    pending_map.reserve(pending_offers.size());
    for (auto& po : pending_offers) {
        pending_map.emplace(po.offer_id, std::move(po));
    }

    // Collect all offer IDs found in the wallet for orphan detection.
    std::unordered_set<std::string> wallet_offer_ids;

    // Also collect PENDING_ACCEPT wallet offers for reverse adoption check:
    // wallet offers that are still pending but not tracked in State.
    std::vector<json> unadopted_records;

    // ------------------------------------------------------------------
    // [WALLET-LOAD 2026-08-04] Early-stopped pagination.
    //
    // This loop used to walk the ENTIRE trade archive (14,100+ records at
    // 50/page = ~282 get_all_offers calls) every reconcile because the
    // wallet's DEFAULT ordering (confirmed_at_index DESC) sorts pending
    // offers LAST.  start/end are array indices, not heights (verified:
    // docs.chia.net offer-rpc + chia-blockchain trade_store.py), so a
    // since-height filter is impossible -- but sort_key="RELEVANCE"
    // orders pending statuses FIRST, then terminal records by
    // created_at_time DESC.  Page 1 therefore carries the whole live set
    // (tracked offers + PENDING_ACCEPT adoptees), and the scan stops
    // after kReconcileStopAfterOldPages consecutive pages entirely older
    // than the oldest tracked offer's creation minus a 24 h adoptee
    // slack (wallet_poll_throttle.hpp).  ~282 calls -> ~2-4.
    //
    // SAFETY: phantom/adoption semantics for TRACKED offers are fully
    // preserved.  Absence-from-scan was never trusted (SETTLE-FIX
    // 2026-07-31): every tracked offer not seen in the scanned pages is
    // individually verified with get_offer below before any removal, so
    // stopping early can only convert bulk pages into a handful of
    // targeted lookups, never a wrong removal.
    // ------------------------------------------------------------------
    std::int64_t oldest_tracked_unix = 0;
    for (const auto& [id, po] : pending_map) {
        const auto unix_s = std::chrono::duration_cast<std::chrono::seconds>(
            po.created_at_ts.time_since_epoch()).count();
        if (unix_s > 0
            && (oldest_tracked_unix == 0 || unix_s < oldest_tracked_unix)) {
            oldest_tracked_unix = unix_s;
        }
    }
    const std::int64_t now_unix =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const std::int64_t cutoff_unix =
        execution::reconcile_scan_cutoff(oldest_tracked_unix, now_unix);
    execution::ReconcileEarlyStop early_stop;

    // Paginate through wallet offers, newest-relevant first.
    constexpr std::int64_t kPageSize = 50;
    std::int64_t offset = 0;
    std::int64_t pages_scanned = 0;
    bool more = true;

    while (more) {
        std::vector<json> trade_records;
        try {
            trade_records = co_await wallet_->get_all_offers(
                offset, offset + kPageSize, /*file_contents=*/false,
                /*include_completed=*/true,
                /*sort_key=*/"RELEVANCE", /*reverse=*/false);
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("[reconcile] get_all_offers failed: {}", e.what());
            co_return removed_ids;
        }
        ++pages_scanned;

        if (trade_records.empty() ||
            static_cast<std::int64_t>(trade_records.size()) < kPageSize) {
            more = false;
        }

        // Newest created_at_time on this page, for the early-stop rule.
        std::int64_t page_newest_created = 0;
        for (const auto& rec : trade_records) {
            const std::int64_t created =
                rec.value("created_at_time", std::int64_t{0});
            page_newest_created = std::max(page_newest_created, created);
        }

        for (const auto& rec : trade_records) {
            if (!rec.contains("trade_id") || !rec.contains("status")) {
                continue;
            }
            std::string trade_id = rec["trade_id"].get<std::string>();
            int status = trade_status::parse(rec["status"]);

            wallet_offer_ids.insert(trade_id);

            // Check if this wallet offer is one we are tracking.
            auto it = pending_map.find(trade_id);
            if (it == pending_map.end()) {
                // Not tracked in State.  If PENDING_ACCEPT, this is a
                // wallet offer the engine lost track of (e.g., removed
                // by verify_pending_offer_coins during a wallet desync).
                // Collect for adoption below.
                if (status == trade_status::kPendingAccept) {
                    unadopted_records.push_back(rec);
                }
                continue;
            }

            // Detected terminal state that we missed during normal polling.
            // [review #171 round 15] FAILED only.  A CANCELLED is left to
            // detect_fills, which proves it on-chain before it closes it: a
            // cancel's status can overwrite a real take (round 11), and a
            // local cancel leaves the offer takeable, with nothing in the
            // wallet watching its coins.  Marked cancel_pending, so it is
            // polled every heartbeat and nothing cancels it again.
            if (status == trade_status::kCancelled) {
                state_->mark_cancel_pending(trade_id);
            } else if (status == trade_status::kFailed) {
                state_->remove_offer(trade_id);
                removed_ids.push_back(trade_id);
                logger_->warn("[reconcile] Removed orphaned offer {} "
                              "(wallet status={}, was pending in State)",
                              trade_id.substr(0, 12), status);
            }
            // Note: confirmed fills (status==4) are not processed here --
            // they are handled by detect_fills() which now uses
            // include_completed=true and matches against cancel_pending
            // offers still in State.
        }

        // [WALLET-LOAD 2026-08-04] Early stop: in the RELEVANCE ordering
        // the terminal region is created_at_time-descending, so once
        // consecutive pages are entirely older than the cutoff nothing
        // relevant can follow.
        if (more && early_stop.observe_page(execution::page_entirely_older(
                        page_newest_created, cutoff_unix))) {
            logger_->debug("[reconcile] early stop after {} pages "
                           "({} consecutive pages older than cutoff {})",
                           pages_scanned,
                           early_stop.consecutive_old_pages(), cutoff_unix);
            more = false;
        }

        offset += kPageSize;
    }

    // Detect orphans: offers in State that no longer exist in the wallet.
    // This can happen if the wallet was restarted or the offer expired
    // server-side without a cancel call.
    //
    // [SETTLE-FIX 2026-07-31] Absence from the paginated scan above is NOT
    // proof the offer is gone.  Offers settle while the loop walks pages, so
    // records get skipped.  Previously a skipped CONFIRMED offer was deleted
    // from State here and stamped "cancelled/periodic_reconcile" by the
    // caller -- which DESTROYED the fill, because detect_fills() only
    // inspects offers still tracked in State.  A read-only wallet probe on
    // 2026-07-31 found 6 XCH/BYC asks the wallet reported CONFIRMED that we
    // had recorded as cancels (6 XCH and its BYC proceeds off the books).
    //
    // Verify each candidate directly before destroying tracking state.  We
    // deliberately do NOT emit the Fill from here: leaving a confirmed offer
    // in State lets detect_fills() handle it through the normal path, which
    // captures the creation fee and updates position accounting correctly.
    for (const auto& [offer_id, po] : pending_map) {
        if (wallet_offer_ids.find(offer_id) != wallet_offer_ids.end()) {
            continue;
        }

        int status = -1;
        try {
            const json rec = co_await wallet_->get_offer(
                offer_id, /*file_contents=*/false);
            if (rec.contains("status")) {
                status = trade_status::parse(rec["status"]);
            }
        } catch (const rpc::ChiaRPCError& e) {
            // Fail safe: never destroy tracking on an RPC error.  The offer
            // stays in State and is re-examined next cycle.
            logger_->warn("[reconcile] get_offer failed for {} ({}) -- "
                          "keeping tracked, will retry: {}",
                          offer_id.substr(0, 12), po.pair_name, e.what());
            continue;
        }

        if (status == trade_status::kConfirmed) {
            logger_->info("[reconcile] Offer {} ({}) missing from scan but "
                          "wallet reports CONFIRMED -- keeping tracked so "
                          "detect_fills records the fill",
                          offer_id.substr(0, 12), po.pair_name);
            continue;
        }

        if (status == trade_status::kCancelled) {
            // [review #171 round 15] Left to detect_fills, as in the scan.
            state_->mark_cancel_pending(offer_id);
            logger_->debug("[reconcile] Offer {} ({}) missing from scan, "
                           "wallet status CANCELLED -- left tracked for "
                           "detect_fills to prove", offer_id.substr(0, 12),
                           po.pair_name);
            continue;
        }

        if (status != trade_status::kFailed) {
            // Still live, pending, or unrecognised -- not ours to remove.
            logger_->debug("[reconcile] Offer {} ({}) missing from scan, "
                           "wallet status={} -- keeping tracked",
                           offer_id.substr(0, 12), po.pair_name, status);
            continue;
        }

        state_->remove_offer(offer_id);
        removed_ids.push_back(offer_id);
        logger_->warn("[reconcile] Removed phantom offer {} ({}) "
                      "-- wallet status={}",
                      offer_id.substr(0, 12), po.pair_name, status);
    }

    // -----------------------------------------------------------------------
    // Reverse adoption: wallet PENDING_ACCEPT offers not tracked in State.
    //
    // Root cause: verify_pending_offer_coins or a previous reconciliation
    // removed an offer from State (e.g., during wallet desync, pagination
    // race, or restart), but the wallet still holds it as PENDING_ACCEPT
    // with coins locked.  Without adoption the engine sees 0 pending
    // offers yet 0 spendable XCH -- a permanent deadlock.
    //
    // Gao & Wang (2020): tracking an offer we created is always cheaper
    // than leaving coins locked invisibly.  Worst case the UTXO liberation
    // or cancel_stale path will cancel it on the next heartbeat.
    // -----------------------------------------------------------------------
    std::size_t adopted_count = 0;
    for (const auto& rec : unadopted_records) {
        const std::string trade_id = rec["trade_id"].get<std::string>();

        // Skip if the offer was just removed in this reconciliation cycle
        // (terminal state detected above).
        bool just_removed = false;
        for (const auto& rid : removed_ids) {
            if (rid == trade_id) { just_removed = true; break; }
        }
        if (just_removed) continue;

        auto parsed = try_parse_wallet_offer(rec, current_block);
        if (parsed) {
            state_->upsert_offer(*parsed);
            ++adopted_count;
            logger_->warn("[reconcile] ADOPTED untracked wallet offer {} "
                          "({} {} on {}) -- wallet has it PENDING_ACCEPT "
                          "but engine lost tracking",
                          trade_id.substr(0, 12),
                          (parsed->side == Side::Bid) ? "BID" : "ASK",
                          parsed->size, parsed->pair_name);
        } else {
            // Can't determine pair/side -- adopt with minimal metadata.
            // UTXO liberation can still cancel it to free locked coins.
            PendingOffer po;
            po.offer_id         = trade_id;
            po.pair_name        = "UNKNOWN";
            po.side             = Side::Bid;
            po.price            = 0;
            po.size             = 0;
            po.tier             = 0;
            po.fee_mojos        = 0;
            po.created_at_block = 0;
            state_->upsert_offer(po);
            ++adopted_count;
            logger_->warn("[reconcile] ADOPTED unparseable wallet offer {} "
                          "-- tracking to prevent coin-lock deadlock",
                          trade_id.substr(0, 12));
        }
    }

    if (!removed_ids.empty() || adopted_count > 0) {
        logger_->info("[reconcile] Corrected {} removals, {} adoptions",
                      removed_ids.size(), adopted_count);
    } else {
        logger_->debug("[reconcile] State consistent -- no discrepancies");
    }

    co_return removed_ids;
}

// ---------------------------------------------------------------------------
// evaluate_orphan -- cost-aware evaluation of an untracked wallet offer
//
// Scholarly basis:
//   Gueant, Lehalle & Fernandez-Tapia (2013) -- cancel only when expected
//     adverse selection loss exceeds the cancellation cost.
//   Gao & Wang (2020) -- the zero-offer gap during cancel->repost is the
//     primary adverse selection cost for latent market makers.  Keeping a
//     slightly stale offer is cheaper than having no presence.
//   Ait-Sahalia & Saglam (2017) -- stale-quote risk = f(price_deviation,
//     remaining_lifetime, offer_size).  Cancellation threshold should
//     scale with all three factors.
//
// The Chia wallet trade record's "summary" field contains:
//   { "offered":   [ [asset_id, amount], ... ],
//     "requested": [ [asset_id, amount], ... ] }
//
// From this we derive the pair, side, price, and size, then compare the
// offer's implied price against the current market mid-price to decide
// whether to adopt (re-track) or cancel (waste a fee but avoid loss).
// ---------------------------------------------------------------------------

OrphanEvaluation OfferManager::evaluate_orphan(
    const json& trade_record,
    BlockHeight current_block,
    const std::unordered_map<std::string, Mojo>& mid_prices) const
{
    OrphanEvaluation eval;
    eval.trade_id = trade_record.value("trade_id", "");

    // ---- Parse summary to extract offered/requested amounts ---------------
    if (!trade_record.contains("summary") ||
        !trade_record["summary"].is_object()) {
        eval.disposition = OrphanDisposition::Unknown;
        eval.reason = "no summary field in trade record";
        return eval;
    }
    const auto& summary = trade_record["summary"];

    // Parse offered and requested asset->amount maps.
    // Chia wallet format: { "asset_id_hex_or_xch": amount_int, ... }
    std::unordered_map<std::string, Mojo> offered;    // we give
    std::unordered_map<std::string, Mojo> requested;  // we get

    auto parse_side = [](const json& obj,
                         std::unordered_map<std::string, Mojo>& out) {
        if (!obj.is_object()) return;
        for (auto& [asset_id, amount_val] : obj.items()) {
            if (amount_val.is_number_integer()) {
                out[asset_id] = amount_val.get<Mojo>();
            } else if (amount_val.is_number_unsigned()) {
                out[asset_id] = static_cast<Mojo>(
                    amount_val.get<std::uint64_t>());
            }
        }
    };

    if (summary.contains("offered"))   parse_side(summary["offered"],   offered);
    if (summary.contains("requested")) parse_side(summary["requested"], requested);

    if (offered.empty() || requested.empty()) {
        eval.disposition = OrphanDisposition::Unknown;
        eval.reason = "empty offered or requested in summary";
        return eval;
    }

    // ---- Match against our configured pairs --------------------------------
    // For each pair, check if the offered/requested asset IDs match.
    // An offer where we GIVE base and GET quote = ask (we sell).
    // An offer where we GIVE quote and GET base = bid (we buy).
    bool matched = false;
    for (const auto& [pname, pcfg] : pair_config_map_) {
        const std::string& base  = pcfg.base_asset_id;
        const std::string& quote = pcfg.quote_asset_id;

        bool offered_base  = offered.count(base)  > 0;
        bool offered_quote = offered.count(quote)  > 0;
        bool requested_base  = requested.count(base) > 0;
        bool requested_quote = requested.count(quote) > 0;

        if (offered_base && requested_quote) {
            // We gave base, got quote -> ASK (we sold base).
            eval.pair_name = pname;
            eval.side      = Side::Ask;
            eval.size      = offered.at(base);
            // Price = quote_amount / base_amount (quote mojos per base mojo).
            // But our pricing is in "quote per unit base", scaled to mojos.
            // For XCH-denominated pairs, both sides are in mojos already.
            const double base_d  = static_cast<double>(offered.at(base));
            const double quote_d = static_cast<double>(requested.at(quote));
            eval.price = (base_d > 0.0)
                ? static_cast<Mojo>(std::llround(quote_d / base_d
                    * static_cast<double>(kMojosPerXch)))
                : 0;
            matched = true;
            break;
        }
        if (offered_quote && requested_base) {
            // We gave quote, got base -> BID (we bought base).
            eval.pair_name = pname;
            eval.side      = Side::Bid;
            eval.size      = requested.at(base);
            const double base_d  = static_cast<double>(requested.at(base));
            const double quote_d = static_cast<double>(offered.at(quote));
            eval.price = (base_d > 0.0)
                ? static_cast<Mojo>(std::llround(quote_d / base_d
                    * static_cast<double>(kMojosPerXch)))
                : 0;
            matched = true;
            break;
        }
    }

    if (!matched || eval.price <= 0) {
        eval.disposition = OrphanDisposition::Unknown;
        eval.reason = matched ? "could not compute price"
                              : "no matching pair config for asset IDs";
        return eval;
    }

    // ---- Check orphan adoption is enabled ---------------------------------
    if (!strategy_cfg_.orphan_adopt_enabled) {
        eval.disposition = OrphanDisposition::Cancel;
        eval.reason = "orphan_adopt_enabled=false (legacy mode)";
        return eval;
    }

    // ---- Age check --------------------------------------------------------
    BlockHeight age = 0;
    if (current_block > 0 && trade_record.contains("created_at_time")) {
        // Approximate age from timestamps if created_at_time is available
        // but we don't have the creation block height directly.
        // The wallet trade record has "created_at_time" (epoch seconds).
        // Block time is ~52 seconds.
        const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        double created_at = 0.0;
        if (trade_record["created_at_time"].is_number()) {
            created_at = trade_record["created_at_time"].get<double>();
        }
        if (created_at > 0.0) {
            const double age_seconds = static_cast<double>(now_epoch) - created_at;
            age = static_cast<BlockHeight>(std::max(0.0, age_seconds / 52.0));
        }
    }

    if (age > strategy_cfg_.orphan_max_adopt_age_blocks) {
        eval.disposition = OrphanDisposition::Cancel;
        eval.reason = fmt::format("too old: ~{} blocks > max {}",
                                  age, strategy_cfg_.orphan_max_adopt_age_blocks);
        return eval;
    }

    // ---- Price deviation check --------------------------------------------
    auto mid_it = mid_prices.find(eval.pair_name);
    if (mid_it == mid_prices.end() || mid_it->second <= 0) {
        // No market data available for this pair -- can't evaluate.
        // Conservatively cancel.
        eval.disposition = OrphanDisposition::Cancel;
        eval.reason = "no current mid-price available for " + eval.pair_name;
        return eval;
    }

    const Mojo mid = mid_it->second;
    const double mid_d   = static_cast<double>(mid);
    const double price_d = static_cast<double>(eval.price);

    // Signed deviation: positive means our price is ABOVE mid.
    const double signed_dev = (mid_d > 0.0) ? (price_d - mid_d) / mid_d : 0.0;
    eval.price_deviation = std::abs(signed_dev);

    // Adverse direction:
    //   BID too high (signed_dev > 0) -> overpaying, adverse.
    //   ASK too low  (signed_dev < 0) -> underselling, adverse.
    eval.adverse = (eval.side == Side::Bid)
        ? (signed_dev > 0.0)
        : (signed_dev < 0.0);

    // ---- Inventory helper bonus -------------------------------------------
    // Check if this orphan would help reduce inventory imbalance.
    // If we're long the base asset and this is an ask (sell), it helps.
    // If we're short and this is a bid (buy), it helps.
    // We check via the state's position if available.
    double effective_threshold = strategy_cfg_.orphan_adverse_threshold;
    {
        auto pending = state_->get_all_offers();
        int bid_count = 0, ask_count = 0;
        for (const auto& po : pending) {
            if (po.pair_name == eval.pair_name) {
                if (po.side == Side::Bid) ++bid_count;
                else ++ask_count;
            }
        }
        // If we have more bids than asks, we're accumulating -> asks help.
        // If we have more asks than bids, we're depleting -> bids help.
        const bool helps_inventory =
            (bid_count > ask_count && eval.side == Side::Ask) ||
            (ask_count > bid_count && eval.side == Side::Bid);
        eval.inventory_helper = helps_inventory;
        if (helps_inventory) {
            effective_threshold += strategy_cfg_.orphan_inventory_bonus;
        }
    }

    // ---- Cost-aware cancel/keep decision ----------------------------------
    //
    // From Gao & Wang (2020), the optimal decision is:
    //   cancel if expected_adverse_loss > cancel_cost
    //
    // expected_adverse_loss = deviation x size x adverse_fill_prob
    // cancel_cost = fee + liquidity_opportunity_cost
    //
    // For simplicity and robustness, we use the threshold approach:
    //   - Non-adverse deviation (favorable): always adopt.
    //   - Adverse but within threshold: adopt (cost to cancel > likely loss).
    //   - Adverse beyond threshold: cancel (likely loss > cancel cost).
    //   - Mild adverse (between half-threshold and threshold): adopt-stale.
    eval.cancel_cost = to_mojo_saturating(current_fee_mojos_);

    if (!eval.adverse) {
        // Favorable deviation -- our offer is more conservative than
        // the current optimal.  Safe to keep: a bid below mid is fine,
        // an ask above mid is fine.
        eval.disposition = OrphanDisposition::Adopt;
        eval.reason = fmt::format("favorable: {} {:.2f}% from mid on {}",
                                  (eval.side == Side::Bid) ? "BID" : "ASK",
                                  eval.price_deviation * 100.0,
                                  eval.pair_name);
    } else if (eval.price_deviation <= effective_threshold * 0.5) {
        // Small adverse deviation -- well within tolerance.
        eval.disposition = OrphanDisposition::Adopt;
        eval.reason = fmt::format("small adverse {:.2f}% < {:.1f}% half-threshold",
                                  eval.price_deviation * 100.0,
                                  effective_threshold * 50.0);
    } else if (eval.price_deviation <= effective_threshold) {
        // Moderate adverse -- cheaper to keep than cancel, but flag
        // for early refresh on the next heartbeat.
        eval.disposition = OrphanDisposition::AdoptStale;
        eval.expected_loss = static_cast<Mojo>(std::llround(
            eval.price_deviation * static_cast<double>(eval.size)));
        eval.reason = fmt::format("moderate adverse {:.2f}% <= {:.1f}% threshold, "
                                  "adopt-stale{}",
                                  eval.price_deviation * 100.0,
                                  effective_threshold * 100.0,
                                  eval.inventory_helper ? " (inventory helper)" : "");
    } else {
        // Large adverse deviation -- expected loss exceeds cancel cost.
        eval.expected_loss = static_cast<Mojo>(std::llround(
            eval.price_deviation * static_cast<double>(eval.size)));
        eval.disposition = OrphanDisposition::Cancel;
        eval.reason = fmt::format("adverse {:.2f}% > {:.1f}% threshold, "
                                  "expected_loss={} > cancel_cost={}",
                                  eval.price_deviation * 100.0,
                                  effective_threshold * 100.0,
                                  eval.expected_loss, eval.cancel_cost);
    }

    return eval;
}

// ---------------------------------------------------------------------------
// startup_reconcile -- scan wallet for orphaned offers on startup
//
// Enhanced with cost-aware orphan evaluation (CAOE): instead of blindly
// cancelling all orphans, evaluates each one against the current market
// mid-price and adopts well-priced orphans to save fees and preserve
// market presence.
//
// Scholarly basis:
//   Gueant, Lehalle & Fernandez-Tapia (2013) -- cost-aware cancellation
//   Gao & Wang (2020) -- zero-offer gap avoidance for latent market makers
//   Ait-Sahalia & Saglam (2017) -- stale-quote risk scaling
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>> OfferManager::startup_reconcile(
    const std::unordered_set<std::string>& known_offer_ids,
    BlockHeight current_block)
{
    std::vector<std::string> cancelled_ids;

    // [S46] Describes THIS call only -- same contract as
    // last_terminal_offers_. Cleared before anything can populate it.
    db_leg_ = StartupDbLeg{};
    // [S14] Same contract.
    wallet_pending_cancel_.clear();

    logger_->info("[startup_reconcile] Scanning wallet for orphaned offers...");

    // -- Wallet sync pre-check -----------------------------------------------
    // If the wallet is not fully synced, get_all_offers may return
    // incomplete results and cancel_offer will fail with "Wallet needs to
    // be fully synced".  Check sync status and set a flag; if not synced,
    // force-adopt all orphans instead of attempting cancel (which would
    // fail and create uncancelled/untracked orphans locking XCH).
    bool wallet_synced = false;
    try {
        auto sync_status = co_await wallet_->get_sync_status();
        if (sync_status.contains("synced"))
            wallet_synced = sync_status["synced"].get<bool>();
        bool syncing = false;
        if (sync_status.contains("syncing"))
            syncing = sync_status["syncing"].get<bool>();
        if (syncing) wallet_synced = false;
        if (!wallet_synced) {
            logger_->warn("[startup_reconcile] Wallet NOT synced -- will "
                          "force-adopt all orphans instead of cancelling "
                          "to prevent uncancellable orphan deadlock");
        }
    } catch (const std::exception& e) {
        logger_->warn("[startup_reconcile] Wallet sync check failed: {} "
                      "-- assuming not synced, will force-adopt", e.what());
    }

    // ---- Phase 1: Collect all PENDING_ACCEPT offers from wallet -----------
    struct WalletOffer {
        std::string trade_id;
        json        record;
        bool        known;  // true if in our DB
    };
    std::vector<WalletOffer> wallet_offers;

    // [S46] Every trade id the scan MENTIONED, at any status -- not just the
    // PENDING_ACCEPT subset kept in wallet_offers. This is the set the DB leg
    // below subtracts, so a DB row the wallet answered about (in any state)
    // costs no extra probe, and only genuinely unmentioned rows are walked.
    std::unordered_set<std::string> scanned_ids;

    // [S14] PENDING_CANCEL records past kMaxStartupPendingCancelRecords.
    std::size_t pending_cancel_overflow = 0;

    constexpr std::int64_t kPageSize = 50;
    std::int64_t offset = 0;
    bool more = true;
    bool scan_complete = true;

    while (more) {
        std::vector<json> trade_records;
        try {
            trade_records = co_await wallet_->get_all_offers(
                offset, offset + kPageSize, /*file_contents=*/false,
                /*include_completed=*/false);
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("[startup_reconcile] get_all_offers failed: {}",
                           e.what());
            // [S46] Do NOT return here with the DB leg unwalked. A failed
            // scan is exactly the state in which DB rows are stranded, and
            // the old early return is what made "0 wallet offers scanned"
            // read as a clean bill of health. We learned nothing about any
            // DB-pending row, so every one of them is UNVERIFIABLE -- the
            // rows stay pending and the caller keeps asking on the
            // heartbeat.
            scan_complete = false;
            more = false;
            break;
        }

        if (trade_records.empty() ||
            static_cast<std::int64_t>(trade_records.size()) < kPageSize) {
            more = false;
        }

        for (auto& rec : trade_records) {
            if (!rec.contains("trade_id") || !rec.contains("status")) {
                continue;
            }
            std::string trade_id = rec["trade_id"].get<std::string>();
            int status = trade_status::parse(rec["status"]);

            scanned_ids.insert(trade_id);

            // [S14] PENDING_CANCEL records are kept too.  This scan used to
            // keep PENDING_ACCEPT only, so a trade whose cancel never landed
            // -- still takeable, every maker coin unspent -- was invisible to
            // the whole engine.  The decision is a pure function so a test
            // can pin it.
            const StartupScanBucket bucket = startup_scan_bucket(
                status, known_offer_ids.count(trade_id) > 0);
            if (bucket == StartupScanBucket::PendingCancelObserved) {
                if (wallet_pending_cancel_.size() < kMaxStartupPendingCancelRecords) {
                    wallet_pending_cancel_.push_back(
                        WalletPendingCancelRecord{trade_id, std::move(rec)});
                } else {
                    ++pending_cancel_overflow;
                }
                continue;
            }
            if (bucket == StartupScanBucket::Ignore) {
                continue;
            }

            wallet_offers.push_back(WalletOffer{
                trade_id,
                std::move(rec),
                bucket == StartupScanBucket::KnownLive
            });
        }

        offset += kPageSize;
    }

    if (!wallet_pending_cancel_.empty()) {
        logger_->info("[startup_reconcile] [S14] {} wallet trade(s) are "
                      "PENDING_CANCEL -- kept for boot recovery",
                      wallet_pending_cancel_.size());
    }
    if (pending_cancel_overflow > 0) {
        logger_->warn("[startup_reconcile] [S14] {} further PENDING_CANCEL "
                      "trade(s) beyond the cap of {} were NOT kept this boot",
                      pending_cancel_overflow, kMaxStartupPendingCancelRecords);
    }

    // ---- Phase 1b: THE DB -> WALLET LEG -----------------------------------
    // [S46 2026-09-02] The direction that did not exist. Everything above
    // asks "what does the wallet hold, and do we know about it?". A DB row
    // the wallet never mentions is not reached by that question at all.
    //
    // get_offer() is the only wallet call keyed by OUR id, and
    // reconcile_offers() already uses it with the right posture: an RPC
    // error keeps the offer tracked and retries, a CONFIRMED status keeps it
    // tracked so detect_fills books the fill, and only CANCELLED/FAILED is
    // treated as terminal. That posture is reused verbatim here rather than
    // written a second time.
    {
        std::size_t probes = 0;
        for (const auto& id : known_offer_ids) {
            if (scan_complete && scanned_ids.count(id) > 0) {
                continue;  // the wallet answered about it already
            }
            if (!scan_complete) {
                db_leg_.unverifiable.push_back(id);
                continue;
            }
            if (probes >= kMaxStartupDbProbes) {
                // Not "assume gone" -- assume NOTHING. The heartbeat sweep
                // is unbounded in cycles and is where a long walk belongs.
                db_leg_.unverifiable.push_back(id);
                continue;
            }
            ++probes;

            int status = -1;
            try {
                const json rec = co_await wallet_->get_offer(
                    id, /*file_contents=*/false);
                if (rec.contains("status")) {
                    status = trade_status::parse(rec["status"]);
                }
            } catch (const rpc::ChiaRPCError& e) {
                // Fail safe. An unsynced wallet refuses here exactly as it
                // refused the cancel on the way down, and treating that as
                // "gone" would be the same fail-open one process later.
                logger_->warn("[startup_reconcile] get_offer failed for {} -- "
                              "row stays pending, heartbeat will retry: {}",
                              id.substr(0, 12), e.what());
                db_leg_.unverifiable.push_back(id);
                continue;
            }

            // [FILL-PROOF, review #171 round 13] CANCELLED is not terminal
            // here: a cancel writes it, over a real take too, so detect_fills
            // proves the offer on-chain before the row is closed.
            if (status == trade_status::kFailed) {
                db_leg_.terminal.push_back(id);
            } else if (status == trade_status::kCancelled) {
                db_leg_.cancelled_unproven.push_back(id);
            } else if (status == trade_status::kConfirmed) {
                db_leg_.confirmed.push_back(id);
            } else if (status == trade_status::kPendingAccept) {
                db_leg_.still_live.push_back(id);
            } else {
                // kPendingConfirm, kPendingCancel, or an unrecognised code.
                // A recognised pending state is not terminal and an
                // unrecognised one is no evidence at all; neither authorises
                // stamping the row.
                db_leg_.unverifiable.push_back(id);
            }
        }

        if (db_leg_.total() > 0) {
            logger_->warn("[startup_reconcile] DB->wallet leg: {} DB-pending "
                          "row(s) absent from the wallet scan -- {} terminal, "
                          "{} cancelled (proven before they are closed), "
                          "{} still live, {} confirmed/filling, {} "
                          "UNVERIFIABLE (kept pending for the heartbeat)",
                          db_leg_.total(), db_leg_.terminal.size(),
                          db_leg_.cancelled_unproven.size(),
                          db_leg_.still_live.size(),
                          db_leg_.confirmed.size(),
                          db_leg_.unverifiable.size());
        }
    }

    if (!scan_complete) {
        co_return cancelled_ids;
    }

    // Separate known from orphans.
    std::size_t total_pending = wallet_offers.size();
    std::vector<WalletOffer*> orphans;
    std::size_t restored = 0;
    for (auto& wo : wallet_offers) {
        if (wo.known) {
            ++restored;
        } else {
            orphans.push_back(&wo);
        }
    }

    if (orphans.empty()) {
        // [S46] The DB-leg counts are on this line deliberately. Its previous
        // wording -- "0 wallet offers scanned, 0 known/restored, 0 orphans" --
        // is what the engine logged on 2026-09-02 with seven stranded DB rows,
        // and an operator reading it had no way to tell a genuinely empty book
        // from a wallet that answered nothing.
        logger_->info("[startup_reconcile] Complete: {} wallet offers scanned, "
                      "{} known/restored, 0 orphans; DB-leg {} probed "
                      "({} terminal, {} live, {} unverifiable)",
                      total_pending, restored, db_leg_.total(),
                      db_leg_.terminal.size(), db_leg_.still_live.size(),
                      db_leg_.unverifiable.size());
        co_return cancelled_ids;
    }

    // ---- Phase 2: Fetch mid-prices for orphan evaluation ------------------
    // Build the set of pairs that orphans might belong to, then fetch
    // current dexie ticker prices for cost-aware evaluation.
    std::unordered_map<std::string, Mojo> mid_prices;

    if (strategy_cfg_.orphan_adopt_enabled && dexie_client_) {
        // Fetch ticker for each enabled pair.
        for (const auto& [pname, pcfg] : pair_config_map_) {
            if (!pcfg.enabled) continue;
            try {
                auto ticker = co_await dexie_client_->get_ticker(
                    pcfg.base_asset_id, pcfg.quote_asset_id);
                if (ticker.has_value()) {
                    // Mid = (best_bid + best_ask) / 2, in XCH mojos.
                    const double mid = (ticker->best_bid + ticker->best_ask) / 2.0;
                    if (mid > 0.0) {
                        mid_prices[pname] = static_cast<Mojo>(std::llround(
                            mid * static_cast<double>(kMojosPerXch)));
                    }
                }
            } catch (const std::exception& e) {
                logger_->warn("[startup_reconcile] Failed to fetch ticker for "
                              "{}: {}", pname, e.what());
            }
        }
        logger_->info("[startup_reconcile] Fetched mid-prices for {} pairs",
                      mid_prices.size());
    }

    // ---- Phase 3: Evaluate each orphan ------------------------------------
    std::size_t adopted = 0, adopted_stale = 0, cancelled = 0, unknown = 0;

    for (auto* wo : orphans) {
        OrphanEvaluation eval = evaluate_orphan(
            wo->record, current_block, mid_prices);

        switch (eval.disposition) {
            case OrphanDisposition::Adopt:
            case OrphanDisposition::AdoptStale: {
                // Re-register this orphan as a tracked PendingOffer.
                PendingOffer po;
                po.offer_id        = wo->trade_id;
                po.pair_name       = eval.pair_name;
                po.side            = eval.side;
                po.price           = eval.price;
                po.size            = eval.size;
                po.tier            = 0;  // Unknown original tier -- assign 0.
                po.fee_mojos       = current_fee_mojos_;
                // Approximate creation block from age.
                if (current_block > 0 && wo->record.contains("created_at_time")) {
                    const auto now_epoch = std::chrono::duration_cast<
                        std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                    double created_at = 0.0;
                    if (wo->record["created_at_time"].is_number()) {
                        created_at = wo->record["created_at_time"].get<double>();
                    }
                    if (created_at > 0.0) {
                        const BlockHeight approx_age = static_cast<BlockHeight>(
                            std::max(0.0,
                                (static_cast<double>(now_epoch) - created_at)
                                    / 52.0));
                        po.created_at_block = (current_block > approx_age)
                            ? (current_block - approx_age) : 0;
                    }
                }
                // For AdoptStale, assign a creation block that makes it
                // look older (closer to TTL), triggering early refresh.
                if (eval.disposition == OrphanDisposition::AdoptStale) {
                    // Set created_at_block to (current - ttl + 5) so it
                    // will be classified as Expired on next heartbeat,
                    // triggering an immediate selective refresh.
                    const BlockHeight ttl = strategy_cfg_.offer_ttl_blocks;
                    if (current_block > ttl && ttl > 5) {
                        po.created_at_block = current_block - ttl + 5;
                    }
                }
                state_->upsert_offer(po);

                if (eval.disposition == OrphanDisposition::Adopt) {
                    ++adopted;
                    logger_->info("[startup_reconcile] ADOPTED orphan {} "
                                  "({} {} on {} @ {} mojos) -- {}",
                                  wo->trade_id.substr(0, 24),
                                  (eval.side == Side::Bid) ? "BID" : "ASK",
                                  eval.size, eval.pair_name, eval.price,
                                  eval.reason);
                } else {
                    ++adopted_stale;
                    logger_->info("[startup_reconcile] ADOPTED-STALE orphan {} "
                                  "({} {} on {} @ {} mojos) -- {}",
                                  wo->trade_id.substr(0, 24),
                                  (eval.side == Side::Bid) ? "BID" : "ASK",
                                  eval.size, eval.pair_name, eval.price,
                                  eval.reason);
                }
                break;
            }

            case OrphanDisposition::Cancel:
            case OrphanDisposition::Unknown: {
                // Cancel this orphan on-chain.
                const char* label = (eval.disposition == OrphanDisposition::Unknown)
                    ? "UNKNOWN" : "CANCEL";
                logger_->warn("[startup_reconcile] {} orphan {} -- {}",
                              label, wo->trade_id.substr(0, 24), eval.reason);

                bool cancel_ok = false;

                // Skip cancel attempt entirely when wallet is not synced.
                // cancel_offer will fail with "Wallet needs to be fully
                // synced" and emergency_cancel's multiple retries will
                // also fail, wasting time.  Go straight to force-adopt.
                if (!wallet_synced) {
                    logger_->warn("[startup_reconcile] Skipping cancel for {} "
                                  "-- wallet not synced, will force-adopt",
                                  wo->trade_id.substr(0, 24));
                } else {
                bool needs_emergency = false;
                try {
                    co_await cancel_offer_charged(wo->trade_id,
                                                   cancel_fee_for(wo->trade_id),
                                                   /*secure=*/true);
                    cancel_ok = true;
                } catch (const rpc::ChiaRPCError& e) {
                    const std::string_view msg{e.what()};
                    needs_emergency =
                        msg.find("insufficient funds") != std::string_view::npos ||
                        msg.find("spendable balance") != std::string_view::npos;
                    if (!needs_emergency) {
                        logger_->error("[startup_reconcile] Failed to cancel "
                                       "{}: {}", wo->trade_id.substr(0, 24),
                                       e.what());
                    }
                } catch (const std::exception& e) {
                    logger_->error("[startup_reconcile] Failed to cancel "
                                   "{}: {}", wo->trade_id.substr(0, 24),
                                   e.what());
                }
                if (needs_emergency)
                    cancel_ok = co_await emergency_cancel(
                        wo->trade_id, "startup_reconcile");
                } // end wallet_synced
                if (cancel_ok) {
                    // [S14] An accepted cancel is a SUBMISSION.  The orphan
                    // stays takeable until the spend confirms, and a take on
                    // an untracked trade is booked nowhere -- so track it,
                    // flagged cancel_pending, until the wallet verdict (or
                    // the escalation) resolves it.
                    adopt_wallet_record(wo->trade_id, wo->record, current_block);
                    state_->mark_cancel_pending(wo->trade_id);
                    cancelled_ids.push_back(wo->trade_id);
                    if (eval.disposition == OrphanDisposition::Cancel)
                        ++cancelled;
                    else
                        ++unknown;
                } else {
                    // Cancel failed (insufficient XCH, wallet not synced,
                    // etc.).  Force-adopt to prevent deadlock: the wallet
                    // still holds this offer as PENDING_ACCEPT, locking
                    // coins.  If we don't track it, the engine sees 0
                    // pending offers but 0 spendable XCH -- permanent
                    // stall.  Adopting lets UTXO liberation or the next
                    // cancel cycle free the locked coins.
                    adopt_wallet_record(wo->trade_id, wo->record, current_block);
                    ++adopted;
                    logger_->warn(
                        "[startup_reconcile] FORCE-ADOPTED uncancellable "
                        "orphan {} ({} {} on {}) -- cancel failed, tracking "
                        "to prevent deadlock",
                        wo->trade_id.substr(0, 24),
                        (eval.side == Side::Bid) ? "BID" : "ASK",
                        eval.size,
                        eval.pair_name.empty() ? "UNKNOWN" : eval.pair_name);
                }
                break;
            }
        }
    }

    logger_->info("[startup_reconcile] Complete: {} wallet offers, "
                  "{} known/restored, {} adopted, {} adopted-stale, "
                  "{} cancelled, {} unknown-cancelled",
                  total_pending, restored, adopted, adopted_stale,
                  cancelled, unknown);

    co_return cancelled_ids;
}

// ---------------------------------------------------------------------------
// prune_stuck_transactions -- detect and clear stuck wallet transactions
// ---------------------------------------------------------------------------

asio::awaitable<int> OfferManager::prune_stuck_transactions(
    const std::vector<std::int64_t>& wallet_ids,
    std::int64_t max_age_seconds)
{
    int wallets_pruned = 0;
    const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (const auto wid : wallet_ids) {
        try {
            auto txs = co_await wallet_->get_transactions(wid, 0, 200);

            int stuck_count = 0;
            int fresh_count = 0;
            for (const auto& tx : txs) {
                const bool confirmed =
                    tx.contains("confirmed") && tx["confirmed"].get<bool>();
                const bool age_known = tx.contains("created_at_time");
                const std::int64_t age =
                    age_known
                        ? (now_epoch - tx["created_at_time"].get<std::int64_t>())
                        : 0;
                const bool has_bundle = tx.contains("spend_bundle") &&
                                        !tx["spend_bundle"].is_null();

                // [review 2026-09-13] The per-row rule -- unknown age counts
                // as FRESH, and a row carrying a spend bundle gets 3x the
                // threshold -- lives in stuck_tx_verdict.hpp so ctest drives
                // the same code this does rather than a copy of it. Only the
                // JSON field reading above is still uncovered.
                const auto row_class = classify_stuck_row(
                    confirmed, age_known, age, has_bundle, max_age_seconds);

                if (row_class == StuckRowClass::Confirmed) {
                    continue;
                }
                // [S67] The wallet's own word that a fee is too low: some peer
                // refused this row on the fee and none accepted it.  Already
                // in hand -- no RPC is added -- and counted once per
                // transaction name until the set is cleared (see
                // kMaxReportedFeeRejections; "once" is a normal case, not an
                // invariant).  It changes nothing this function decides.
                // [review #163 r9] The whole sent_to array is read, not its
                // last entry: the array is per PEER, not a timeline.
                if (execution::sent_to_reports_fee_rejection(tx)) {
                    const std::string tx_key = (tx.contains("name") && tx["name"].is_string())
                        ? tx["name"].get<std::string>() : std::string{};
                    if (fee_rejections_reported_.size() >= execution::kMaxReportedFeeRejections) {
                        fee_rejections_reported_.clear();
                    }
                    if (!tx_key.empty() && fee_rejections_reported_.insert(tx_key).second
                        && fee_rejections_seen_ != std::numeric_limits<std::uint32_t>::max()) {
                        ++fee_rejections_seen_;
                    }
                }
                if (row_class == StuckRowClass::FreshOrUnknown) {
                    ++fresh_count;
                    continue;
                }

                ++stuck_count;

                // Log details for the first few stuck transactions.
                if (stuck_count <= 5) {
                    std::int64_t amount = 0;
                    int tx_type = -1;
                    std::string tx_name = "(unknown)";
                    if (tx.contains("amount")) amount = tx["amount"].get<std::int64_t>();
                    if (tx.contains("type"))   tx_type = tx["type"].get<int>();
                    if (tx.contains("name"))   tx_name = tx["name"].get<std::string>();
                    logger_->warn("[prune_stuck_tx] wallet {} stuck tx: "
                                  "type={} amount={} age={}s bundle={} "
                                  "name={}",
                                  wid, tx_type, amount, age,
                                  has_bundle ? "yes" : "no", tx_name);
                }
            }

            // [S33 review] delete_unconfirmed_transactions is WALLET-WIDE, so
            // one old row must not authorise deleting this heartbeat's offer
            // creations and unconfirmed cancel spends beside it.  The decision
            // lives in execution/stuck_tx_verdict.hpp so ctest drives the same
            // code this does; THIS CALL SITE IS NOT COVERED -- nothing in
            // cpp/tests constructs an OfferManager.
            if (stuck_count > 0 && fresh_count > 0) {
                logger_->info("[prune_stuck_tx] wallet {} has {} stuck "
                              "transaction(s) but {} fresh/unknown-age "
                              "unconfirmed row(s) -- HOLDING the wallet-wide "
                              "delete; it cannot be aimed",
                              wid, stuck_count, fresh_count);
            }
            if (authorises_wallet_wide_delete(/*past_threshold=*/stuck_count,
                                              /*fresh_or_unknown=*/fresh_count)) {
                // [S33 review] Text corrected: the 3x branch above also counts
                // rows that DO carry a spend bundle, and reverse=false makes
                // that branch reachable for the first time, so the old
                // "(no spend bundle, ...)" wording would now be printed while
                // deleting on the strength of bundled rows.
                logger_->warn("[prune_stuck_tx] wallet {} has {} unconfirmed "
                              "transaction(s) past their stuck threshold "
                              "(base {}s; 3x that when a spend bundle exists) "
                              "and none fresher -- clearing unconfirmed",
                              wid, stuck_count, max_age_seconds);
                co_await wallet_->delete_unconfirmed_transactions(wid);
                ++wallets_pruned;
                logger_->info("[prune_stuck_tx] wallet {} unconfirmed "
                              "transactions cleared", wid);
            }
        } catch (const std::exception& e) {
            logger_->error("[prune_stuck_tx] wallet {} failed: {}",
                           wid, e.what());
        }
    }

    if (wallets_pruned > 0) {
        logger_->info("[prune_stuck_tx] Pruned stuck transactions from "
                      "{} wallet(s)", wallets_pruned);
    }

    co_return wallets_pruned;
}

// ---------------------------------------------------------------------------
// build_offer_dict -- map a TierQuote to the wallet RPC offer_dict format
// ---------------------------------------------------------------------------

json OfferManager::build_offer_dict(const PairConfig& pair,
                                    const TierQuote&  tier) const
{
    // Resolve both asset IDs to wallet IDs.
    const std::int64_t base_wid  = resolve_wallet_id(pair.base_asset_id);
    const std::int64_t quote_wid = resolve_wallet_id(pair.quote_asset_id);

    if (base_wid < 0 || quote_wid < 0) {
        logger_->error("Cannot resolve wallet IDs: base='{}' (wid={}), "
                       "quote='{}' (wid={})",
                       pair.base_asset_id, base_wid,
                       pair.quote_asset_id, quote_wid);
        return json::object();
    }

    // Resolve the per-asset mojo denomination from the pair configuration.
    // XCH uses 10^12 mojos per unit; CAT tokens use 10^3 mojos per unit.
    //
    // ISO/IEC 5055: guard against zero/negative denomination to prevent
    // division-by-zero undefined behaviour or nonsensical mojo amounts.
    const std::int64_t quote_denom = pair.quote_mojos_per_unit;
    const std::int64_t base_denom  = pair.base_mojos_per_unit;
    if (quote_denom <= 0 || base_denom <= 0) {
        logger_->error("build_offer_dict: invalid mojos_per_unit "
                       "(base={}, quote={}) for pair '{}' -- must be > 0",
                       base_denom, quote_denom, pair.name);
        return json::object();
    }

    // tier.size  is in base-asset mojos.
    // tier.price is the exchange rate (quote-per-base units) scaled by
    //            kMojosPerXch (the engine multiplies the strategy's
    //            floating-point price by kMojosPerXch for fixed-point
    //            storage in the Quote struct).
    //
    // Canonical formula in xop::quote_mojos_for (types.hpp):
    //   quote_mojos = tier.size * tier.price * quote_denom
    //               / (base_denom * kMojosPerXch)
    //
    // We compute in double to avoid int64 overflow (the numerator can
    // reach ~10^25 for typical XCH/CAT pairs).
    const double size_d  = static_cast<double>(tier.size);
    const double price_d = static_cast<double>(tier.price);
    const double base_d  = static_cast<double>(base_denom);
    const double quote_d = static_cast<double>(quote_denom);

    json offer_dict = json::object();

    if (tier.side == Side::Bid) {
        // BID: we offer quote-asset mojos (negative), request base-asset
        // mojos (positive).
        // Round up (ceil) so that we offer at least enough quote to cover
        // the requested base amount at the stated price.
        const Mojo quote_amount = static_cast<Mojo>(
            std::ceil(quote_mojos_for(size_d, price_d, base_d, quote_d)));

        // Wallet RPC convention: negative = we spend, positive = we receive.
        offer_dict[std::to_string(quote_wid)] = -quote_amount;
        offer_dict[std::to_string(base_wid)]  =  tier.size;

    } else {
        // ASK: we offer base-asset mojos (negative), request quote-asset
        // mojos (positive).
        // Round down (floor) so that we request conservatively, ensuring
        // the offer is attractive to takers.
        const Mojo quote_amount = static_cast<Mojo>(
            std::floor(quote_mojos_for(size_d, price_d, base_d, quote_d)));

        offer_dict[std::to_string(base_wid)]  = -tier.size;
        offer_dict[std::to_string(quote_wid)] =  quote_amount;
    }

    return offer_dict;
}

// ---------------------------------------------------------------------------
// [T7-10] post_merged_side -- merge same-side tiers into one RPC call
// ---------------------------------------------------------------------------

asio::awaitable<void> OfferManager::begin_xch_lock_cycle()
{
    // Reset first: a stale ledger from the previous cycle must never gate
    // this one, and an RPC failure below leaves the ledger inactive.
    xch_cycle_ledger_ = CoinLockLedger{};
    xch_ledger_refusal_logged_ = false;
    try {
        auto coins = co_await wallet_->get_spendable_coins(1);
        // Parsed by the same helper the unit tests exercise, so schema
        // handling cannot silently diverge from the tested path (review).
        std::vector<Mojo> amounts = spendable_amounts_from_coin_records(coins);
        const auto floor = static_cast<Mojo>(std::llround(
            strategy_cfg_.fee_reserve_xch
            * static_cast<double>(kMojosPerXch)));
        xch_cycle_ledger_ = CoinLockLedger(
            std::move(amounts), floor, strategy_cfg_.xch_cycle_commit_frac);
        logger_->debug("XCH lock ledger: {} free coins, {} mojos free, "
                       "floor {} mojos, commit_frac {:.2f}",
                       coins.size(), xch_cycle_ledger_.remaining(), floor,
                       strategy_cfg_.xch_cycle_commit_frac);
    } catch (const std::exception& e) {
        logger_->warn("XCH lock ledger snapshot failed ({}) -- this cycle "
                      "runs on the wallet-requery guards only", e.what());
    }
}

std::string OfferManager::late_trade_id(const json& result,
                                       const std::string& what)
{
    if (result.contains("trade_record")
        && result["trade_record"].contains("trade_id")
        && result["trade_record"]["trade_id"].is_string()) {
        auto id = result["trade_record"]["trade_id"].get<std::string>();
        if (!id.empty()) return id;
    }
    logger_->critical("a {} offer was created after the stop and its trade "
                      "id is missing or not a string -- it cannot be "
                      "cancelled individually and is LIVE and unmanaged",
                      what);
    if (escalate_) {
        escalate_("an offer created after the stop (" + what + ") is LIVE "
                  "and CANNOT be cancelled: the wallet returned no usable "
                  "trade id. The bulk cancel never saw it, so any earlier "
                  "cancellation-submitted message does not cover this one. "
                  "The book must be inspected by hand.");
    }
    return {};
}

asio::awaitable<json> OfferManager::cancel_offer_charged(
    const std::string& trade_id, std::uint64_t fee, bool secure)
{
    // [FILL-PROOF, review #171] Not an offer the wallet reports CONFIRMED
    // that the fill proof has not settled.  Chia 2.7.4's secure cancel sets
    // PENDING_CANCEL over whatever status the trade has, and an insecure one
    // sets CANCELLED, so a cancel sent while a lookup failed or the node lagged
    // would erase the CONFIRMED the proof waits on -- and a real take with it.
    // [round 2] Unless the node has proven it live again, at depth
    // (execution::live_offer_cancellable).  Refused like a wallet refusal,
    // which every caller already handles.
    if (cancel_withheld_for_proof(trade_id)) {
        throw rpc::ChiaRPCError("cancel withheld: the wallet reports "
                                + trade_id.substr(0, 12)
                                + " CONFIRMED and the fill proof has not settled it");
    }
    // [review #163 r6] to_mojo_saturating, not a bare cast: `fee` is a
    // std::uint64_t and a wrapped negative is silently zeroed by
    // CoinLockLedger::clamp_need(), which would leave the pool believing this
    // cancel locks nothing.  See xop::to_mojo_saturating (types.hpp).
    xch_cycle_ledger_.note_lock(0, to_mojo_saturating(fee));
    json reply = co_await wallet_->cancel_offer(trade_id, fee, secure);
    // [review #163] The wallet ACCEPTED it (a refusal throws past this line).
    // Tell the fee controller what was really paid, now -- see
    // set_cancel_observer.  A local-only cancel spends nothing on chain.
    if (secure) {
        // [review #163 r9] THE SAME LINE IS ALSO THE ONLY HONEST INPUT TO THE
        // ROLLING FEE WINDOW.  Engine::cancel_fees_paid used to re-derive each
        // cancel's fee from cancel_fee_for(), a FRESH POLICY LOOKUP -- so a
        // cancel that fell through to emergency_cancel and went out at a
        // halved tier, at a secure fee of 0, or as a local-only cancel that
        // spends nothing was still booked at the full policy fee.  This
        // accumulates what the wallet ACCEPTED instead; an insecure cancel
        // contributes nothing because it commits nothing on chain.
        cancel_fees_accepted_ = (fee > std::numeric_limits<std::uint64_t>::max()
                                           - cancel_fees_accepted_)
                                    ? std::numeric_limits<std::uint64_t>::max()
                                    : cancel_fees_accepted_ + fee;
        if (cancel_observer_) {
            cancel_observer_(trade_id, fee);
        }
    }
    co_return reply;
}

// ---------------------------------------------------------------------------
// [S14 2026-09-13] The cancel escalation's wallet write and boot adoption
// ---------------------------------------------------------------------------

asio::awaitable<std::optional<std::string>> OfferManager::recancel_secure(
    const std::string& trade_id, std::uint64_t fee)
{
    std::optional<std::string> error;
    try {
        co_await cancel_offer_charged(trade_id, fee, /*secure=*/true);
    } catch (const rpc::ChiaRPCError& e) {
        error = std::string{e.what()};
    }
    co_return error;
}

bool OfferManager::adopt_wallet_record(const std::string& trade_id,
                                       const json&        record,
                                       BlockHeight        current_block)
{
    auto parsed = try_parse_wallet_offer(record, current_block);
    if (parsed) {
        state_->upsert_offer(*parsed);
        return true;
    }
    PendingOffer po;
    po.offer_id        = trade_id;
    po.pair_name       = "UNKNOWN";
    po.side            = Side::Bid;
    po.price           = 0;
    po.size            = 0;
    po.tier            = 0;
    po.fee_mojos       = 0;
    po.created_at_block = 0;
    state_->upsert_offer(po);
    return false;
}

void OfferManager::adopt_wallet_pending_cancel(
    const WalletPendingCancelRecord& pending, BlockHeight current_block)
{
    if (state_->get_offer(pending.trade_id).offer_id.empty()) {
        adopt_wallet_record(pending.trade_id, pending.record, current_block);
    }
    state_->mark_cancel_pending(pending.trade_id);
}

asio::awaitable<json> OfferManager::cancel_offers_charged(
    std::uint64_t fee, bool secure, std::int64_t n_offers)
{
    // [BULKCANCEL 2026-09-11] batch_fee is charged PER BATCH, so reserve one
    // whole fee coin per batch rather than one for the whole call.
    // [S33 2026-09-11] The arithmetic, the >= 1 clamp and the rationale live
    // in execution/coin_lock_ledger.hpp so that ctest drives the same code
    // this does -- inline here, the reservation had no coverage at all.
    reserve_bulk_cancel(xch_cycle_ledger_, to_mojo_saturating(fee), n_offers);
    co_return co_await wallet_->cancel_offers(fee, secure);
}

Mojo OfferManager::xch_principal_from_offer_dict(const json& offer_dict)
{
    if (!offer_dict.contains("1")) {
        return 0;
    }
    const auto v = offer_dict["1"].get<std::int64_t>();
    return v < 0 ? static_cast<Mojo>(-v) : Mojo{0};
}

std::vector<Mojo> OfferManager::spendable_amounts_from_coin_records(
    const std::vector<json>& records)
{
    std::vector<Mojo> amounts;
    amounts.reserve(records.size());
    for (const auto& cr : records) {
        const auto& coin = cr.contains("coin") ? cr["coin"] : cr;
        if (coin.contains("amount")) {
            amounts.push_back(coin["amount"].get<Mojo>());
        }
    }
    return amounts;
}

OfferManager::PreflightDrops OfferManager::preflight_side_drops(
    bool any_bid, int admit_bid, bool any_ask, int admit_ask,
    bool bids_buy_xch, bool asks_buy_xch)
{
    PreflightDrops drops;
    const bool asks_doomed = any_ask && admit_ask == 0 && admit_bid > 0;
    const bool bids_doomed = any_bid && admit_bid == 0 && admit_ask > 0;
    if (asks_doomed) {
        drops.drop_asks = true;
        if (!bids_buy_xch) {
            drops.drop_bids = true;
        }
    } else if (bids_doomed) {
        drops.drop_bids = true;
        if (!asks_buy_xch) {
            drops.drop_asks = true;
        }
    }
    return drops;
}

bool OfferManager::xch_ledger_probe_admits(CoinLockLedger&   probe,
                                           const json&       offer_dict,
                                           const PairConfig& pair,
                                           Side              side) const
{
    if (!probe.active()) {
        return true;
    }
    const bool buys_xch =
        (side == Side::Bid && pair.base_asset_id == "xch")
        || (side == Side::Ask && pair.quote_asset_id == "xch");
    // [MIN-INPUT-COIN review #162] Same floor as the real admission, or the
    // preflight would keep a side the cycle ledger then refuses.
    // [review #163 r6] These two were IMPLICIT uint64 -> Mojo narrowings: the
    // sinks take a Mojo and no diagnostic fires.  See xch_ledger_admits below
    // and xop::to_mojo_saturating (types.hpp).
    const Mojo min_coin = ledger_min_coin_mojos(
        offer_dict, strategy_cfg_.offer_min_input_coin_frac);
    if (buys_xch) {
        return probe.try_lock_floor_only(
            0, to_mojo_saturating(current_fee_mojos_), min_coin);
    }
    return probe.try_lock(xch_principal_from_offer_dict(offer_dict),
                          to_mojo_saturating(current_fee_mojos_), min_coin);
}

bool OfferManager::xch_ledger_admits(const json&       offer_dict,
                                     const PairConfig& pair,
                                     Side              side,
                                     const char*       context)
{
    if (!xch_cycle_ledger_.active()) {
        return true;
    }
    // Buy-XCH offers are CAP-exempt but never FLOOR-exempt (review round
    // 3).  Cap-exempt mirrors the pre-existing recovery-zone and
    // xch_buy_only_mode escapes: buy-XCH offers net-increase XCH when
    // filled, and vetoing them on the spend budget re-created the low-XCH
    // starvation deadlock those paths exist to prevent.  Floor-checked
    // because an unconditional bypass re-opened the incident from the
    // other side: fee-coin locks alone could drain the pool to zero.
    // Below the floor, the remaining escapes are the ones that spend no
    // XCH at all: resting-offer fills, TTL expiries, and the xch_recovery
    // taker (which pays wUSDC.b).
    const bool buys_xch =
        (side == Side::Bid && pair.base_asset_id == "xch")
        || (side == Side::Ask && pair.quote_asset_id == "xch");
    // [MIN-INPUT-COIN review #162] The floor create_offer_min_coin will send
    // for THIS offer_dict -- per-tier, merged batch and batch fallback alike
    // -- which the wallet also applies to the XCH fee coin.  0 for an
    // XCH-funded offer, so those admissions are unchanged.
    const Mojo min_coin = ledger_min_coin_mojos(
        offer_dict, strategy_cfg_.offer_min_input_coin_frac);
    if (buys_xch) {
        if (xch_cycle_ledger_.try_lock_floor_only(
                0, to_mojo_saturating(current_fee_mojos_), min_coin)) {
            return true;
        }
        xch_ledger_suppressed_ = true;
        if (!xch_ledger_refusal_logged_) {
            logger_->warn("XCH lock ledger: refusing buy-XCH {} for {} -- "
                          "even a fee-coin lock would breach the reserve "
                          "floor (remaining={})",
                          context, pair.name, xch_cycle_ledger_.remaining());
            xch_ledger_refusal_logged_ = true;
        }
        return false;
    }
    const Mojo principal = xch_principal_from_offer_dict(offer_dict);
    if (xch_cycle_ledger_.try_lock(
            principal, to_mojo_saturating(current_fee_mojos_), min_coin)) {
        return true;
    }
    xch_ledger_suppressed_ = true;
    if (!xch_ledger_refusal_logged_) {
        logger_->warn(
            "XCH lock ledger: refusing {} for {} (principal={} fee={} "
            "committed={} remaining={}) -- whole-coin locking would breach "
            "the floor or cycle cap; further refusals this cycle at debug",
            context, pair.name, principal, current_fee_mojos_,
            xch_cycle_ledger_.committed(), xch_cycle_ledger_.remaining());
        xch_ledger_refusal_logged_ = true;
    } else {
        logger_->debug("XCH lock ledger: refusing {} for {} "
                       "(principal={} fee={})",
                       context, pair.name, principal, current_fee_mojos_);
    }
    return false;
}

asio::awaitable<int> OfferManager::post_merged_side(
    const PairConfig&              pair,
    const std::vector<TierQuote>&  tiers,
    BlockHeight                    block_height)
{
    if (tiers.empty()) co_return 0;

    // Build individual offer_dicts and merge by summing wallet_id amounts.
    json merged_dict = json::object();
    for (const auto& tier : tiers) {
        json single = build_offer_dict(pair, tier);
        if (single.empty()) {
            logger_->warn("Batch: skipping tier {} {} -- could not build dict",
                          tier.tier_index, to_string(tier.side));
            continue;
        }
        for (auto& [key, val] : single.items()) {
            if (merged_dict.contains(key)) {
                merged_dict[key] = merged_dict[key].get<std::int64_t>()
                                 + val.get<std::int64_t>();
            } else {
                merged_dict[key] = val;
            }
        }
    }

    if (merged_dict.empty()) co_return 0;

    // [S31] Same check on the batch path. One merged offer is still an offer,
    // and this function is reached after several awaits.
    if (abort_predicate_ && abort_predicate_()) {
        logger_->error("aborting merged batch creation for {}: the engine "
                       "asked us to stop", pair.name);
        co_return 0;
    }

    // [S74 / review #165, round 4] ...and the same on the batch path: a stop
    // is latched, so start no create. Cancels nothing (see post_quotes).
    if (stop_creating_predicate_ && stop_creating_predicate_()) {
        logger_->warn("not creating the merged {} batch: a stop is latched. "
                      "Nothing already created is cancelled.", pair.name);
        co_return 0;
    }

    // [XCH-LOCK-LEDGER] One merged offer, one lock: the merged dict's XCH
    // leg is the sum of every tier's, so the ledger charge is exact.
    if (!xch_ledger_admits(merged_dict, pair, tiers.front().side,
                           "merged batch")) {
        co_return 0;
    }

    // Create the merged offer via RPC.
    // co_await cannot appear inside a catch handler in a C++20 coroutine,
    // so capture any exception info here and perform the fallback after the
    // try/catch block.
    // [OFFER-EXPIRY] One merged offer, one timelock: the merged spend is a
    // single offer and carries a single max_time.
    const std::optional<std::uint64_t> expiry_max_time =
        expiry_max_time_for(pair);
    json result;
    bool batch_failed = false;
    bool batch_uncertain = false;
    std::string batch_err;
    // [MIN-INPUT-COIN] The merged dict still has one spend leg, so the floor
    // scales with the merged amount.
    // [S74 / review #165] Held until the merged offer is in State (or until
    // this call gives up on it): a keep stop waits for exactly this window.
    PostingMark batch_mark{posting_in_flight_flag_};
    try {
        result = co_await create_offer_min_coin(
            merged_dict, expiry_max_time, pair, tiers.front().side,
            static_cast<int>(tiers.front().tier_index), "merged batch");
    } catch (const rpc::ChiaRPCTransportError& e) {
        // [MERGE #162 x #165] ONE handler, not two. ChiaRPCTransportError
        // derives from ChiaRPCError, so keeping both PRs' handlers on the same
        // try is an unreachable duplicate (MSVC C2312, GCC -Wexceptions) --
        // and both effects are wanted. Still BEFORE the base handler, which
        // would otherwise take this too.
        //   * #162: suppress the per-tier fallback, which would otherwise
        //     rebuild the batch on top of a merged offer the wallet holds;
        //   * #165: record the uncertainty, so a keep stop stops reporting a
        //     clean book (the PostingMark comment has the whole argument).
        batch_failed = true;
        batch_err = e.what();
        batch_uncertain =
            rpc::create_possibly_submitted(e.curl_code(), e.http_code());
        note_create_outcome_unknown(e, pair.name, "merged batch");
    } catch (const rpc::ChiaRPCError& e) {
        batch_failed = true;
        batch_err = e.what();
    }

    // [MIN-INPUT-COIN review #162, round 2] No answer is not a refusal.  The
    // merged offer may already rest in the wallet, and creating the tiers
    // individually would duplicate it tier by tier.  Post nothing more for
    // this side this cycle; a refusal, or a failure before the request was
    // written, still falls back below.
    if (batch_failed && batch_uncertain) {
        logger_->error("Batch create_offer for {} {} got NO ANSWER ({}) -- "
                       "the merged offer may exist in the wallet, so the "
                       "tiers are NOT created individually this cycle",
                       pair.name, to_string(tiers.front().side), batch_err);
        co_return 0;
    }

    if (batch_failed) {
        // [S74] This create answered (with a failure): released here so the
        // fallback tiers below are marked one at a time by their own marks,
        // never nested inside this one.
        batch_mark.release();
        // Fallback: if batch fails, fall through to individual creation.
        logger_->warn("Batch create_offer failed for {} {} -- "
                      "falling back to individual: {}",
                      pair.name, to_string(tiers.front().side), batch_err);
        int fallback_count = 0;
        for (const auto& tier : tiers) {
            json single_dict = build_offer_dict(pair, tier);
            if (single_dict.empty()) continue;
            // [XCH-LOCK-LEDGER] Re-charged per tier (review round 3):
            // the merged attempt charged ONE combined selection, but each
            // fallback tier is a separate wallet offer with its own
            // whole-coin principal + fee-coin selection, so skipping the
            // charge under-counts real locks and can breach both cap and
            // floor.  Over-counting after the failed merged create is the
            // contract's deliberate conservative direction; the cost is a
            // quieter cycle after a transient batch failure, healed at the
            // next reseed.
            if (!xch_ledger_admits(single_dict, pair, tier.side,
                                   "batch fallback")) {
                continue;
            }
            // [review] The fallback loop issued creates without ever
            // consulting the predicate -- so a watchdog fire during the
            // failed merged request, or between fallback tiers, went on
            // rebuilding the book one tier at a time. Checked before each.
            if (abort_predicate_ && abort_predicate_()) {
                logger_->error("abandoning batch fallback for {} after tier "
                               "{}: the engine asked us to stop",
                               pair.name, tier.tier_index);
                break;
            }
            // [S74 / review #165, round 4] ...and the fallback loop is a
            // create-per-tier loop like post_quotes', so it needs the same
            // non-cancelling gate. Its own copy: a gate on one loop says
            // nothing about another (see the "mutate every copy" rule).
            if (stop_creating_predicate_ && stop_creating_predicate_()) {
                logger_->warn("not creating any further {} fallback tier: a "
                              "stop is latched. Nothing already created is "
                              "cancelled.", pair.name);
                break;
            }
            bool tier_failed = false;
            std::string tier_err;
            json sr;
            // [S74 / review #165] One mark per fallback create, cleared when
            // that tier's offer is in State below.
            PostingMark fallback_mark{posting_in_flight_flag_};
            try {
                sr = co_await create_offer_min_coin(
                    single_dict, expiry_max_time, pair, tier.side,
                    static_cast<int>(tier.tier_index), "batch fallback");
            } catch (const rpc::ChiaRPCTransportError& e2) {
                // [review #165, round 4] Its own copy, for the same reason as
                // the two above: no answer is not a refusal.
                tier_failed = true;
                tier_err = e2.what();
                note_create_outcome_unknown(e2, pair.name, "batch fallback");
            } catch (const rpc::ChiaRPCError& e2) {
                tier_failed = true;
                tier_err = e2.what();
            }
            // And again after it lands, for the same reason as the primary
            // path: a create that completes after the bulk cancel has
            // enumerated the book leaves a live offer nobody cancelled.
            if (!tier_failed && abort_predicate_ && abort_predicate_()) {
                const std::string late_id = late_trade_id(sr, "fallback");
                logger_->error("fallback create for {} tier {} landed AFTER "
                               "the stop; cancelling trade {}", pair.name,
                               tier.tier_index,
                               late_id.empty() ? "<unknown>" : late_id);
                if (!late_id.empty()) {
                    // [review round 11] Adopt BEFORE cancelling, as the
                    // merged path does: a secure cancel is an unconfirmed
                    // spend, and a counterparty can still win the race -- a
                    // fill on an unadopted trade is invisible to detection and
                    // to the next startup.
                    {
                        PendingOffer pending;
                        pending.offer_id         = late_id;
                        pending.pair_name        = pair.name;
                        pending.side             = tier.side;
                        pending.price            = tier.price;
                        pending.size             = tier.size;
                        pending.tier             = tier.tier_index;
                        pending.created_at_block = block_height;
                        pending.created_at_ts    = std::chrono::system_clock::now();
                        pending.fee_mojos        = current_fee_mojos_;
                        pending.post_spread_bps  = tier.spread_bps;
                        state_->upsert_offer(pending);
                    }
                    try {
                        co_await cancel_offer_charged(
                            late_id, xop::risk::watchdog_cancel().fee_mojos,
                        xop::risk::watchdog_cancel().secure);
                    } catch (const std::exception& e2) {
                        logger_->critical("could not cancel late offer {}: "
                                          "{} -- LIVE and unmanaged",
                                          late_id, e2.what());
                        if (escalate_) {
                            escalate_("a fallback offer created after the "
                                      "stop could NOT be cancelled and is "
                                      "LIVE: trade " + late_id + " ("
                                      + e2.what() + ").");
                        }
                    }
                }
                break;
            }
            if (tier_failed) {
                logger_->error("Fallback create_offer failed for {} tier {}: {}",
                               pair.name, tier.tier_index, tier_err);
                continue;
            }
            if (sr.contains("offer") && sr["offer"].is_string()
                && sr.contains("trade_record")
                && sr["trade_record"].contains("trade_id")
                && sr["trade_record"]["trade_id"].is_string()) {
                // [OFFER-EXPIRY] The fallback posts real, individual
                // offers; skipping the check here would leave exactly one
                // posting path able to rest an unbounded offer.
                if (expiry_max_time.has_value()
                    && !expiry_echo_ok(sr, *expiry_max_time)) {
                    PendingOffer adopt;
                    adopt.offer_id =
                        sr["trade_record"]["trade_id"].get<std::string>();
                    adopt.pair_name        = pair.name;
                    adopt.side             = tier.side;
                    adopt.price            = tier.price;
                    adopt.size             = tier.size;
                    adopt.tier             = tier.tier_index;
                    adopt.created_at_block = block_height;
                    adopt.created_at_ts    = std::chrono::system_clock::now();
                    adopt.fee_mojos        = current_fee_mojos_;
                    adopt.post_spread_bps  = tier.spread_bps;
                    co_await retire_offer_failed_expiry(
                        adopt, *expiry_max_time, "batch fallback");
                    continue;
                }
                std::string offer_text = sr["offer"].get<std::string>();
                PendingOffer po;
                // Retain dexie's id so this offer is excluded from our own
                // arbitrage scan; dropping it here would leave the fallback
                // path takeable by our own taker.
                po.dexie_id         = co_await submit_to_dexie(
                    offer_text,
                    fmt::format("{} {} tier {} (batch fallback)", pair.name,
                                to_string(tier.side), tier.tier_index));
                po.offer_id         = sr["trade_record"]["trade_id"].get<std::string>();
                po.pair_name        = pair.name;
                po.side             = tier.side;
                po.price            = tier.price;
                po.size             = tier.size;
                po.tier             = tier.tier_index;
                po.created_at_block = block_height;
                po.created_at_ts    = std::chrono::system_clock::now();
                // [WALLET-LOAD] For the fill-poll striking-distance reset.
                po.post_spread_bps  = tier.spread_bps;
                // [S70] Past expiry_echo_ok above: the wallet's echo.
                po.expiry_max_time  = expiry_max_time.value_or(0);
                state_->upsert_offer(po);
                fallback_mark.release();  // [S74] recorded
                ++fallback_count;

                // -- Fee reserve guard (batch fallback, UTXO-aware) ---------
                // Each individual creation in the fallback can lock UTXOs and
                // eat into the fee reserve.  Check after each success.
                if (strategy_cfg_.fee_reserve_xch > 0.0) {
                    try {
                        auto fb_bal = co_await wallet_->get_wallet_balance(1);
                        Mojo fb_spendable = 0;
                        if (fb_bal.contains("spendable_balance"))
                            fb_spendable = fb_bal["spendable_balance"].get<Mojo>();
                        const auto fb_reserve = static_cast<Mojo>(std::llround(
                            strategy_cfg_.fee_reserve_xch
                            * static_cast<double>(kMojosPerXch)));
                        if (fb_spendable < fb_reserve) {
                            logger_->warn(
                                "XCH fee reserve guard (batch fallback): "
                                "spendable {:.6f} XCH < reserve {:.3f} XCH "
                                "after {} tier {} -- stopping fallback",
                                static_cast<double>(fb_spendable) / kMojosPerXch,
                                strategy_cfg_.fee_reserve_xch,
                                pair.name, tier.tier_index);
                            break;
                        }
                    } catch (const std::exception& e) {
                        logger_->warn("XCH fee reserve guard (batch fallback): "
                                      "balance check failed: {}", e.what());
                    }
                }
            }
        }
        co_return fallback_count;
    }

    // [sweep] The SUCCESSFUL merged create had no post-create recheck. The
    // pre-create gate and the fallback tiers both got one; this path -- the
    // one that actually publishes to dexie on the happy day -- did not. If
    // the switch fires while create_offer() is suspended here, the merged
    // offer is submitted to a book the bulk cancel has already enumerated.
    if (abort_predicate_ && abort_predicate_()) {
        const std::string late_id = late_trade_id(result, "merged");
        logger_->error("merged create for {} landed AFTER the stop; "
                       "cancelling trade {} rather than publishing it",
                       pair.name, late_id.empty() ? "<unknown>" : late_id);
        if (!late_id.empty()) {
            // [review round 10] TRACK IT EITHER WAY. A successful secure
            // cancel is an UNCONFIRMED spend -- a counterparty holding the
            // offer file can still win the race to the coins. Returning
            // without adopting the trade meant a fill in that window hit an
            // offer neither State nor the DB had ever heard of: fill
            // detection could not see it, and the next startup could not
            // reconcile it. The trade is adopted under its real id (no
            // dexie id -- it was never published) so the ordinary fill and
            // status machinery carries it to a terminal state, whichever
            // way the race resolves.
            // [review round 11] ONE aggregate record, not a loop.
            // State::upsert_offer is keyed by offer_id alone, so a per-tier
            // loop under one trade id overwrote itself and retained only
            // the LAST tier -- the "all constituents tracked" claim was
            // false for every merged batch of more than one. The merged
            // trade is one wallet trade; it is represented as one pending
            // record carrying the total size, priced at the most
            // conservative (worst-for-us) tier so fill accounting cannot
            // overstate what the race can win.
            {
                PendingOffer pending;
                pending.offer_id         = late_id;
                pending.pair_name        = pair.name;
                pending.side             = tiers.front().side;
                pending.price            = tiers.front().price;
                pending.size             = 0;
                for (const auto& tier : tiers) {
                    pending.size += tier.size;
                    const bool worse = (tier.side == Side::Bid)
                        ? tier.price > pending.price
                        : tier.price < pending.price;
                    if (worse) pending.price = tier.price;
                }
                pending.tier             = tiers.front().tier_index;
                pending.created_at_block = block_height;
                pending.created_at_ts    = std::chrono::system_clock::now();
                pending.fee_mojos        = current_fee_mojos_;
                pending.post_spread_bps  = tiers.front().spread_bps;
                state_->upsert_offer(pending);
            }
            try {
                co_await cancel_offer_charged(
                    late_id, xop::risk::watchdog_cancel().fee_mojos,
                    xop::risk::watchdog_cancel().secure);
            } catch (const std::exception& e) {
                logger_->critical("could not cancel the late merged offer "
                                  "{}: {} -- LIVE and unmanaged",
                                  late_id, e.what());
                if (escalate_) {
                    escalate_("a merged offer created after the stop could "
                              "NOT be cancelled and is LIVE: trade "
                              + late_id + " (" + e.what() + "). It IS "
                              "tracked in State, so fills on it will be "
                              "seen.");
                }
            }
        }
        co_return 0;
    }

    // Extract trade_id and offer text.
    if (!result.contains("offer") || !result["offer"].is_string()
        || !result.contains("trade_record")
        || !result["trade_record"].contains("trade_id")) {
        logger_->error("Batch: create_offer response missing fields for {} {}",
                       pair.name, to_string(tiers.front().side));
        co_return 0;
    }

    std::string trade_id   = result["trade_record"]["trade_id"].get<std::string>();
    std::string offer_text = result["offer"].get<std::string>();

    // [OFFER-EXPIRY] Same fail-closed check as the single path, before the
    // dexie submission.  One adoption record stands in for the whole merged
    // offer, exactly as the tier loop below tracks every tier under this
    // one trade_id.
    if (expiry_max_time.has_value()
        && !expiry_echo_ok(result, *expiry_max_time)) {
        // [review #150] ONE AGGREGATE RECORD, not the first tier.  This is a
        // single wallet trade carrying every merged tier, and upsert_offer is
        // keyed by offer_id alone -- recording only tiers.front() under-books
        // the size, so if the cancel loses the race to a taker the fill is
        // accounted at a fraction of what was actually filled.  Same shape as
        // the late-merged-create path above (round 11): total size, priced at
        // the most conservative (worst-for-us) tier so fill accounting cannot
        // overstate what the race can win.
        PendingOffer adopt;
        adopt.offer_id         = trade_id;
        adopt.pair_name        = pair.name;
        adopt.side             = tiers.front().side;
        adopt.price            = tiers.front().price;
        adopt.size             = 0;
        for (const auto& t : tiers) {
            adopt.size += t.size;
            const bool worse = (t.side == Side::Bid)
                ? t.price > adopt.price
                : t.price < adopt.price;
            if (worse) adopt.price = t.price;
        }
        adopt.tier             = tiers.front().tier_index;
        adopt.created_at_block = block_height;
        adopt.created_at_ts    = std::chrono::system_clock::now();
        adopt.fee_mojos        = current_fee_mojos_;
        adopt.post_spread_bps  = tiers.front().spread_bps;
        co_await retire_offer_failed_expiry(adopt, *expiry_max_time,
                                            "merged batch");
        co_return 0;
    }

    // Submit to dexie (best-effort).  Retain the id: every tier merged into
    // this batch rests on the book under it, and own-offer exclusion in the
    // arbitrage taker matches on it.
    const std::string batch_dexie_id = co_await submit_to_dexie(
        offer_text,
        fmt::format("{} {} merged batch of {} tiers", pair.name,
                    to_string(tiers.front().side), tiers.size()));

    // Track all constituent tiers with the same offer_id.
    for (const auto& tier : tiers) {
        PendingOffer pending;
        pending.offer_id         = trade_id;
        pending.pair_name        = pair.name;
        pending.side             = tier.side;
        pending.price            = tier.price;
        pending.size             = tier.size;
        pending.tier             = tier.tier_index;
        pending.created_at_block = block_height;
        pending.created_at_ts    = std::chrono::system_clock::now();
        pending.fee_mojos        = current_fee_mojos_;
        // [WALLET-LOAD] For the fill-poll striking-distance reset.
        pending.post_spread_bps  = tier.spread_bps;
        pending.dexie_id         = batch_dexie_id;
        // [S70] Past expiry_echo_ok above: the wallet's echo.
        pending.expiry_max_time  = expiry_max_time.value_or(0);
        state_->upsert_offer(pending);
    }
    batch_mark.release();  // [S74] every constituent tier is in State

    logger_->info("Batch: posted {} {} ({} tiers merged) [{}]",
                  pair.name, to_string(tiers.front().side),
                  tiers.size(), trade_id.substr(0, 12));

    co_return static_cast<int>(tiers.size());
}

// ---------------------------------------------------------------------------
// submit_to_dexie -- post offer text to the dexie aggregator (best-effort)
// ---------------------------------------------------------------------------

asio::awaitable<std::string> OfferManager::submit_to_dexie(
    const std::string& offer_text,
    std::string        posting)
{
    // Best-effort submission to the Dexie aggregator for cross-platform
    // visibility.  The offer is already valid on-chain regardless of
    // whether Dexie accepts it, so failures are non-fatal.
    //
    // ISO/IEC 27001:2022: full offer bech32m payload is never logged;
    // DexieClient internally truncates the payload in log output.
    // ISO/IEC 5055: null-pointer guard prevents UB if client is absent.

    // Guard: if no DexieClient was injected, skip silently.
    if (!dexie_client_) {
        logger_->debug("submit_to_dexie: no DexieClient configured -- "
                       "skipping aggregator submission");
        co_return std::string{};
    }

    try {
        // Ensure the client session is open before posting.
        if (!dexie_client_->is_open()) {
            dexie_client_->open();
        }

        // POST /v1/offers with JSON body {"offer": "<bech32m text>"}.
        // DexieClient::submit_offer handles rate limiting, retries on
        // 429/5xx, and JSON parsing internally.
        rpc::SubmitResult result = co_await dexie_client_->submit_offer(
            offer_text, dexie_cfg_.claim_rewards);

        if (result.success) {
            logger_->info("submit_to_dexie: accepted (dexie_id={})",
                          result.offer_id);
            co_return result.offer_id;
        }

        // Dexie rejected the offer (e.g. duplicate, invalid, expired).
        // Log the reason but do not treat as a hard failure.
        logger_->warn("submit_to_dexie: rejected by Dexie -- {}",
                      result.error_message);
        if (dexie_rejected_too_many_inputs(result.error_message)) {
            note_dexie_too_many_inputs(posting, offer_text.size());
        }
        co_return std::string{};

    } catch (const rpc::DexieRateLimitError& e) {
        // Rate-limit exhaustion after max retries.  Non-fatal: the offer
        // remains valid on-chain; aggregator visibility is best-effort.
        logger_->warn("submit_to_dexie: rate-limited -- {}", e.what());
        co_return std::string{};
    } catch (const rpc::DexieClientError& e) {
        // Non-retryable 4xx error (bad request, invalid offer format, etc.).
        logger_->warn("submit_to_dexie: client error -- {}", e.what());
        // [MIN-INPUT-COIN] The shape seen live: HTTP 400 whose body carries
        // "Too many input coins".  Not an auto-cancel -- only the one line
        // an operator can grep for and act on.
        if (dexie_rejected_too_many_inputs(e.response_body)) {
            note_dexie_too_many_inputs(posting, offer_text.size());
        }
        co_return std::string{};
    } catch (const rpc::DexieServerError& e) {
        // Server-side 5xx that persisted after retries.
        logger_->warn("submit_to_dexie: server error -- {}", e.what());
        co_return std::string{};
    } catch (const rpc::DexieError& e) {
        // Catch-all for any other DexieClient transport error
        // (curl failure, JSON parse error, client not open, etc.).
        logger_->warn("submit_to_dexie: transport error -- {}", e.what());
        co_return std::string{};
    } catch (const std::exception& e) {
        // Defensive catch for unexpected exceptions.  Should not fire in
        // normal operation, but prevents an unhandled exception from
        // propagating and crashing the offer lifecycle coroutine.
        logger_->error("submit_to_dexie: unexpected exception -- {}",
                       e.what());
        co_return std::string{};
    }
}

// ---------------------------------------------------------------------------
// resolve_wallet_id -- look up a wallet ID from an asset ID
// ---------------------------------------------------------------------------

std::int64_t OfferManager::resolve_wallet_id(const AssetId& asset_id) const
{
    // Native XCH is always wallet_id 1.
    if (asset_id == "xch" || asset_id == "XCH") {
        return 1;
    }

    auto it = wallet_id_map_.find(asset_id);
    if (it != wallet_id_map_.end()) {
        return it->second;
    }

    return -1;  // Not found.
}

// ---------------------------------------------------------------------------
// init_wallet_id_map -- one-time cache population from the wallet RPC
// ---------------------------------------------------------------------------

asio::awaitable<void> OfferManager::init_wallet_id_map()
{
    logger_->info("Initialising wallet ID map from wallet RPC...");

    try {
        auto wallets = co_await wallet_->get_wallets();

        for (const auto& w : wallets) {
            // Each wallet record contains "id" (int) and "data" (asset ID
            // for CAT wallets).  Type 6 = CAT wallet.
            if (!w.contains("id")) {
                continue;
            }

            std::int64_t wid = w["id"].get<std::int64_t>();

            // Type 6 is CAT_WALLET in the Chia wallet enumeration.
            if (w.contains("type") && w["type"].get<int>() == 6 &&
                w.contains("data")) {
                std::string asset_id = w["data"].get<std::string>();

                std::transform(asset_id.begin(), asset_id.end(), asset_id.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });

                // Wallet RPC returns CAT asset IDs with a trailing "00"
                // suffix in this field; normalize back to the canonical
                // 64-hex asset ID used throughout config and Dexie.
                if (asset_id.size() == 66 &&
                    asset_id.compare(asset_id.size() - 2, 2, "00") == 0) {
                    asset_id.resize(64);
                }

                wallet_id_map_[asset_id] = wid;
                logger_->debug("Mapped asset {} -> wallet_id {}", asset_id,
                               wid);
            }
        }

        wallet_ids_resolved_ = true;
        logger_->info("Wallet ID map initialised: {} CAT wallets found",
                      wallet_id_map_.size());
    } catch (const rpc::ChiaRPCError& e) {
        logger_->error("Failed to initialise wallet ID map: {}", e.what());
        // Allow retry on next post_quotes() call by leaving the flag false.
    }
}

// ---------------------------------------------------------------------------
// try_parse_wallet_offer -- extract PendingOffer from a wallet trade record
//
// Best-effort metadata extraction: parses the summary's offered/requested
// fields and matches against pair_config_map_ to determine pair, side,
// price, and size.  Returns std::nullopt if the record is unparseable.
// ---------------------------------------------------------------------------

std::optional<PendingOffer> OfferManager::try_parse_wallet_offer(
    const json& trade_record,
    BlockHeight current_block) const
{
    if (!trade_record.contains("trade_id") ||
        !trade_record.contains("summary") ||
        !trade_record["summary"].is_object()) {
        return std::nullopt;
    }

    const std::string trade_id = trade_record["trade_id"].get<std::string>();
    const auto& summary = trade_record["summary"];

    // Parse offered and requested asset -> amount maps.
    std::unordered_map<std::string, Mojo> offered;
    std::unordered_map<std::string, Mojo> requested;

    auto parse_side = [](const json& obj,
                         std::unordered_map<std::string, Mojo>& out) {
        if (!obj.is_object()) return;
        for (auto& [asset_id, amount_val] : obj.items()) {
            if (amount_val.is_number_integer()) {
                out[asset_id] = amount_val.get<Mojo>();
            } else if (amount_val.is_number_unsigned()) {
                out[asset_id] = static_cast<Mojo>(
                    amount_val.get<std::uint64_t>());
            } else if (amount_val.is_string()) {
                try {
                    out[asset_id] = static_cast<Mojo>(
                        std::stoll(amount_val.get<std::string>()));
                } catch (...) {}
            }
        }
    };

    if (summary.contains("offered"))
        parse_side(summary["offered"], offered);
    if (summary.contains("requested"))
        parse_side(summary["requested"], requested);

    if (offered.empty() || requested.empty()) return std::nullopt;

    // Match against configured pairs.
    PendingOffer po;
    po.offer_id = trade_id;
    po.tier = 0;
    po.fee_mojos = 0;
    bool matched = false;

    for (const auto& [pname, pcfg] : pair_config_map_) {
        const std::string& base  = pcfg.base_asset_id;
        const std::string& quote = pcfg.quote_asset_id;

        if (offered.count(base) && requested.count(quote)) {
            po.pair_name = pname;
            po.side      = Side::Ask;
            po.size      = offered.at(base);
            const double base_d  = static_cast<double>(offered.at(base));
            const double quote_d = static_cast<double>(requested.at(quote));
            po.price = (base_d > 0.0)
                ? static_cast<Mojo>(std::llround(
                    quote_d / base_d * static_cast<double>(kMojosPerXch)))
                : 0;
            matched = true;
            break;
        }
        if (offered.count(quote) && requested.count(base)) {
            po.pair_name = pname;
            po.side      = Side::Bid;
            po.size      = requested.at(base);
            const double base_d  = static_cast<double>(requested.at(base));
            const double quote_d = static_cast<double>(offered.at(quote));
            po.price = (base_d > 0.0)
                ? static_cast<Mojo>(std::llround(
                    quote_d / base_d * static_cast<double>(kMojosPerXch)))
                : 0;
            matched = true;
            break;
        }
    }

    if (!matched) return std::nullopt;

    // Extract fee.
    if (summary.contains("fees") && summary["fees"].is_number()) {
        po.fee_mojos = summary["fees"].get<std::uint64_t>();
    }

    // [S70] The wallet's own record of the timelock it put on this offer
    // (valid_times is parse_timelock_info of the conditions it signed), so an
    // adopted offer keeps its expiry across a restart.  0 when absent.
    po.expiry_max_time = trade_record_max_time(trade_record);

    // Approximate created_at_block from wall-clock time.
    po.created_at_block = 0;
    if (current_block > 0 && trade_record.contains("created_at_time") &&
        trade_record["created_at_time"].is_number()) {
        const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        double created_at = trade_record["created_at_time"].get<double>();
        if (created_at > 0.0) {
            const double age_seconds =
                static_cast<double>(now_epoch) - created_at;
            const BlockHeight approx_age = static_cast<BlockHeight>(
                std::max(0.0, age_seconds / 52.0));
            po.created_at_block = (current_block > approx_age)
                ? (current_block - approx_age) : 0;
        }
    }

    return po;
}

// ---------------------------------------------------------------------------
// parse_settled_fill -- extract SETTLED size/price from a confirmed trade
//                       record for Fill construction (see header contract).
//
// Dimensional analysis (write it out -- this codebase has hit 1e9-class
// unit errors twice):
//
//   The wallet summary carries RAW MOJOS of each asset:
//     XCH : 1 unit = 1e12 mojos  (base_mojos_per_unit / quote_mojos_per_unit)
//     CATs: 1 unit = 1e3  mojos
//
//   Engine Fill convention (must match build_offer_dict / quote_mojos_for):
//     size  [base mojos]   = base_units * base_mojos_per_unit
//     price [pseudo-mojos] = quote_units_per_base_unit * kMojosPerXch
//
//   From settled raw mojos:
//     base_units  = base_mojos  / base_denom
//     quote_units = quote_mojos / quote_denom
//     price = (quote_units / base_units) * kMojosPerXch
//           = quote_mojos * base_denom * kMojosPerXch
//             / (base_mojos * quote_denom)                          ... (*)
//
//   Roundtrip proof against the canonical helper (types.hpp):
//     quote_mojos_for(size, price, base_denom, quote_denom)
//       = base_mojos * (*) * quote_denom / (base_denom * kMojosPerXch)
//       = quote_mojos                                               [exact]
//
//   Worked example, XCH/wUSDC.b ask (base XCH 1e12, quote CAT 1e3):
//     settled: gave 1 XCH (1e12 base mojos), got 20 wUSDC.b (20'000
//     quote mojos).  price = 20'000 * 1e12 * 1e12 / (1e12 * 1e3)
//     = 20e12 = 20 quote-units-per-base * kMojosPerXch.  Correct.
//     Omitting the base_denom/quote_denom ratio (1e9 for XCH/CAT
//     pairs) would yield 20e3 -- the classic 1e9-class error.
//
// Computation in double: the numerator can reach ~1e12 * 1e12 * 1e12
// = 1e36 which overflows int64 (double keeps ~15-16 significant digits,
// ample for prices whose true precision is bounded by mojo granularity).
// ---------------------------------------------------------------------------

std::optional<SettledFill> OfferManager::parse_settled_fill(
    const json&       trade_record,
    Side              side,
    const PairConfig& pair)
{
    if (!trade_record.contains("summary") ||
        !trade_record["summary"].is_object()) {
        return std::nullopt;
    }
    const auto& summary = trade_record["summary"];

    // Parse offered / requested asset -> raw-mojo maps.  Tolerates the
    // integer, unsigned, and string encodings observed across Chia wallet
    // versions (same tolerance as try_parse_wallet_offer above).
    std::unordered_map<std::string, Mojo> offered;    // we gave
    std::unordered_map<std::string, Mojo> requested;  // we got

    auto parse_side_map = [](const json& obj,
                             std::unordered_map<std::string, Mojo>& out) {
        if (!obj.is_object()) return;
        for (auto& [asset_id, amount_val] : obj.items()) {
            if (amount_val.is_number_integer()) {
                out[asset_id] = amount_val.get<Mojo>();
            } else if (amount_val.is_number_unsigned()) {
                out[asset_id] = static_cast<Mojo>(
                    amount_val.get<std::uint64_t>());
            } else if (amount_val.is_string()) {
                try {
                    out[asset_id] = static_cast<Mojo>(
                        std::stoll(amount_val.get<std::string>()));
                } catch (...) {}
            }
        }
    };

    if (summary.contains("offered"))
        parse_side_map(summary["offered"], offered);
    if (summary.contains("requested"))
        parse_side_map(summary["requested"], requested);

    if (offered.empty() || requested.empty()) return std::nullopt;

    // Locate the two legs on the sides our order implies.
    //   Ask: we GAVE base, GOT quote.
    //   Bid: we GAVE quote, GOT base.
    const std::string& base_id  = pair.base_asset_id;
    const std::string& quote_id = pair.quote_asset_id;

    const auto& base_side  = (side == Side::Ask) ? offered   : requested;
    const auto& quote_side = (side == Side::Ask) ? requested : offered;

    const auto b_it = base_side.find(base_id);
    const auto q_it = quote_side.find(quote_id);
    if (b_it == base_side.end() || q_it == quote_side.end()) {
        return std::nullopt;
    }

    const Mojo base_mojos  = b_it->second;
    const Mojo quote_mojos = q_it->second;
    if (base_mojos <= 0 || quote_mojos <= 0) return std::nullopt;
    if (pair.base_mojos_per_unit <= 0 || pair.quote_mojos_per_unit <= 0) {
        return std::nullopt;
    }

    // Formula (*) from the dimensional analysis above.
    const double price_d =
        static_cast<double>(quote_mojos)
        * static_cast<double>(pair.base_mojos_per_unit)
        * static_cast<double>(kMojosPerXch)
        / (static_cast<double>(base_mojos)
           * static_cast<double>(pair.quote_mojos_per_unit));

    if (!std::isfinite(price_d) || price_d <= 0.0) return std::nullopt;

    SettledFill sf;
    sf.size  = base_mojos;
    sf.price = static_cast<Mojo>(std::llround(price_d));
    return sf;
}

// ---------------------------------------------------------------------------
// emergency_cancel -- reduced/zero-fee cancel fallback
// ---------------------------------------------------------------------------

asio::awaitable<bool> OfferManager::emergency_cancel(
    const std::string& offer_id,
    const std::string& context,
    bool prefer_zero_fee)
{
    // [FILL-PROOF, review #171] cancel_offer_charged refuses an offer under
    // fill proof.  Stop here rather than read the balance and walk the whole
    // fee ladder into that refusal.
    if (cancel_withheld_for_proof(offer_id)) {
        logger_->warn("{}: cancel of {} withheld -- the wallet reports it "
                      "CONFIRMED and the fill proof has not settled it",
                      context, offer_id.substr(0, 12));
        co_return false;
    }
    try {
        // When prefer_zero_fee is set (UTXO liberation), try the fee=0
        // secure cancel FIRST so we don't burn spendable XCH on fees.
        if (prefer_zero_fee) {
            logger_->warn("{}: attempting zero-fee secure cancel for {}",
                          context, offer_id.substr(0, 12));
            bool zero_ok = false;
            try {
                co_await cancel_offer_charged(
                    offer_id, 0, /*secure=*/true);
                zero_ok = true;
            } catch (const std::exception& e) {
                logger_->debug("{}: zero-fee secure cancel failed for {}: {}",
                               context, offer_id.substr(0, 12), e.what());
            }
            if (zero_ok) {
                logger_->info("{}: secure-cancelled {} with fee=0",
                              context, offer_id.substr(0, 12));
                co_return true;
            }
            // Fall through to descending fee loop.
        }

        auto xch_bal = co_await wallet_->get_wallet_balance(1);
        Mojo xch_spendable = 0;
        if (xch_bal.contains("spendable_balance"))
            xch_spendable = xch_bal["spendable_balance"].get<Mojo>();

        if (xch_spendable > 0) {
            // Try descending fee tiers: 2x dynamic fee, 1x dynamic fee,
            // half, quarter, down to 1 mojo.  If the wallet reports
            // insufficient funds at a given tier, halve and retry.
            // This lets us cancel even when spendable is far below the
            // configured minimum fee.
            // [review #163 r6] The DOUBLING happens in the uint64 domain, so
            // it has to saturate BEFORE the conversion does: a bare
            // `static_cast<Mojo>(current_fee_mojos_ * 2)` wraps the product
            // first and then narrows the wrapped value, which no amount of
            // care at the cast alone would catch.
            const Mojo fee_cap = to_mojo_saturating(
                current_fee_mojos_ > std::numeric_limits<std::uint64_t>::max() / 2U
                    ? std::numeric_limits<std::uint64_t>::max()
                    : current_fee_mojos_ * 2U);
            Mojo attempt_fee = std::min(
                fee_cap,
                std::max(Mojo{1}, xch_spendable - Mojo{1000}));

            while (attempt_fee >= 1) {
                logger_->warn("{}: emergency cancel {} with fee "
                              "{} mojos (spendable {} mojos)",
                              context, offer_id.substr(0, 12),
                              attempt_fee, xch_spendable);
                bool rpc_failed = false;
                bool insufficient = false;
                try {
                    co_await cancel_offer_charged(
                        offer_id,
                        static_cast<std::uint64_t>(attempt_fee),
                        /*secure=*/true);
                } catch (const rpc::ChiaRPCError& e) {
                    rpc_failed = true;
                    const std::string_view msg{e.what()};
                    insufficient =
                        msg.find("insufficient funds")
                            != std::string_view::npos ||
                        msg.find("spendable balance")
                            != std::string_view::npos;
                    if (!insufficient) {
                        logger_->error(
                            "{}: emergency cancel RPC error for {}: {}",
                            context, offer_id.substr(0, 12), e.what());
                    }
                }
                if (!rpc_failed) {
                    logger_->info(
                        "{}: emergency-cancelled {} (fee {} mojos)",
                        context, offer_id.substr(0, 12), attempt_fee);
                    co_return true;
                }
                if (!insufficient) break;  // non-balance error, stop retrying
                // Halve the fee and retry.
                attempt_fee = attempt_fee / 2;
            }
        }

        // Zero spendable or all fee tiers exhausted: try secure cancel
        // with fee=0.  The offer's locked coins serve as the spend
        // bundle inputs, so no additional spendable XCH is needed.
        // Skip if prefer_zero_fee already tried this at the top.
        if (!prefer_zero_fee) {
            logger_->warn("{}: attempting secure cancel with fee=0 for {}",
                          context, offer_id.substr(0, 12));
            bool secure_zero_ok = false;
            try {
                co_await cancel_offer_charged(
                    offer_id, 0, /*secure=*/true);
                secure_zero_ok = true;
            } catch (const rpc::ChiaRPCError& e) {
                logger_->debug("{}: secure cancel fee=0 failed for {}: {}",
                               context, offer_id.substr(0, 12), e.what());
            } catch (const std::exception& e) {
                logger_->debug("{}: secure cancel fee=0 exception for {}: {}",
                               context, offer_id.substr(0, 12), e.what());
            }
            if (secure_zero_ok) {
                logger_->info("{}: secure-cancelled {} with fee=0",
                              context, offer_id.substr(0, 12));
                co_return true;
            }
        }

        // Last resort: local-only cancel.
        // No on-chain fee -- drops from wallet but doesn't invalidate the
        // offer on-chain.  Better than leaving funds locked forever.
        logger_->warn("{}: zero XCH spendable -- local-only (insecure) "
                      "cancel for {}", context, offer_id.substr(0, 12));
        co_await cancel_offer_charged(
            offer_id, 0, /*secure=*/false);
        logger_->warn("{}: local-only cancelled {} "
                      "(INSECURE -- offer may still be taken on-chain)",
                      context, offer_id.substr(0, 12));
        co_return true;
    } catch (const std::exception& e) {
        logger_->error("{}: emergency cancel also failed for {}: {}",
                       context, offer_id.substr(0, 12), e.what());
    }
    co_return false;
}

}  // namespace xop::execution

