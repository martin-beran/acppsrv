#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>

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
    using http_body_type = boost::beast::http::string_body;
    using http_request_type = boost::beast::http::request<http_body_type>;
    using http_response_type = boost::beast::http::response<http_body_type>;
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
    // Expects that conn uses a strand as its executor
    boost::asio::awaitable<void>
        handle_connection(socket_type conn, endpoint_type client, uint64_t sid);
    boost::asio::awaitable<http_response_type>
        handle_request(const http_request_type& request, uint64_t sid,
                       uint64_t req_n);
    template <boost::asio::completion_token_for<void()> CompletionToken>
        auto conn_limit(CompletionToken&& token);
    template <class Endpoint>
        void active_connection_end(const Endpoint& client, uint64_t sid,
                                   const boost::system::error_code& ec,
                                   std::string_view stage = {});
    void before_request_timeout(boost::beast::tcp_stream& stream, bool first);
    void in_request_timeout(boost::beast::tcp_stream& stream);
    template <std::integral T> std::variant<std::string_view, T>
        log_limit(const std::optional<T>& limit);
    static void co_spawn_handler(std::exception_ptr e);
    uint16_t port;
    std::optional<uint32_t> listen_queue;
    std::optional<uint32_t> max_connections;
    std::optional<std::chrono::nanoseconds> idle_timeout;
    std::optional<std::chrono::nanoseconds> keepalive_timeout;
    std::optional<uint32_t> keepalive_requests;
    std::optional<uint32_t> max_request_headers;
    std::optional<uint32_t> max_request_body;
    thread_pool& workers;
    acceptor_type acceptor;
    std::atomic<size_t> active_connections{0};
    std::mutex active_connections_mtx;
    std::optional<boost::asio::async_result<
        boost::asio::use_awaitable_t<decltype(acceptor)::executor_type>,
        void()>::handler_type> active_connections_hnd;
    std::atomic<uint64_t> session{0};
};

} // namespace acppsrv
