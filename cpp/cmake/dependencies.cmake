# =============================================================================
# dependencies.cmake — Dependency resolution for XOPTrader C++ engine
# =============================================================================
#
# Strategy: vcpkg manifest mode is the primary path.  find_package() locates
# packages that vcpkg (or the system) provides.  FetchContent is the fallback
# for header-only libraries when vcpkg is unavailable or a CI image is lean.
#
# ISO/IEC 27001:2022 — no secrets or credentials appear in build scripts.
# =============================================================================

include(FetchContent)

# ---------------------------------------------------------------------------
# 1. Boost (Asio, Beast, ProgramOptions, JSON)
#    Boost is complex to FetchContent; require vcpkg or system install.
# ---------------------------------------------------------------------------
find_package(Boost 1.84 REQUIRED
    COMPONENTS
        system              # Asio runtime dependency
        program_options     # CLI argument parsing
        json                # Boost.JSON (lightweight alternative path)
)
# Asio and Beast are header-only but live inside the Boost super-project.
# Boost::system satisfies the link-time requirement for Asio coroutines.

# ---------------------------------------------------------------------------
# 2. OpenSSL — TLS for Chia RPC (mutual TLS) and HTTPS to dexie / CEX APIs.
#    Must be vcpkg or system; FetchContent is impractical.
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# 3. CURL — HTTP client for REST calls to dexie.space, CEX feeds, etc.
# ---------------------------------------------------------------------------
find_package(CURL REQUIRED)

# ---------------------------------------------------------------------------
# 4. nlohmann-json — JSON DOM / SAX parsing.
#    Try vcpkg first; fall back to FetchContent (header-only, trivial).
# ---------------------------------------------------------------------------
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via find_package — fetching via FetchContent")
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# 5. spdlog — structured, high-performance logging.
#    Try vcpkg first; fall back to FetchContent.
# ---------------------------------------------------------------------------
find_package(spdlog 1.13 QUIET)
if(NOT spdlog_FOUND)
    message(STATUS "spdlog not found via find_package — fetching via FetchContent")
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.14.1
        GIT_SHALLOW    TRUE
    )
    set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
endif()

# ---------------------------------------------------------------------------
# 6. yaml-cpp — YAML configuration file parsing.
#    Try vcpkg first; fall back to FetchContent.
# ---------------------------------------------------------------------------
find_package(yaml-cpp 0.8 QUIET)
if(NOT yaml-cpp_FOUND)
    message(STATUS "yaml-cpp not found via find_package — fetching via FetchContent")
    FetchContent_Declare(
        yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG        0.8.0
        GIT_SHALLOW    TRUE
    )
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(yaml-cpp)
endif()

# ---------------------------------------------------------------------------
# 7. SQLite3 — lightweight local trade log / cost-basis database.
# ---------------------------------------------------------------------------
find_package(unofficial-sqlite3 CONFIG QUIET)
if(NOT unofficial-sqlite3_FOUND)
    # Some distributions expose it as plain "SQLite3"
    find_package(SQLite3 REQUIRED)
endif()

# ---------------------------------------------------------------------------
# 8. prometheus-cpp — Prometheus metrics exposition (pull + push).
#    Complex build; require vcpkg or system install.
# ---------------------------------------------------------------------------
find_package(prometheus-cpp CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Helper: aggregate an INTERFACE target that downstream code can link against
# in one shot.  Individual targets are still available for selective linking.
# ---------------------------------------------------------------------------
add_library(xop_deps INTERFACE)

target_link_libraries(xop_deps INTERFACE
    Boost::system
    Boost::program_options
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    prometheus-cpp::pull
    prometheus-cpp::push
)

# SQLite3 — vcpkg exposes "unofficial::sqlite3::sqlite3"; system exposes "SQLite::SQLite3".
if(TARGET unofficial::sqlite3::sqlite3)
    target_link_libraries(xop_deps INTERFACE unofficial::sqlite3::sqlite3)
elseif(TARGET SQLite::SQLite3)
    target_link_libraries(xop_deps INTERFACE SQLite::SQLite3)
endif()
