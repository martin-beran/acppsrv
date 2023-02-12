#pragma once

#include <boost/asio.hpp>

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <optional>

namespace acppsrv {

class configuration;
class thread_pool;

class http_server {
public:
    http_server(const configuration& cfg, thread_pool& workers);
    http_server(const http_server&) = delete;
    http_server(http_server&&) = delete;
    ~http_server() = default;
    http_server& operator=(const http_server&) = delete;
    http_server& operator=(http_server&&) = delete;
    bool run();
private:
    boost::asio::awaitable<void> accept_loop();
    boost::asio::awaitable<void>
        handle_connection(boost::asio::ip::tcp::socket conn);
    uint16_t port;
    std::optional<uint32_t> listen_queue;
    std::optional<uint32_t> max_connections;
    std::optional<std::chrono::nanoseconds> idle_timeout;
    std::optional<std::chrono::nanoseconds> keepalive_timeout;
    std::optional<uint32_t> keepalive_requests;
    std::optional<uint32_t> max_request_line;
    std::optional<uint32_t> max_request_headers;
    std::optional<uint32_t> max_request_body;
    thread_pool& workers;
    boost::asio::ip::tcp::acceptor acceptor;
    std::atomic<size_t> active_connections{0};
};

} // namespace acppsrv
