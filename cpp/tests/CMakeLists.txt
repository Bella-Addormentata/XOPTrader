# =============================================================================
# CMakeLists.txt -- Test build configuration for XOPTrader C++ engine
# =============================================================================
#
# Uses Google Test fetched via FetchContent (no system install required).
#
# Build:
#   cmake -B build -S .. -DXOP_BUILD_TESTS=ON
#   cmake --build build --target xop_tests
#   ctest --test-dir build --output-on-failure
#
# ISO/IEC 27001:2022 -- no secrets or credentials in test infrastructure.
# ISO/IEC 5055       -- deterministic tests; no undefined behaviour.
# =============================================================================

include(FetchContent)

# ---------------------------------------------------------------------------
# Google Test -- fetched at configure time; no system dependency required.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

# Prevent GTest from overriding our compiler/linker settings on Windows.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# Suppress GTest installation targets (they pollute our install rules).
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Third-party: relax strict warning flags for googletest targets so that
# diagnostics inside gtest headers (e.g. -Wdouble-promotion) do not fail
# our build.  First-party targets retain -Werror.
if (NOT MSVC)
    target_compile_options(gtest      PRIVATE -Wno-error -Wno-double-promotion)
    target_compile_options(gtest_main PRIVATE -Wno-error -Wno-double-promotion)
endif()

# ---------------------------------------------------------------------------
# Test executable -- links against the xop_core library and GTest.
# ---------------------------------------------------------------------------
add_executable(xop_tests
    test_database.cpp
    test_avellaneda.cpp
    test_glft.cpp
    test_spread.cpp
    test_inventory.cpp
    test_coin_manager.cpp
    test_volatility.cpp
    test_regime.cpp
    test_competitor_detection.cpp
    test_whale_detection.cpp
    test_advanced_trading.cpp
    test_market_analyzer.cpp
    test_limits.cpp
    test_config.cpp
    test_liquidity.cpp
    test_market_allocator.cpp
    test_fee_tracker.cpp
    test_pnl_units.cpp
    test_competitiveness_pid.cpp
)

target_link_libraries(xop_tests
    PRIVATE
        xop_core
        GTest::gtest
        GTest::gtest_main
)

# Ensure the test binary can find the xop headers.
target_include_directories(xop_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Register with CTest so that `ctest` discovers and runs the tests.
include(GoogleTest)
gtest_discover_tests(xop_tests)
