// ---------------------------------------------------------------------------
// [WALLET-CIRCUIT 2026-09-13] rpc_post RECORDS how each call ended.
//
// test_wallet_circuit.cpp pins the counting and gating rules as pure
// functions. None of it matters unless rpc_post actually feeds them -- remove
// that one line and the breaker is exactly as blind as it was on 2026-09-12,
// with every rule test still green. This is the part of the fix a test CAN
// reach without a Chia daemon: ChiaTLSConfig::validate() only checks that
// three regular files exist, and ChiaWalletRPC::get_sync_status() is a bare
// rpc_post.
//
//   * A client that was never opened throws before any transport attempt and
//     must record NOTHING: a closed client is not a stalled wallet.
//   * A client aimed at a loopback port that is bound but not listening fails
//     inside libcurl on its only attempt (max_retries = 0) and must record
//     exactly one transport failure.
//   * Moving a client moves its evidence.
//
// NOT COVERED: the HttpError, MalformedBody, ApplicationError and Ok endings
// need a TLS server that speaks JSON-RPC, and nothing here provides one; and
// whether the Engine and OfferManager READ the counters at the right places
// (TODO S36).
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "xop/rpc/chia_rpc.hpp"
#include "xop/rpc/transport_evidence.hpp"

namespace {

namespace asio = boost::asio;
namespace fs   = std::filesystem;

using xop::rpc::ChiaRPCConfig;
using xop::rpc::ChiaRPCError;
using xop::rpc::ChiaRPCTransportError;
using xop::rpc::ChiaWalletRPC;
using xop::rpc::json;
using xop::rpc::TransportCounters;

// Run one awaitable to completion on @p ioc; return what it threw, if anything.
std::exception_ptr run_to_completion(asio::io_context& ioc,
                                     asio::awaitable<void> task)
{
    std::exception_ptr failure;
    bool finished = false;
    asio::co_spawn(ioc, std::move(task), [&](std::exception_ptr ep) {
        failure  = ep;
        finished = true;
    });
    ioc.restart();
    ioc.run();
    if (!finished) {
        return std::make_exception_ptr(
            std::runtime_error("the awaitable did not run to completion"));
    }
    return failure;
}

std::exception_ptr run_to_completion(asio::io_context& ioc,
                                     asio::awaitable<json> task)
{
    std::exception_ptr failure;
    bool finished = false;
    asio::co_spawn(ioc, std::move(task), [&](std::exception_ptr ep, json) {
        failure  = ep;
        finished = true;
    });
    ioc.restart();
    ioc.run();
    if (!finished) {
        return std::make_exception_ptr(
            std::runtime_error("the awaitable did not run to completion"));
    }
    return failure;
}

// Three EMPTY regular files: enough for ChiaTLSConfig::validate(), useless to
// libcurl -- which never gets far enough to read them, because the TCP
// connect fails first.
class EmptyTlsFiles {
public:
    EmptyTlsFiles()
    {
        static std::atomic<unsigned> sequence{0};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path()
             / ("xop_rpc_transport_" + std::to_string(stamp) + "_"
                + std::to_string(sequence.fetch_add(1)));
        fs::create_directories(dir_);
        for (const char* name : {"client.crt", "client.key", "ca.crt"}) {
            std::ofstream touch(dir_ / name);
        }
    }
    ~EmptyTlsFiles()
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    EmptyTlsFiles(const EmptyTlsFiles&)            = delete;
    EmptyTlsFiles& operator=(const EmptyTlsFiles&) = delete;

    [[nodiscard]] fs::path cert() const { return dir_ / "client.crt"; }
    [[nodiscard]] fs::path key() const { return dir_ / "client.key"; }
    [[nodiscard]] fs::path ca() const { return dir_ / "ca.crt"; }

private:
    fs::path dir_{};
};

// A loopback TCP port that is BOUND but never listens, held for the object's
// lifetime so no other process can take it. A connect to it is refused.
class SilentLoopbackPort {
public:
    SilentLoopbackPort()
    {
        const asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);
        acceptor_.open(ep.protocol());
        acceptor_.bind(ep);
    }
    [[nodiscard]] std::uint16_t port() const
    {
        return acceptor_.local_endpoint().port();
    }

private:
    asio::io_context        ioc_{};
    asio::ip::tcp::acceptor acceptor_{ioc_};  // after ioc_: members init in declaration order
};

ChiaRPCConfig unreachable_wallet(const EmptyTlsFiles& files,
                                 const SilentLoopbackPort& port)
{
    ChiaRPCConfig cfg{};
    cfg.host                  = "127.0.0.1";
    cfg.port                  = port.port();
    cfg.tls.cert_path         = files.cert();
    cfg.tls.key_path          = files.key();
    cfg.tls.ca_cert_path      = files.ca();
    cfg.max_retries           = 0;  // the first attempt is the final attempt
    cfg.request_timeout       = std::chrono::milliseconds{1000};
    cfg.curl_thread_pool_size = 1;
    return cfg;
}

// True when @p err is a transport error raised by libcurl (not by HTTP).
bool is_curl_transport_error(const std::exception_ptr& err)
{
    try {
        std::rethrow_exception(err);
    } catch (const ChiaRPCTransportError& e) {
        return e.curl_code() != CURLE_OK;
    } catch (...) {
        return false;
    }
}

TEST(RpcPostTransportEvidence, ACallOnAClientThatWasNeverOpenedRecordsNothing)
{
    asio::io_context ioc;
    const ChiaRPCConfig cfg{};
    ChiaWalletRPC client(ioc, cfg);
    ASSERT_FALSE(client.is_open());

    const std::exception_ptr err = run_to_completion(ioc, client.get_sync_status());
    ASSERT_TRUE(err != nullptr) << "rpc_post must refuse a client that is not open";
    bool refused_locally = false;
    try {
        std::rethrow_exception(err);
    } catch (const ChiaRPCTransportError&) {
        refused_locally = false;
    } catch (const ChiaRPCError&) {
        refused_locally = true;
    } catch (...) {
        refused_locally = false;
    }
    EXPECT_TRUE(refused_locally)
        << "a closed client fails before any transport attempt";

    const TransportCounters c = client.transport_counters();
    EXPECT_EQ(c.transport_failures, std::uint64_t{0})
        << "a closed client is not evidence that the wallet stalled";
    EXPECT_EQ(c.answered, std::uint64_t{0});
    EXPECT_EQ(c.consecutive_failures, std::uint32_t{0});
}

TEST(RpcPostTransportEvidence, AConnectionNobodyAnswersIsRecordedAsOneTransportFailure)
{
    const EmptyTlsFiles      files;
    const SilentLoopbackPort port;
    asio::io_context         ioc;
    ChiaWalletRPC client(ioc, unreachable_wallet(files, port));
    ASSERT_TRUE(run_to_completion(ioc, client.open()) == nullptr)
        << "open() only checks that the three TLS files exist";
    ASSERT_TRUE(client.is_open());

    const std::exception_ptr err = run_to_completion(ioc, client.get_sync_status());
    ASSERT_TRUE(err != nullptr);
    EXPECT_TRUE(is_curl_transport_error(err))
        << "nothing listens on the port, so libcurl itself must fail";

    const TransportCounters c = client.transport_counters();
    EXPECT_EQ(c.transport_failures, std::uint64_t{1})
        << "rpc_post must record the failure its caller is about to swallow";
    EXPECT_EQ(c.answered, std::uint64_t{0});
    EXPECT_EQ(c.consecutive_failures, std::uint32_t{1});
    client.close();
}

TEST(RpcPostTransportEvidence, MovingAClientMovesItsEvidence)
{
    const EmptyTlsFiles      files;
    const SilentLoopbackPort port;
    asio::io_context         ioc;
    ChiaWalletRPC client(ioc, unreachable_wallet(files, port));
    ASSERT_TRUE(run_to_completion(ioc, client.open()) == nullptr);
    ASSERT_TRUE(run_to_completion(ioc, client.get_sync_status()) != nullptr);
    ASSERT_EQ(client.transport_counters().transport_failures, std::uint64_t{1});

    ChiaWalletRPC moved(std::move(client));
    EXPECT_EQ(moved.transport_counters().transport_failures, std::uint64_t{1});
    EXPECT_EQ(moved.transport_counters().consecutive_failures, std::uint32_t{1});
    EXPECT_EQ(client.transport_counters().transport_failures, std::uint64_t{0})
        << "the moved-from client keeps no stale evidence";

    const ChiaRPCConfig idle_cfg{};
    ChiaWalletRPC assigned(ioc, idle_cfg);
    assigned = std::move(moved);
    EXPECT_EQ(assigned.transport_counters().transport_failures, std::uint64_t{1});
    EXPECT_EQ(assigned.transport_counters().consecutive_failures, std::uint32_t{1});
    EXPECT_EQ(moved.transport_counters().transport_failures, std::uint64_t{0});
    assigned.close();
}

}  // namespace
