# =============================================================================
# CMakeLists.txt — Root build for XOPTrader CHIA DEX market-making engine
# =============================================================================
#
# Build:
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
#   cmake --build build --config Release
#
# Prerequisites:
#   vcpkg with manifest mode (vcpkg.json in this directory).
#   A C++20 compiler with coroutine support (MSVC 19.29+, GCC 11+, Clang 14+).
#
# ISO/IEC 27001:2022 — no secrets or credentials are embedded in the build.
# ISO/IEC 5055       — static analysis targets provided (clang-tidy, cppcheck).
# =============================================================================

cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

# ---------- vcpkg manifest mode auto-detection ----------------------------
# If the user forgot -DCMAKE_TOOLCHAIN_FILE, try the VCPKG_ROOT env variable.
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
    message(STATUS "Auto-detected vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()

project(xop_trader
    VERSION   0.7.46
    LANGUAGES CXX
    DESCRIPTION "CHIA DEX market-making bot — XOPTrader C++ engine"
)

# Generate xop/version.hpp from the project VERSION above so the binary
# never has a hand-maintained version literal.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/xop/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/xop/version.hpp"
    @ONLY
)

# ==========================================================================
# Global compiler settings
# ==========================================================================
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # For clang-tidy / LSP

# ---------- Coroutine support ---------------------------------------------
# C++20 coroutines require explicit opt-in on some compilers.
if(MSVC)
    # MSVC 19.29+ : /await:strict for standards-conformant coroutines.
    add_compile_options(/await:strict)
    # Silence harmless warnings that flood Boost.Asio headers.
    add_compile_definitions(_WIN32_WINNT=0x0A00)  # Target Windows 10+
    add_compile_definitions(_SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fcoroutines)
    # GCC 11-12 need this; GCC 13+ enables coroutines with -std=c++20 alone,
    # but the flag is harmless even when redundant.
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang 14+ supports coroutines under -std=c++20 without extra flags.
    # Apple Clang may lag; add -fcoroutines-ts if needed.
endif()

# ---------- Hardening flags (ISO/IEC 5055 — CWE mitigation) ---------------
if(NOT MSVC)
    add_compile_options(
        # ISO/IEC 5055 -- additional warnings for financial code correctness:
        # -Wshadow: detect accidental variable shadowing (CWE-710)
        # -Wformat-security / -Wformat=2: catch format-string vulnerabilities (CWE-134)
        # -Wdouble-promotion: flag implicit float-to-double promotions in arithmetic
        # T4-22: -Werror promotes all warnings to errors (matching MSVC /WX).
        -Wall -Wextra -Wpedantic -Werror
        -Wshadow -Wformat-security -Wformat=2 -Wdouble-promotion
        -fstack-protector-strong
        # ISO/IEC 5055 -- _FORTIFY_SOURCE requires optimisation (-O1+); applying
        # it in Debug (-O0) builds causes compiler warnings or has no effect.
        $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
    )
    # ISO/IEC 5055 -- RELRO and BIND_NOW harden the GOT/PLT on Linux only.
    # Apple ld (macOS/Clang) does not support -z; guard with Linux check.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_link_options(-Wl,-z,relro -Wl,-z,now)
    endif()
else()
    # ISO/IEC 5055 -- /WX promotes warnings to errors, enforcing clean builds.
    # _CRT_SECURE_NO_WARNINGS suppresses MSVC C4996 deprecation warnings for
    # standard POSIX functions like getenv() that are safe in this context.
    add_compile_options(/W4 /WX /sdl /GS /guard:cf)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_link_options(/guard:cf)
endif()

# ---------- Link-Time Optimization for Release (T6-08) ---------------------
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_output)
if(_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(STATUS "LTO not supported: ${_ipo_output}")
endif()

# ==========================================================================
# Options
# ==========================================================================
option(XOP_BUILD_TESTS       "Build unit and integration tests"    ON)
option(XOP_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds"  OFF)
option(XOP_ENABLE_CLANG_TIDY "Run clang-tidy during build"        OFF)

if(XOP_ENABLE_SANITIZERS AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ==========================================================================
# Dependencies (vcpkg + FetchContent fallback)
# ==========================================================================
include(cmake/dependencies.cmake)

# ==========================================================================
# Core library — everything except main(), so tests can link against it.
# ==========================================================================
add_library(xop_core STATIC
    # -- Core infrastructure ---------------------------------------------------
    src/config.cpp
    src/state.cpp
    src/database.cpp
    src/engine.cpp
    src/backtest.cpp

    # -- Strategy layer --------------------------------------------------------
    src/strategy/spread.cpp
    src/strategy/avellaneda.cpp
    src/strategy/glft.cpp
    src/strategy/regime.cpp
    src/strategy/liquidity.cpp
    src/strategy/arbitrage.cpp
    src/strategy/new_strategies.cpp
    src/strategy/chia_edge.cpp
    src/strategy/order_book_tactics.cpp
    src/strategy/strategy_portfolio.cpp
    src/strategy/fee_tracker.cpp
    src/strategy/kappa_calibrator.cpp
    src/strategy/market_allocator.cpp

    # -- Risk layer ------------------------------------------------------------
    src/risk/inventory.cpp
    src/risk/limits.cpp
    src/risk/hedging.cpp
    src/risk/loss_manager.cpp
    src/risk/drift_analyzer.cpp

    # -- Execution layer -------------------------------------------------------
    src/execution/offer_manager.cpp
    src/execution/coin_manager.cpp
    src/execution/market_data.cpp

    # -- RPC / networking ------------------------------------------------------
    src/rpc/chia_rpc.cpp
    src/rpc/dexie_client.cpp
    src/rpc/coingecko_client.cpp

    # -- Data layer ------------------------------------------------------------
    src/data/volatility.cpp
    src/data/adverse_selection.cpp
    src/data/market_analyzer.cpp

    # -- Monitoring ------------------------------------------------------------
    src/monitoring/pnl.cpp
    src/monitoring/metrics.cpp
    src/monitoring/alerts.cpp
    src/monitoring/on_chain_reconciler.cpp
)

target_include_directories(xop_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(xop_core PUBLIC xop_deps)

# ==========================================================================
# Executable
# ==========================================================================
add_executable(xop_trader src/main.cpp)
target_link_libraries(xop_trader PRIVATE xop_core)

# ISO/IEC 5055 -- PIE (Position-Independent Executable) enables full ASLR,
# mitigating CWE-119 class memory-corruption exploits at runtime.
if(NOT MSVC)
    target_compile_options(xop_trader PRIVATE -fPIE)
    target_link_options(xop_trader PRIVATE -pie)
endif()

# ==========================================================================
# Tests (Google Test or Catch2 — to be wired in Phase 2)
# ==========================================================================
if(XOP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ==========================================================================
# Install rules
# ==========================================================================
include(GNUInstallDirs)

# Install the engine and its runtime dependencies (DLLs/.so/.dylib)
# so packaging jobs can ship a self-contained runtime bundle.
install(
    TARGETS xop_trader
    RUNTIME_DEPENDENCY_SET xop_runtime_deps
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    RUNTIME_DEPENDENCY_SET xop_runtime_deps
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    PRE_EXCLUDE_REGEXES
        "api-ms-.*"
        "ext-ms-.*"
    POST_EXCLUDE_REGEXES
        ".*system32/.*"
)

# ==========================================================================
# Static analysis targets (optional convenience)
# ==========================================================================
if(XOP_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set_target_properties(xop_core PROPERTIES
            CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-checks=-*,bugprone-*,cppcoreguidelines-*,performance-*,readability-*"
        )
    endif()
endif()

find_program(CPPCHECK_EXE NAMES cppcheck)
if(CPPCHECK_EXE)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK_EXE}
            --enable=all
            --std=c++20
            --suppress=missingInclude
            --project=${CMAKE_BINARY_DIR}/compile_commands.json
        COMMENT "Running cppcheck static analysis"
        VERBATIM
    )
endif()
