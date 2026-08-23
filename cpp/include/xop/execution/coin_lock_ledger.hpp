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
// per posting cycle from the wallet's actual free-coin list and simulates
// coin selection for each candidate lock.  Two separated meanings (review):
//
//   THE POOL MODELS PHYSICS.  Every lock -- offer principal, offer fee,
//   cancel fee -- removes its selected coins, and the FLOOR is enforced
//   against what remains.  note_lock() is the unconditional drain for
//   spends that happen regardless of budget (cancel fees).
//
//   THE CAP MODELS POLICY.  Only OFFER locks (try_lock) count against the
//   per-cycle commitment cap: the configured knob is "the fraction of free
//   XCH one posting cycle may lock into offers", and cancel fees consuming
//   the posting budget starved requotes.
//
// Selection is wallet-shaped: when the sub-need coins sum past the need
// -- the case where Chia's wallet knapsacks smaller coins -- a
// largest-first accumulation of those sub-need coins is used (matching
// chia coin_selection.py's descending sort and largest-first fallback;
// authoritative rather than a bigger-charge candidate, because modelling
// different coin identities than the wallet locks makes later admissions
// undercount); otherwise the smallest single covering coin.  The real
// knapsack is randomized, so this is a bounded approximation -- see
// select_for for the layers that bound the divergence.
//
// Pure and synchronous so it is unit-testable in isolation
// (tests/test_coin_lock_ledger.cpp); the async snapshot fetch lives in
// OfferManager::begin_xch_lock_cycle().
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
    ///                         lock INTO OFFERS; clamped to [0, 1].
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
        // NaN-proof clamp: !(x >= 0) catches NaN without <cmath>, and a
        // NaN reaching the multiplication below would be undefined
        // behaviour at the float-to-Mojo conversion.  Garbage input fails
        // CLOSED (cap 0: no spend-side posting) -- the config parser
        // rejects non-finite values, this guards independent callers.
        if (!(commit_frac >= 0.0)) commit_frac = 0.0;
        if (commit_frac > 1.0) commit_frac = 1.0;
        const Mojo commitable =
            remaining_ > floor_ ? remaining_ - floor_ : Mojo{0};
        cap_ = static_cast<Mojo>(
            static_cast<long double>(commitable) * commit_frac);
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] Mojo remaining() const noexcept { return remaining_; }
    /// XCH locked into OFFERS this cycle (cancel-fee drains excluded).
    [[nodiscard]] Mojo committed() const noexcept { return committed_; }

    /// Admit-or-refuse one OFFER's XCH lock against both the cycle cap and
    /// the pool floor.  On admit, the selected coins leave the pool and the
    /// charge counts against the cap.  Refusal locks nothing.
    /// Over-counting after a failed create is the deliberate conservative
    /// direction.
    [[nodiscard]] bool try_lock(Mojo principal_mojos, Mojo fee_mojos)
    {
        if (!active_) {
            return true;
        }
        const Mojo need = clamp_need(principal_mojos, fee_mojos);
        if (need == 0) {
            return true;
        }
        const Selection sel = select_for(need);
        if (!sel.covered) {
            return false;  // pool cannot fund this lock at all
        }
        if (saturating_add(committed_, sel.locked) > cap_) {
            return false;  // per-cycle OFFER commitment cap
        }
        // The floor is a live, independent check: cancel-fee drains
        // (note_lock) shrink the pool without consuming the cap, so the cap
        // passing does NOT imply the floor holds.  Pinned by
        // FloorRefusesAfterCancelDrainsEvenWhenCapAdmits.
        if (remaining_ - sel.locked < floor_) {
            return false;
        }
        apply(sel);
        committed_ = saturating_add(committed_, sel.locked);
        return true;
    }

    /// Floor-aware, CAP-EXEMPT admission for buy-XCH offers (review
    /// round 3): recovery exemptions may skip the per-cycle offer cap --
    /// buy-XCH offers net-increase XCH when filled -- but must never skip
    /// the fee-reserve floor, or the exemption itself can walk the pool to
    /// zero (the incident's five CAT-principal bids would have done
    /// exactly that through an unconditional bypass).  The charge does not
    /// count against committed(): the cap remains a spend-side budget.
    [[nodiscard]] bool try_lock_floor_only(Mojo principal_mojos,
                                           Mojo fee_mojos)
    {
        if (!active_) {
            return true;
        }
        const Mojo need = clamp_need(principal_mojos, fee_mojos);
        if (need == 0) {
            return true;
        }
        const Selection sel = select_for(need);
        if (!sel.covered) {
            return false;
        }
        if (remaining_ - sel.locked < floor_) {
            return false;
        }
        apply(sel);
        return true;
    }

    /// Unconditional pool drain for spends that happen REGARDLESS of
    /// budget -- cancel fees (review: cancels run after the cycle snapshot,
    /// and a secure cancel locks a whole XCH fee coin the snapshot still
    /// counts as free).  Never refuses, never consumes the offer cap; a
    /// need the pool cannot cover drains the pool entirely.
    void note_lock(Mojo principal_mojos, Mojo fee_mojos)
    {
        if (!active_) {
            return;
        }
        const Mojo need = clamp_need(principal_mojos, fee_mojos);
        if (need == 0) {
            return;
        }
        const Selection sel = select_for(need);
        if (sel.covered) {
            apply(sel);
            return;
        }
        coins_.clear();
        remaining_ = 0;
    }

private:
    struct Selection {
        bool        covered{false};
        Mojo        locked{0};
        bool        single{false};
        std::size_t single_idx{0};
        std::size_t prefix_count{0};  // how many sub-need coins (largest-first)
        std::size_t tail_start{0};    // index of the first removed coin
    };

    [[nodiscard]] static Mojo clamp_need(Mojo principal, Mojo fee) noexcept
    {
        if (principal < 0) principal = 0;
        if (fee < 0) fee = 0;
        return saturating_add(principal, fee);
    }

    /// Wallet-shaped selection (see header).  coins_ is sorted ascending,
    /// so sub-need coins are a prefix and the smallest single covering
    /// coin is the first element at/after the boundary.  The knapsack
    /// prefix, when applicable, is what the wallet will actually lock --
    /// identity fidelity matters as much as value (review round 5).
    [[nodiscard]] Selection select_for(Mojo need) const
    {
        Selection single_sel;
        const auto it = std::lower_bound(coins_.begin(), coins_.end(), need);
        if (it != coins_.end()) {
            single_sel.covered    = true;
            single_sel.locked     = *it;
            single_sel.single     = true;
            single_sel.single_idx =
                static_cast<std::size_t>(it - coins_.begin());
        }

        // Knapsack model, applicable when the sub-need coins alone can
        // cover the need (Chia prefers them then).  Accumulation is
        // LARGEST-first among the sub-need coins, matching the descending
        // sort chia's coin_selection.py applies before its knapsack and
        // its documented largest-first fallback.  EXACTNESS IS UNATTAINABLE:
        // the real knapsack samples random subsets, so no deterministic
        // model can track the wallet's coin identities perfectly.  The
        // divergence is bounded by three layers: the ledger reseeds from
        // the real coin list every cycle (~1 min), the pre-existing
        // per-offer wallet re-query guards remain as backstops, and the
        // ledger fails open when the snapshot is unavailable.  The floor
        // is defence-in-depth against the incident's systematic walk to
        // zero, not a byte-exact wallet simulation.
        Selection tail_sel;
        {
            Mojo        covered = 0;
            std::size_t count   = 0;
            const auto  sub_need_end =
                static_cast<std::size_t>(it - coins_.begin());
            while (count < sub_need_end && covered < need) {
                covered = saturating_add(
                    covered, coins_[sub_need_end - 1 - count]);
                ++count;
            }
            if (covered >= need) {
                tail_sel.covered      = true;
                tail_sel.locked       = covered;
                tail_sel.prefix_count = count;   // largest sub-need coins
                tail_sel.tail_start   = sub_need_end - count;
            }
        }

        // When the knapsack model applies, it is AUTHORITATIVE, not merely
        // a bigger charge: the wallet will pick those smaller coins, and
        // modelling a different identity (review round 5: {4,4,9} with
        // needs 8 then 4 -- the wallet locks {4,4} then 9 and ends at 0,
        // while a max-charge model locked 9 then 4 and reported 4 left)
        // makes LATER admissions undercount even when this one over-counts.
        if (tail_sel.covered) {
            return tail_sel;
        }
        if (single_sel.covered) {
            return single_sel;
        }
        return Selection{};  // pool cannot cover the need
    }

    void apply(const Selection& sel)
    {
        if (sel.single) {
            coins_.erase(coins_.begin()
                         + static_cast<std::ptrdiff_t>(sel.single_idx));
        } else {
            coins_.erase(
                coins_.begin() + static_cast<std::ptrdiff_t>(sel.tail_start),
                coins_.begin() + static_cast<std::ptrdiff_t>(
                                     sel.tail_start + sel.prefix_count));
        }
        remaining_ -= sel.locked;
    }

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
    Mojo              committed_{0};  // OFFER locks only
    bool              active_{false};
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_COIN_LOCK_LEDGER_HPP
