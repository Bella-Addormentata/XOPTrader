// engine_source_scan.hpp -- shared helpers for the engine.cpp source scans.
//
// [PACE round 2, PR #160] xop_tests constructs no Engine (S36), so glue in
// engine.cpp cannot be driven by a unit test.  The wiring tests
// (test_pace_wiring.cpp, test_sizing_wiring.cpp) read engine.cpp itself, from
// the XOP_TESTS_ENGINE_CPP path tests/CMakeLists.txt defines, and pin which
// function is called, where, and with which inputs.
//
// Every whitespace character is removed from both the source and the needle
// before comparing, so re-wrapping a line does not break a scan; renaming or
// reordering does.

#ifndef XOP_TESTS_ENGINE_SOURCE_SCAN_HPP
#define XOP_TESTS_ENGINE_SOURCE_SCAN_HPP

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

namespace xop_tests::engine_scan {

inline std::string read_engine_cpp()
{
    std::ifstream in(XOP_TESTS_ENGINE_CPP, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

inline std::string squeeze(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            out.push_back(c);
        }
    }
    return out;
}

/// The squeezed text of the function whose definition begins with
/// `signature`, up to the first line holding only "}" (engine.cpp closes
/// every function at column 0).  Empty when the signature is absent.
inline std::string function_body(const std::string& raw, const std::string& signature)
{
    const std::size_t start = raw.find(signature);
    if (start == std::string::npos) {
        return {};
    }
    std::size_t at = start;
    while (true) {
        at = raw.find("\n}", at);
        if (at == std::string::npos) {
            return squeeze(raw.substr(start));
        }
        const std::size_t after = at + 2;
        if (after >= raw.size() || raw[after] == '\r' || raw[after] == '\n') {
            return squeeze(raw.substr(start, after - start));
        }
        at = after;
    }
}

inline std::size_t position_of(const std::string& body, const std::string& needle)
{
    return body.find(squeeze(needle));
}

/// How many times `needle` (squeezed) occurs in `body`, non-overlapping.
inline std::size_t count_of(const std::string& body, const std::string& needle)
{
    const std::string squeezed = squeeze(needle);
    if (squeezed.empty()) {
        return 0;
    }
    std::size_t n = 0;
    for (std::size_t at = body.find(squeezed); at != std::string::npos;
         at = body.find(squeezed, at + squeezed.size())) {
        ++n;
    }
    return n;
}

}  // namespace xop_tests::engine_scan

#endif  // XOP_TESTS_ENGINE_SOURCE_SCAN_HPP
