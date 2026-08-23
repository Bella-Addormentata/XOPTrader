// coin_lock_ledger.hpp -- self-accounted XCH coin-lock budget for one
// posting cycle.
//
// [XCH-LOCK-LEDGER 2026-08-23] Why the wallet's own numbers are not enough:
// the offer manager re-queried get_wallet_balance before EVERY offer, yet a
// batch of 10 offers still walked spendable XCH from 14.59 to 0 in 73
// seconds -- a just-created offer's coin locks do not appear in
// spendable_balance until the wallet processes them, so every re-query in a
// fast batch reads the same stale figure.  The reserve projection had the
// dual defect: it modelled notional fill amounts, while the wallet locks
// whole UTXOs (a 1-XCH ask locks a ~2-XCH coin) plus an XCH fee coin for
// EVERY offer -- including offers whose principal is not XCH at all.
//
// This ledger fixes both by accounting for our own locks: it is seeded once
// per posting cycle from the wallet's actual free-coin list, simulates the
// wallet's coin selection for each candidate offer (smallest single covering
// coin, else largest-first accumulation), and refuses any lock that would
// leave the remaining pool below the floor or push the cycle's committed
// total past the cap.  Pure and synchronous so it is unit-testable in
// isolation (tests/test_coin_lock_ledger.cpp); the async snapshot fetch
// lives in OfferManager::begin_xch_lock_cycle().
//
// A default-constructed ledger is INACTIVE and admits everything: the
// snapshot fetch is best-effort, and failing open leaves the pre-existing
// wallet-requery guards as the (stale but present) backstop rather than
// bricking all posting on one RPC hiccup.

#ifndef XOP_EXECUTION_COIN_LOCK_LEDGER_HPP
#define XOP_EXECUTION_COIN_LOCK_LEDGER_HPP

#include "xop/types.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace xop::execution {

class CoinLockLedger {
public:
    /// Inactive ledger: admits every lock (see header comment).
    CoinLockLedger() = default;

    /// @param free_coin_mojos  Amounts of the wallet's spendable coins.
    /// @param floor_mojos      Unlocked total that must survive the cycle.
    /// @param commit_frac      Fraction of (total - floor) one cycle may
    ///                         commit; clamped to [0, 1].
    CoinLockLedger(std::vector<Mojo> free_coin_mojos,
                   Mojo              floor_mojos,
                   double            commit_frac)
        : coins_(std::move(free_coin_mojos))
        , floor_(floor_mojos < 0 ? Mojo{0} : floor_mojos)
        , active_(true)
    {
        coins_.erase(std::remove_if(coins_.begin(), coins_.end(),
                                    [](Mojo c) { return c <= 0; }),
                     coins_.end());
        std::sort(coins_.begin(), coins_.end());
        for (Mojo c : coins_) {
            remaining_ = saturating_add(remaining_, c);
        }
        if (commit_frac < 0.0) commit_frac = 0.0;
        if (commit_frac > 1.0) commit_frac = 1.0;
        const Mojo commitable =
            remaining_ > floor_ ? remaining_ - floor_ : Mojo{0};
        cap_ = static_cast<Mojo>(
            static_cast<long double>(commitable) * commit_frac);
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] Mojo remaining() const noexcept { return remaining_; }
    [[nodiscard]] Mojo committed() const noexcept { return committed_; }

    /// Charge a lock that will happen REGARDLESS of budget -- cancel
    /// fees.  Same wallet-shaped selection as try_lock, but never refuses:
    /// the pool simply reflects reality, so posting later in the cycle is
    /// gated against what cancellations actually consumed (review: the
    /// cycle snapshot is taken before Step 8's cancellation passes, and a
    /// secure cancel locks an XCH fee coin the snapshot still counts as
    /// free).  A need the pool cannot cover drains the pool entirely.
    void note_lock(Mojo principal_mojos, Mojo fee_mojos)
    {
        if (!active_) {
            return;
        }
        if (principal_mojos < 0) principal_mojos = 0;
        if (fee_mojos < 0) fee_mojos = 0;
        const Mojo need = saturating_add(principal_mojos, fee_mojos);
        if (need == 0) {
            return;
        }

        const auto it = std::lower_bound(coins_.begin(), coins_.end(), need);
        if (it != coins_.end()) {
            const Mojo locked = *it;
            coins_.erase(it);
            remaining_ -= locked;
            committed_ = saturating_add(committed_, locked);
            return;
        }
        Mojo covered = 0;
        while (!coins_.empty() && covered < need) {
            const Mojo c = coins_.back();
            covered = saturating_add(covered, c);
            coins_.pop_back();
            remaining_ -= c;
            committed_ = saturating_add(committed_, c);
        }
    }

    /// Admit-or-refuse one offer's XCH lock.  On admit, the selected coins
    /// leave the pool and count against the cycle cap.  Refusal locks
    /// nothing.  Over-counting is the deliberate failure direction: a
    /// create_offer that later fails leaves the ledger over-committed for
    /// the rest of the cycle, which only makes posting MORE conservative.
    [[nodiscard]] bool try_lock(Mojo principal_mojos, Mojo fee_mojos)
    {
        if (!active_) {
            return true;
        }
        if (principal_mojos < 0) principal_mojos = 0;
        if (fee_mojos < 0) fee_mojos = 0;
        const Mojo need = saturating_add(principal_mojos, fee_mojos);
        if (need == 0) {
            return true;
        }

        // Wallet-shaped selection: the smallest single coin that covers the
        // need; otherwise accumulate largest-first until covered.
        Mojo        locked = 0;
        bool        single = false;
        std::size_t single_idx = 0;
        std::size_t take_from_back = 0;

        const auto it = std::lower_bound(coins_.begin(), coins_.end(), need);
        if (it != coins_.end()) {
            locked = *it;
            single = true;
            single_idx = static_cast<std::size_t>(it - coins_.begin());
        } else {
            Mojo covered = 0;
            while (take_from_back < coins_.size() && covered < need) {
                const Mojo c = coins_[coins_.size() - 1 - take_from_back];
                covered = saturating_add(covered, c);
                locked  = saturating_add(locked, c);
                ++take_from_back;
            }
            if (covered < need) {
                return false;  // pool cannot fund this lock at all
            }
        }

        if (saturating_add(committed_, locked) > cap_) {
            return false;  // cycle commitment cap
        }
        if (remaining_ - locked < floor_) {
            return false;  // would leave the pool below the floor
        }

        if (single) {
            coins_.erase(coins_.begin()
                         + static_cast<std::ptrdiff_t>(single_idx));
        } else {
            coins_.resize(coins_.size() - take_from_back);
        }
        remaining_ -= locked;
        committed_ = saturating_add(committed_, locked);
        return true;
    }

private:
    [[nodiscard]] static Mojo saturating_add(Mojo a, Mojo b) noexcept
    {
        if (a > std::numeric_limits<Mojo>::max() - b) {
            return std::numeric_limits<Mojo>::max();
        }
        return a + b;
    }

    std::vector<Mojo> coins_;       // unlocked, sorted ascending
    Mojo              floor_{0};
    Mojo              cap_{std::numeric_limits<Mojo>::max()};
    Mojo              remaining_{0};
    Mojo              committed_{0};
    bool              active_{false};
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_COIN_LOCK_LEDGER_HPP
