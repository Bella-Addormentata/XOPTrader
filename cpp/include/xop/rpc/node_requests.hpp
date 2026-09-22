// ---------------------------------------------------------------------------
// node_requests.hpp -- pure request construction and response parsing for the
// full node's fee endpoints.
//
// [S67 2026-09-20] Split out of chia_rpc.cpp, like wallet_requests.hpp, so the
// wire format is reachable from ctest: the only evidence of the defect below
// is a KEY NAME in a JSON object nothing but the node ever read.
//
//   get_fee_estimate sent {"target_times", "spend_type": "send_xch_transaction"}.
//     The handler (chia 2.7.4 full_node_rpc_api.py get_fee_estimate) multiplies
//     ONE fee rate by the cost the request names, and for that spend_type the
//     cost is its stopgap table's 9,401,710 -- a plain XCH send.  This wallet's
//     CAT cancels cost 42.2M and its takes 92.6M-212M, so the number that came
//     back was 4.5x-22x too small for what it was used to pay for.
//     _validate_fee_estimate_cost requires EXACTLY ONE of spend_bundle, cost,
//     spend_type, so "cost" replaces "spend_type", it is not added beside it.
//
//   The rate is independent of the cost asked about, so ONE request with a
//   reference cost yields the rate, and every class's estimate is rate x its
//   cost: the fix adds no RPC.
//
//   get_blockchain_state already carries the node's admission floor --
//     mempool_cost, mempool_max_total_cost and mempool_min_fees.cost_5000000
//     (full_node_rpc_api.py get_blockchain_state) -- and get_block_height()
//     already fetches it every poll, so reading it costs no RPC either.
//
// Source, tag 2.7.4:
//   https://github.com/Chia-Network/chia-blockchain/blob/2.7.4/chia/full_node/full_node_rpc_api.py
//   https://github.com/Chia-Network/chia-blockchain/blob/2.7.4/chia/full_node/mempool_manager.py
//   https://github.com/Chia-Network/chia-blockchain/blob/2.7.4/chia/full_node/mempool.py
//
// Pure functions: no I/O, no clock, no logging.
// ---------------------------------------------------------------------------

#ifndef XOP_RPC_NODE_REQUESTS_HPP
#define XOP_RPC_NODE_REQUESTS_HPP

#include <cmath>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace xop::rpc {

/// The cost get_fee_estimate is asked about when only the RATE is wanted.
/// 10^9 keeps nine significant digits of mojos-per-cost in the integer the
/// node returns (it truncates rate x cost to uint64), and at any plausible
/// rate the product stays far below 2^63.
inline constexpr std::uint64_t kFeeEstimateReferenceCost = 1'000'000'000ULL;

/// chia 2.7.4 mempool_manager.py nonzero_fee_minimum_fpc: with the mempool at
/// capacity a spend paying less than this per cost is refused outright
/// (Err.INVALID_FEE_TOO_CLOSE_TO_ZERO).
inline constexpr double kNonzeroFeeMinimumFpc = 5.0;

/// The legacy payload, byte for byte what chia_rpc.cpp sent before S67.  Kept
/// so `fees.cost_aware_estimate: false` is provably unchanged.
[[nodiscard]] inline nlohmann::json make_fee_estimate_request_legacy(
    std::uint64_t target_time_seconds)
{
    return nlohmann::json{
        {"target_times", {target_time_seconds}},
        {"spend_type", "send_xch_transaction"}
    };
}

/// The cost-aware payload: exactly one of the handler's three cost sources.
[[nodiscard]] inline nlohmann::json make_fee_estimate_request(
    std::uint64_t target_time_seconds, std::uint64_t cost)
{
    return nlohmann::json{
        {"target_times", {target_time_seconds}},
        {"cost", cost}
    };
}

namespace detail {

/// A JSON number as a uint64: an unsigned integer, or a signed one that is
/// not negative (nlohmann stores a non-negative integer PARSED from text as
/// unsigned, but one built from a C++ int as signed).  A float, a negative
/// number, a string or null is not a count and yields false.
[[nodiscard]] inline bool read_count(const nlohmann::json& j, std::uint64_t& out)
{
    if (j.is_number_unsigned()) {
        out = j.get<std::uint64_t>();
        return true;
    }
    if (j.is_number_integer()) {
        const auto v = j.get<std::int64_t>();
        if (v >= 0) {
            out = static_cast<std::uint64_t>(v);
            return true;
        }
    }
    return false;
}

}  // namespace detail

/// What one get_fee_estimate reply says.  `ok` false means the reply could
/// not be read; every other member is then 0 and must not be used.
struct FeeEstimateReading {
    bool          ok{false};
    std::uint64_t estimate_mojos{0};   ///< estimates[0], for the cost asked
    double        rate{0.0};           ///< estimate_mojos / cost, mojos per cost
    std::uint64_t mempool_cost{0};     ///< "mempool_size": total CLVM cost held
    std::uint64_t mempool_max_cost{0}; ///< "mempool_max_size"
    bool          mempool_known{false};
};

/// Parse a get_fee_estimate reply to a request that named `cost`.
///
/// The handler names the mempool's total COST "mempool_size" here (it is
/// mempool.total_mempool_cost()), while get_blockchain_state uses
/// "mempool_size" for the item COUNT and "mempool_cost" for the cost.  Same
/// key, different quantity; this function reads the get_fee_estimate meaning.
[[nodiscard]] inline FeeEstimateReading parse_fee_estimate(const nlohmann::json& resp,
                                                           std::uint64_t         cost)
{
    FeeEstimateReading out{};
    if (cost == 0U || !resp.is_object()) {
        return out;
    }
    const auto est = resp.find("estimates");
    if (est == resp.end() || !est->is_array() || est->empty()
        || !detail::read_count((*est)[0], out.estimate_mojos)) {
        return FeeEstimateReading{};
    }
    out.rate = static_cast<double>(out.estimate_mojos) / static_cast<double>(cost);
    out.ok   = true;
    const auto size = resp.find("mempool_size");
    const auto max  = resp.find("mempool_max_size");
    std::uint64_t size_v = 0;
    std::uint64_t max_v  = 0;
    if (size != resp.end() && max != resp.end()
        && detail::read_count(*size, size_v) && detail::read_count(*max, max_v)) {
        out.mempool_cost     = size_v;
        out.mempool_max_cost = max_v;
        out.mempool_known    = true;
    }
    return out;
}

/// The node's mempool, as its last get_blockchain_state reply described it.
/// `known` false means the node did not say -- never "the mempool is empty".
struct MempoolState {
    bool          known{false};
    std::uint64_t cost{0};             ///< blockchain_state.mempool_cost
    std::uint64_t max_total_cost{0};   ///< blockchain_state.mempool_max_total_cost
    double        min_fee_rate_5m{0.0};///< mempool_min_fees.cost_5000000
};

/// Extract the mempool fields from a get_blockchain_state reply.  Any missing
/// or mistyped field leaves `known` false: a reading is whole or it is absent.
[[nodiscard]] inline MempoolState node_mempool_from_blockchain_state(const nlohmann::json& resp)
{
    MempoolState out{};
    if (!resp.is_object()) {
        return out;
    }
    const auto bs = resp.find("blockchain_state");
    if (bs == resp.end() || !bs->is_object()) {
        return out;
    }
    const auto cost = bs->find("mempool_cost");
    const auto max  = bs->find("mempool_max_total_cost");
    const auto fees = bs->find("mempool_min_fees");
    std::uint64_t cost_v = 0;
    std::uint64_t max_v  = 0;
    if (cost == bs->end() || max == bs->end() || fees == bs->end()
        || !detail::read_count(*cost, cost_v) || !detail::read_count(*max, max_v)
        || !fees->is_object()) {
        return out;
    }
    const auto rate = fees->find("cost_5000000");
    if (rate == fees->end() || !rate->is_number()) {
        return out;
    }
    const double r = rate->get<double>();
    if (!std::isfinite(r) || r < 0.0) {
        return out;
    }
    out.cost            = cost_v;
    out.max_total_cost  = max_v;
    out.min_fee_rate_5m = r;
    out.known           = out.max_total_cost > 0U;
    return out;
}

/// The rate the node itself will refuse a spend of `spend_cost` below, in
/// mojos per cost; 0 when its mempool has room (it then admits any fee) or
/// when the state is unknown.
///
/// mempool.py at_full_capacity(cost) is total + cost > max.  At capacity
/// mempool_manager.py refuses fee/cost < 5, then refuses fee/cost <= the rate
/// of the cheapest items it would have to evict (get_min_fee_rate).  The
/// blockchain state only publishes that second rate for a 5M-cost spend; a
/// larger spend evicts more and can need more, so this is a floor on the
/// floor -- the controller's feedback covers the rest.
[[nodiscard]] inline double admission_floor_rate(const MempoolState& m,
                                                 std::uint64_t       spend_cost) noexcept
{
    if (!m.known) {
        return 0.0;
    }
    const bool at_capacity = m.cost > m.max_total_cost
                          || spend_cost > m.max_total_cost - m.cost;
    if (!at_capacity) {
        return 0.0;
    }
    return m.min_fee_rate_5m > kNonzeroFeeMinimumFpc ? m.min_fee_rate_5m : kNonzeroFeeMinimumFpc;
}

}  // namespace xop::rpc

#endif  // XOP_RPC_NODE_REQUESTS_HPP
