#pragma once

#include <boost/asio.hpp>

#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <coroutine>
#include <exception>
#include <optional>

namespace acppsrv {

class configuration;
class log_msg;
class thread_pool;

class http_server {
public:
    using protocol_type = boost::asio::ip::tcp;
    using socket_type = protocol_type::socket;
    using acceptor_type = protocol_type::acceptor;
    using endpoint_type = socket_type::endpoint_type;
    static_assert(std::is_same_v<endpoint_type, acceptor_type::endpoint_type>);
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
        handle_connection(socket_type conn, endpoint_type client);
    template <boost::asio::completion_token_for<void()> CompletionToken>
        auto conn_limit(CompletionToken&& token);
    template <class Endpoint>
        void active_connection_end(const Endpoint& client);
    template <class T> void log_limit(log_msg& msg,
                                      const std::optional<T>& limit);
    static void co_spawn_handler(std::exception_ptr e);
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
    acceptor_type acceptor;
    std::atomic<size_t> active_connections{0};
    std::mutex active_connections_mtx;
    std::optional<boost::asio::async_result<
        boost::asio::use_awaitable_t<decltype(acceptor)::executor_type>,
        void()>::handler_type> active_connections_hnd;

};

} // namespace acppsrv
