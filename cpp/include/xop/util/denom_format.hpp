#ifndef XOP_UTIL_DENOM_FORMAT_HPP
#define XOP_UTIL_DENOM_FORMAT_HPP
// ---------------------------------------------------------------------------
// denom_format.hpp -- fmt/spdlog formatter for Denominated<Tag>.
//
// [2026-09-02] WHY THIS IS A SEPARATE HEADER FROM denom.hpp.
//
// denom.hpp is a PURE header -- <compare>, <cstdint>, <type_traits> and
// xop::types, nothing else -- which is this repo's convention for testable logic
// and is what lets cpp/tests/test_denom.cpp drive it in isolation. Pulling fmt
// into it would drag a third-party dependency into every translation unit that
// wants a strong typedef, for the sake of a logging convenience. So the types
// live there and the formatter lives here, and only engine.cpp (which already
// includes spdlog) pays for it.
//
// WHY A FORMATTER AT ALL, rather than writing `.v` at each spdlog call site.
// There are ~24 format arguments across ~20 spdlog statements in engine.cpp that
// print values produced by, or derived from, the three retyped functions. Those
// are LOG lines: they read a value and emit text, they do not participate in the
// money path, and forcing `.v` at each one adds two dozen unwraps whose only
// effect is to make the genuinely load-bearing `.v` extractions -- the ones at
// record_taker_fill(), at decide_funding(), at the database boundary -- harder
// to find in a grep. denom.hpp LIMIT 2 says extraction belongs at a boundary and
// must be greppable; a formatter keeps the log sites OFF that list, which makes
// the list mean something.
//
// This does not weaken the guarantee. Formatting is a read; it cannot transpose
// a denomination, and it cannot feed a wrong-denominated value back into
// arithmetic. The one thing it does cost is that a log line prints a bare number
// with no unit marker -- which is exactly what it printed before this change, so
// nothing regressed. Deliberately NOT decorated with a "base"/"quote" suffix:
// several of these lines are parsed by the ops tooling and by the log-scraping
// in scripts/, and changing the rendered text of a live bot's logs is a
// behaviour change that has no business riding along with a type change.
//
// The formatter INHERITS from fmt::formatter<Mojo>, so every existing format
// spec ({}, {:d}, width, fill) keeps working exactly as it did on the bare
// integer. parse() is inherited unchanged; only format() is overridden, and all
// it does is forward `.v`.
// ---------------------------------------------------------------------------

#include <spdlog/fmt/fmt.h>

#include "xop/util/denom.hpp"

// NOTE: this specialisation is deliberately declared in the fmt namespace on the
// PRIMARY template `xop::Denominated<Tag>`, so it covers BaseMojos, QuoteMojos,
// BaseMpu, QuoteMpu, OfferedMojos and any tag added later -- there is no
// per-alias formatter to forget to add.
template <class Tag>
struct fmt::formatter<xop::Denominated<Tag>> : fmt::formatter<xop::Mojo> {
    template <class FormatContext>
    auto format(const xop::Denominated<Tag>& d, FormatContext& ctx) const
    {
        return fmt::formatter<xop::Mojo>::format(d.v, ctx);
    }
};

#endif  // XOP_UTIL_DENOM_FORMAT_HPP
