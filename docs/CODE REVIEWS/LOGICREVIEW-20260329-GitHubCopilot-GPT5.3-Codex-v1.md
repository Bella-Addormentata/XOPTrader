# Logic Review - XOPTrader Trading Engine
**Date:** 2026-03-29  
**Reviewer:** GitHub Copilot (GPT-5.3-Codex)  
**Scope:** End-to-end heartbeat logic, unit consistency, risk gate ordering, offer sizing semantics  
**Method:** Step-trace and dimensional consistency review of active code paths

---

## Executive Summary

The 13-step orchestration remains structurally correct: data ingestion gates quote generation, risk checks execute before posting, and fill processing is confirmation-aware.

The primary remaining logic risk is **cross-asset unit consistency**. The codebase now has denomination-aware fields in `PairConfig`, but runtime conversion logic is still partially fixed to XCH constants and parser initialization does not set those denomination fields.

This creates a logical mismatch between strategy outputs, risk sizing, and execution amounts for non-XCH pair legs.

---

## Findings

### LR-1: Denomination contract is incomplete between config and execution (HIGH)
**Severity:** HIGH  
**Files:**
- [cpp/include/xop/config.hpp](../../cpp/include/xop/config.hpp#L82)
- [cpp/include/xop/config.hpp](../../cpp/include/xop/config.hpp#L86)
- [cpp/src/config.cpp](../../cpp/src/config.cpp#L277)
- [cpp/src/config.cpp](../../cpp/src/config.cpp#L465)

**Logic issue:** Denomination fields exist in the data model but are not populated in parsing logic. Execution code assumes they are meaningful.

This violates a core interface invariant:

$$
\text{PairConfig.denominations must be initialized before sizing/execution math.}
$$

**Consequence:** downstream formulas can use wrong units even when control flow is otherwise correct.

---

### LR-2: Quote/size conversion still assumes XCH scale in generic pair flow (HIGH)
**Severity:** HIGH  
**Files:**
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1976)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1978)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1980)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1982)

**Logic issue:** Step 6 transforms strategy outputs to integer quote/size using `kMojosPerXch` in pair-generic logic.

For mixed assets, this violates unit consistency:

$$
\text{price\_mojos} = \text{price\_display} \times \text{quote\_mojos\_per\_unit}
$$

$$
\text{size\_mojos} = \text{size\_display} \times \text{base\_mojos\_per\_unit}
$$

Current fixed-XCH scaling can over/under-size by large factors on CAT legs.

---

### LR-3: Fill-derived analytics volume normalization is XCH-fixed (MEDIUM)
**Severity:** MEDIUM  
**File:**
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1449)

**Logic issue:** Fill volume passed to VPIN normalization divides by `kMojosPerXch` regardless of pair base denomination.

For CAT-base pairs, this can suppress measured toxicity magnitude and bias multipliers used later in spread logic.

---

### LR-4: Core risk and orchestration ordering still holds (VERIFIED)
**Severity:** INFO  
**Files:**
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1600)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1714)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1951)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L2208)

**Verified behavior:**
1. Step 1 validity gates Step 4+ per pair.
2. Staleness/depeg checks prevent quote generation.
3. Risk constraints apply before ladder generation and posting.
4. Offer posting remains confirmation and fee-gated.

No fresh sequencing regressions were found in the heartbeat pipeline.

---

## Open Assumptions

1. Assumed CAT display denomination is `1e3` mojos per unit, as documented in code comments.
2. Assumed strategy-level `mid` is quote-display-per-base-display value, requiring per-asset denomination conversion at boundaries.
3. If internal canonical price units are intentionally XCH-normalized, this contract is currently undocumented and conflicts with `PairConfig` denomination fields.

---

## Recommended Logic Fix Pattern

1. Initialize `base_mojos_per_unit` and `quote_mojos_per_unit` in parser.
2. Introduce explicit conversion helpers used everywhere:
   - `price_display_to_mojos(price, quote_denom)`
   - `size_display_to_mojos(size, base_denom)`
   - `fill_mojos_to_display(fill_size, base_denom)`
3. Add invariant checks in debug and tests:
   - `base_denom > 0`, `quote_denom > 0`
   - known pair fixtures produce expected integer offer_dict values.

---

## Conclusion

Control-flow logic is solid, but **unit-boundary logic for multi-asset denominations is not yet end-to-end coherent**. This is currently the highest-impact logical correctness risk remaining for paper/live trading on non-XCH pair legs.
