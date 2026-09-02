#ifndef XOP_UTIL_DENOM_HPP
#define XOP_UTIL_DENOM_HPP
// ---------------------------------------------------------------------------
// denom.hpp -- strong typedefs that make a DENOMINATION TRANSPOSITION fail to
//              compile.
//
// [TODO S36 increment, 2026-09-02] WHY THIS EXISTS
// ------------------------------------------------
// Nothing in cpp/tests constructs an Engine, so every money-path call site in
// engine.cpp is an unguarded line: mis-wire one and the full suite stays green.
// Full dependency injection was evaluated and rejected (~30 concrete
// collaborators, one virtual in the whole RPC/execution graph, type erasure and
// per-call heap allocation on a production bot's RPC path). For the DENOMINATION
// class of error specifically, a strong typedef dominates it: the check lands at
// COMPILE time, costs nothing at run time because these wrappers compile away,
// and a compile error cannot be forgotten, skipped or deleted the way a test can.
// CI runs a second compiler (GCC) that evaluates the static_asserts below, which
// is the toolchain this repo's five MSVC-pass/GCC-fail defects were only ever
// visible on.
//
// THE BUG IT IS BUILT AGAINST -- the Step 9e peg-arb denomination defect. A BID
// carries a QUOTE-denominated size (the maker offers quote and requests base),
// and Step 9e treated it as base mojos in three places: the cap comparison, the
// pre-trade funding estimate, and BOTH double-entry ledger legs. On BYC/wUSDC.b
// the error is invisible because base and quote denominations are both 1000; on
// any pair where they differ the cap is applied in the wrong unit. See the
// reconstruction of that exact defect as a compile-rejection in
// take_sizing.hpp's ACCEPTANCE block.
//
// ===========================================================================
// THREE LIMITS. Read all three before trusting anything below.
// ===========================================================================
//
// LIMIT 1 -- TYPES CANNOT SEE VALUES.
//   A QuoteMojos holding a base-denominated number still type-checks. The
//   constructor is explicit, which stops an IMPLICIT conversion, but it does
//   not and cannot stop a call site from writing `BaseMojos{co.size}` on a bid
//   and laundering the wrong number through the type. These typedefs stop
//   TRANSPOSITION -- passing the right value in the wrong argument slot, or a
//   correctly-typed value into a slot expecting the other denomination. They do
//   not stop CORRUPTION. Do not over-trust them and do not write a commit
//   message that says they make denomination errors impossible.
//
//   The mitigation for corruption is NOT a type. It is to remove the hand-wrap
//   from the call sites entirely: see classify_offer_size() in take_sizing.hpp,
//   which is the ONE function licensed to receive a runtime-denominated size
//   next to its Side, and which is reachable from a test today. Everywhere a
//   `BaseMojos{...}` or `QuoteMojos{...}` literal appears in engine.cpp is a
//   place a human judgement is still load-bearing, and those are now greppable.
//
// LIMIT 2 -- THE WRAPPER MUST NOT LEAK.
//   If `.v` is extracted early and the bare Mojo is passed around, the guarantee
//   is gone from that point on. Extraction belongs at a BOUNDARY -- a database
//   write, a log argument, an RPC field, record_taker_fill() -- and must be an
//   explicit `.v`, never an implicit conversion. There is deliberately no
//   `operator Mojo()`, so every extraction is greppable as `.v`.
//
//   `.v` IS ALSO A WRITE VECTOR, and a grep for `.v` will not tell the two
//   apart. The member is public and non-const, so `legs.base.v = legs.quote.v;`
//   compiles with no diagnostic and launders a quote-denominated number into a
//   BaseMojos -- the same corruption as a hand-wrapped `BaseMojos{co.size}`
//   (LIMIT 1), but invisible to a reader who has been told that every `.v` is a
//   boundary extraction. Audit ASSIGNMENTS THROUGH `.v` separately from reads:
//   `grep -nE '\.v\s*=[^=]'`. No such cross-tag write exists in the tree as of
//   2026-09-02; this is written down so that stays a checked fact rather than
//   an assumption.
//
//   Known leaks that this increment does NOT close, recorded rather than
//   quietly tolerated:
//     * engine.cpp Step 8's `std::vector<Mojo> costs` fed to
//       tiers_within_budget(): the vector is filled inside a
//       `for (Side side : {Ask, Bid})` loop, so it is RUNTIME-denominated and
//       cannot carry a single static tag without templating shared machinery.
//     * take_retry.hpp's SpendableReading::spendable is a wallet balance whose
//       denomination is the SPEND asset -- quote for an ask take, base for a bid
//       take. Typing the cost without typing the reading only moves the unwrap
//       one function later, so ask_take_cost() unwraps at its own boundary.
//
// LIMIT 3 -- THE SPEND ASSET IS IRREDUCIBLY RUNTIME.
//   Which asset leaves our wallet is quote for an ask take and base for a bid
//   take. That is a VALUE, not a type, and no static tag can express it. What
//   the types buy is that once the branch has selected an account to debit, both
//   branches carry correctly-typed amounts, so a swap between them is a compile
//   error. The ternary survives; what it selects changes from "which formula" to
//   "which account", which is the part that can be got right by inspection.
//
// PRICE IS DELIBERATELY NOT WRAPPED. `price` on this path is a pseudo-unit
// ratio -- quote_units_per_base_unit scaled by kMojosPerXch (types.hpp:37-47) --
// not an amount of anything, so there is no denomination to transpose. It stays
// a bare Mojo. That is not an oversight; wrapping it would ripple through
// CompetingOffer, TierQuote and every price comparison in the engine for no
// gain against the bug class this header targets.
//
// Pure header: <compare>, <cstdint> and xop::types. No engine types, no asio, no
// spdlog, no fmt. The fmt formatter lives in denom_format.hpp precisely so that
// this file stays testable in isolation. Driven by cpp/tests/test_denom.cpp.
// ---------------------------------------------------------------------------

#include <compare>
#include <cstdint>
#include <type_traits>

#include "xop/types.hpp"

namespace xop {

// ---------------------------------------------------------------------------
// Denominated<Tag> -- a Mojo count that knows which asset it counts.
//
// SHAPE NOTES, each load-bearing:
//
//   * The constructor is EXPLICIT. This is the whole mechanism. Without it,
//     every existing bare-Mojo call site keeps compiling and the increment is
//     theatre. With it, `quote_cost_for_ask(co.size, ...)` -- the shape the
//     Step 9e defect was written in -- stops compiling and a human has to say
//     which denomination `co.size` is in.
//
//   * There is NO `operator Mojo()`. Extraction is `.v`, always, and therefore
//     greppable. See LIMIT 2.
//
//   * `operator<=>` AND `operator==` are BOTH defaulted explicitly. C++20
//     [class.compare.default]/4 does implicitly declare `==` from a defaulted
//     `<=>`, so the second line is redundant by the letter of the standard --
//     it is written out anyway because this is exactly the kind of subtlety
//     that differs between MSVC and GCC in practice, and because
//     `static_assert(a == b)` and gtest's EXPECT_EQ both depend on it. The
//     redundancy costs nothing and removes a toolchain question.
//
//   * Comparison and arithmetic are same-tag ONLY. `BaseMojos < QuoteMojos`
//     does not compile, and that is the assertion this whole file exists to
//     make. There is no cross-tag operator anywhere and none must ever be added.
//
//   * `+`, `-` and `+=` are provided for the TWO live Denominated arithmetic
//     sites that exist, and they are named here precisely so nobody generalises
//     from them: engine.cpp's Step 9f budget subtraction
//     (`max_take_mojos_total - taken_mojos_this_block`) and its budget
//     accumulator (`taken_mojos_this_block += cand.size`). BOTH ARE BOUNDED --
//     the accumulator is compared against the cap every iteration and the loop
//     breaks when it reaches it -- which is why plain signed addition is
//     sufficient THERE and nowhere else by default.
//
//     THESE OPERATORS DO NOT SATURATE, and that is a real limitation rather
//     than an oversight. This codebase deliberately routes every UNBOUNDED
//     accumulation through engine.cpp's saturating_add_mojo() (four sites, all
//     on bare Mojo: pending-spend at 9756/9764 and the pending-plus-new tier
//     totals at 11101/11106), because take_retry.hpp states the consequence
//     outright -- "A wrapped cost would read as affordable". Signed overflow is
//     also UB, so a wrapped total is not merely wrong, it is undefined.
//
//     DO NOT write a new pending-spend or cost accumulator as `total += x` on a
//     Denominated. There is no saturating_add_mojo overload for this type; if
//     you need one, extract at the boundary (`.v`), saturate, and re-wrap --
//     or add a saturating operator here and update this note. An earlier draft
//     of this comment justified these operators by citing the pending-spend
//     accumulation and `shortfall = cost - spendable`; both of those are
//     bare-Mojo sites that do NOT use this type, and citing them made the
//     unsaturated form look blessed for exactly the case it is wrong for.
//
//     `*` and `/` between two Denominated are NOT provided: mojos times mojos
//     is not mojos, and anything wanting that arithmetic wants the exact kernel
//     in take_sizing.hpp instead.
//
//   * Wrapping is zero-cost. This is a single int64_t member with constexpr
//     operations; every one of them compiles away. The DEPLOYED binary's
//     arithmetic does not move, which is the precondition for retyping a live
//     trading bot's money path at all.
//
//   * DEFAULT INITIALISER on the member, per this branch's convention, so a
//     default-constructed Denominated is a hard zero rather than garbage. Zero
//     on this path means DECLINE (take_sizing.hpp's contract), so a forgotten
//     value fails CLOSED.
// ---------------------------------------------------------------------------
template <class Tag>
struct Denominated {
    Mojo v{0};

    constexpr Denominated() noexcept = default;
    constexpr explicit Denominated(Mojo m) noexcept : v(m) {}

    // A FLOATING-POINT argument reaches the Mojo constructor above through an
    // ordinary standard conversion, so `BaseMojos(max_units * mpu)` -- verbatim
    // the Step 9f inline cap this increment removed from engine.cpp on the
    // grounds that narrowing an out-of-range double is UB -- is well-formed
    // C++ without this deletion.
    //
    // MSVC rejects it only because /WX promotes C4244. GCC, which this repo
    // names as the authority and which is the toolchain CI builds on, does NOT
    // diagnose it at all: -Wall -Wextra -Wpedantic do not enable -Wconversion
    // or -Wfloat-conversion. The guard would therefore have been supplied by a
    // WARNING FLAG on one compiler and by nothing at all on the other -- i.e.
    // exactly the MSVC-passes/GCC-fails shape this file exists to prevent.
    // Deleting the overload makes it a TYPE error on both, independent of
    // warning configuration.
    //
    // Verified 2026-09-02, not assumed: at /W4 WITHOUT /WX the parenthesised
    // form compiled with exit 0 and only `warning C4244: conversion from
    // 'double' to 'xop::Mojo'`. With this deletion it is C2280 unconditionally.
    // Note the braced form `BaseMojos{3.7}` was already ill-formed (narrowing
    // in list-initialisation); it is the PARENTHESISED form that got through,
    // which is why the assertion pinning this uses `To(f)` and not `To{f}`.
    template <class F>
        requires std::is_floating_point_v<F>
    constexpr explicit Denominated(F) noexcept = delete;

    friend constexpr auto operator<=>(Denominated, Denominated) = default;
    friend constexpr bool operator==(Denominated, Denominated)  = default;

    friend constexpr Denominated operator+(Denominated a, Denominated b) noexcept
    {
        return Denominated{a.v + b.v};
    }
    friend constexpr Denominated operator-(Denominated a, Denominated b) noexcept
    {
        return Denominated{a.v - b.v};
    }

    constexpr Denominated& operator+=(Denominated o) noexcept
    {
        v += o.v;
        return *this;
    }
};

// ---------------------------------------------------------------------------
// The four tags. Incomplete struct types declared inline in the alias: they are
// never defined and never instantiated, they exist only to make the four
// specialisations distinct types.
//
//   BaseMojos  -- an amount of the BASE asset, in that asset's mojos.
//   QuoteMojos -- an amount of the QUOTE asset, in that asset's mojos.
//   BaseMpu    -- PairConfig::base_mojos_per_unit  (1e12 for XCH, 1000 for a CAT).
//   QuoteMpu   -- PairConfig::quote_mojos_per_unit.
//
// The Mpu pair are separated for the same reason as the amounts:
// quote_cost_for_ask takes both, adjacent, in the same type, and swapping them
// is a silent 1e9x error on any pair whose denominations differ. That
// transposition is now a compile error -- see NoMpuTransposition below.
// ---------------------------------------------------------------------------
using BaseMojos  = Denominated<struct BaseTag>;
using QuoteMojos = Denominated<struct QuoteTag>;
using BaseMpu    = Denominated<struct BaseMpuTag>;
using QuoteMpu   = Denominated<struct QuoteMpuTag>;

// ---------------------------------------------------------------------------
// OfferedMojos -- the honest type for a CompetingOffer's advertised size.
//
// CompetingOffer::size is denominated in the OFFERED asset: base for an ask,
// quote for a bid (engine.cpp's ingest decides side and denomination from the
// same field, `orec.offered[0].id`, five lines apart, so this is an invariant of
// ONE expression rather than two coincidences). That is a runtime fact and no
// static tag can resolve it.
//
// What this tag DOES buy: an OfferedMojos cannot be compared against a BaseMojos
// cap, cannot be handed to record_taker_fill(), and cannot be passed to
// quote_cost_for_ask() -- so the only thing a holder can do with one is send it
// through classify_offer_size(), which takes the Side alongside it and returns
// both legs correctly typed. The unresolved denomination becomes unusable rather
// than merely undocumented.
// ---------------------------------------------------------------------------
using OfferedMojos = Denominated<struct OfferedTag>;

// ===========================================================================
// COMPILE-TIME GUARD -- the negative cases.
//
// A test that passes proves the code works. A static_assert that a WRONG call
// does NOT compile proves the wrong code cannot ship, which is strictly
// stronger: it holds on every translation unit that includes this header, on
// both toolchains, and it cannot be skipped or deleted without the build going
// red. These are evaluated by whichever compiler is building, so GCC checks them
// on CI itself.
//
// THERE ARE 24 OF THEM. Keep this count in step if you add one -- the same
// mutation-harness reasoning as take_sizing.hpp's block. `grep -c
// '^static_assert' cpp/include/xop/util/denom.hpp` returning anything else means
// you are looking at a partially-mutated file, not at the fix.
// ===========================================================================

// -- Distinctness. The foundation: if these two were the same type, every
//    rejection below would be vacuous rather than true.
static_assert(!std::is_same_v<BaseMojos, QuoteMojos>);
static_assert(!std::is_same_v<BaseMpu, QuoteMpu>);

// -- No implicit conversion from a bare Mojo. This is what makes every existing
//    call site stop compiling and demand a human judgement.
static_assert(!std::is_convertible_v<Mojo, BaseMojos>);
static_assert(!std::is_convertible_v<Mojo, QuoteMojos>);

// -- No implicit conversion OUT. This is LIMIT 2 enforced rather than merely
//    documented: the wrapper cannot silently decay back to a bare Mojo and be
//    passed to an untyped overload.
static_assert(!std::is_convertible_v<BaseMojos, Mojo>);
static_assert(!std::is_convertible_v<QuoteMojos, Mojo>);

// -- No cross-denomination conversion, in either direction.
static_assert(!std::is_convertible_v<BaseMojos, QuoteMojos>);
static_assert(!std::is_convertible_v<QuoteMojos, BaseMojos>);

// -- Explicit construction from the other denomination is ALSO rejected. Worth
//    asserting separately from convertibility: `explicit` blocks implicit
//    conversion, and a reader could reasonably assume a static_cast still gets
//    through. It does not -- there is no constructor taking another
//    Denominated, so `BaseMojos{some_quote_mojos}` is ill-formed too. The only
//    way across the boundary is `.v`, which is greppable.
template <class To, class From>
concept ExplicitlyConstructibleFrom = requires(From f) { To{f}; };
static_assert( ExplicitlyConstructibleFrom<BaseMojos, Mojo>);
static_assert(!ExplicitlyConstructibleFrom<BaseMojos, QuoteMojos>);
static_assert(!ExplicitlyConstructibleFrom<QuoteMojos, BaseMojos>);

// -- No FLOATING-POINT construction, pinning the deleted constructor above.
//
//    THIS CONCEPT USES `To(f)` AND NOT `To{f}` AND THE DIFFERENCE IS THE WHOLE
//    ASSERTION. Braced initialisation from a double is already ill-formed
//    without the deleted constructor, because list-initialisation forbids
//    narrowing -- so writing this check as
//    `!ExplicitlyConstructibleFrom<BaseMojos, double>` would have PASSED
//    against the unfixed header and asserted nothing. That is the vacuous-guard
//    shape this branch has now found seven of. The parenthesised form is the
//    one that actually got through (verified: exit 0 at /W4 with only C4244),
//    so the parenthesised form is what must be pinned.
//
//    Mutation check, run 2026-09-02: with the deleted constructor removed, the
//    `!ParenConstructibleFrom<BaseMojos, double>` line below FAILS to compile,
//    and the positive control keeps passing. The guard is not vacuous.
template <class To, class From>
concept ParenConstructibleFrom = requires(From f) { To(f); };
static_assert( ParenConstructibleFrom<BaseMojos, Mojo>);     // positive control
static_assert(!ParenConstructibleFrom<BaseMojos, double>);
static_assert(!ParenConstructibleFrom<QuoteMojos, double>);

// -- Cross-tag COMPARISON does not compile. This is the operator that the Step
//    9e cap check `base_sz > max_mojos` is written in, and it is the single
//    most important rejection in this file.
template <class A, class B>
concept Comparable = requires(A a, B b) { a > b; };
static_assert( Comparable<BaseMojos, BaseMojos>);
static_assert(!Comparable<BaseMojos, QuoteMojos>);
static_assert(!Comparable<QuoteMojos, BaseMojos>);

// -- A wrapper cannot be compared against a bare Mojo either. Without this, the
//    cap check could be "fixed" by leaving one side unwrapped and the guarantee
//    would evaporate at exactly the site it was built for.
//
//    BOTH ORIENTATIONS ARE PINNED, and the second line is the one that matters
//    most: the Step 9e defect was written `if (c.size > max_mojos)`, i.e. with
//    the BARE Mojo ON THE LEFT. Until now only the mirror was asserted here and
//    in take_sizing.hpp (`BaseComparableWith<TakeLegs, Mojo>` is `l.base > c`),
//    so the defect's own spelling was machine-checked nowhere -- it survived
//    only inside the commented STEP_9E_DEFECT_SNIPPET, which no compiler
//    evaluates. C++20's rewritten and synthesized candidates make the two
//    directions fail together TODAY (verified 2026-09-02), so this is a
//    guard-coverage gap rather than a live hole; but a future one-sided
//    heterogeneous operator -- say an `operator>(Mojo, BaseMojos)` added for a
//    log comparison -- would leave the first line green while recompiling the
//    defect verbatim.
static_assert(!Comparable<BaseMojos, Mojo>);
static_assert(!Comparable<Mojo, BaseMojos>);   // <-- the defect's OWN orientation

// -- Sanity, and a real property: the wrappers are zero-overhead. If this ever
//    fails, the "ZERO runtime change" claim in this header's banner is false and
//    the whole cost/benefit argument for choosing typedefs over DI collapses.
static_assert(sizeof(BaseMojos) == sizeof(Mojo));

// -- Positive controls. Same-tag arithmetic and comparison must actually WORK,
//    and must be constexpr, or the negatives above are trivially satisfiable by
//    a type that does nothing at all.
static_assert(BaseMojos{7} + BaseMojos{5} == BaseMojos{12});
static_assert(BaseMojos{7} - BaseMojos{5} == BaseMojos{2});
static_assert(BaseMojos{} == BaseMojos{0});          // fails CLOSED at zero
static_assert(BaseMojos{5} < BaseMojos{7});

}  // namespace xop

#endif  // XOP_UTIL_DENOM_HPP
