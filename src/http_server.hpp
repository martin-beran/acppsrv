#pragma once

#include "worker.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <array>
#include <chrono>
#include <coroutine>
#include <exception>
#include <map>
#include <optional>

namespace acppsrv {

class configuration;
class db_server;
class log_msg;
class http_handler;

namespace http_hnd {

class stat;

} // namespace http_hnd

class http_server {
public:
    using protocol_type = boost::asio::ip::tcp;
    using socket_type = protocol_type::socket;
    using acceptor_type = protocol_type::acceptor;
    using endpoint_type = socket_type::endpoint_type;
    using http_body_type = boost::beast::http::string_body;
    using http_request_type = boost::beast::http::request<http_body_type>;
    using http_response_type = boost::beast::http::response<http_body_type>;
    using empty_response_type =
        boost::beast::http::response<boost::beast::http::empty_body>;
    static_assert(std::is_same_v<endpoint_type, acceptor_type::endpoint_type>);
    http_server(const configuration& cfg, thread_pools_t& workers,
                std::string_view app_name, std::string_view app_version,
                db_server& db_srv);
    http_server(const http_server&) = delete;
    http_server(http_server&&) = delete;
    ~http_server() = default;
    http_server& operator=(const http_server&) = delete;
    http_server& operator=(http_server&&) = delete;
    bool run();
    const std::string header_server;
private:
    struct conn_limit_init;
    boost::asio::awaitable<void> accept_loop();
    // Expects that conn uses a strand as its executor
    boost::asio::awaitable<void>
        handle_connection(socket_type conn, endpoint_type client, uint64_t sid);
    template <boost::asio::completion_token_for<void()> CompletionToken>
        auto conn_limit(CompletionToken&& token);
    template <class Endpoint>
        void active_connection_end(const Endpoint& client, uint64_t sid,
                                   uint64_t requests,
                                   const boost::system::error_code& ec,
                                   std::string_view stage = {});
    void before_request_timeout(boost::beast::tcp_stream& stream, bool first);
    void in_request_timeout(boost::beast::tcp_stream& stream);
    template <std::integral T> std::variant<std::string_view, T>
        log_limit(const std::optional<T>& limit);
    static void co_spawn_handler(const std::exception_ptr& e);
    uint16_t port;
    std::optional<uint32_t> listen_queue;
    std::optional<uint32_t> max_connections;
    std::optional<std::chrono::nanoseconds> idle_timeout;
    std::optional<std::chrono::nanoseconds> keepalive_timeout;
    std::optional<uint32_t> keepalive_requests;
    std::optional<uint32_t> max_request_headers;
    std::optional<uint32_t> max_request_body;
    thread_pools_t& workers;
    size_t use_workers;
    acceptor_type acceptor;
    std::atomic<size_t> active_connections{0};
    std::mutex active_connections_mtx;
    std::optional<boost::asio::async_result<
        boost::asio::use_awaitable_t<acceptor_type::executor_type>,
        void()>::handler_type> active_connections_hnd;
    std::atomic<uint64_t> session{0};
    std::map<std::string, std::unique_ptr<http_handler>> handlers;
    http_hnd::stat* stat;
};

class http_handler {
public:
    static constexpr std::array content_type_json{"application/json"};
    static constexpr std::array content_type_protobuf{
        "application/protobuf",
        "application/x-protobuf",
        "application/vnd.google.protobuf",
    };
    using http_request_type = http_server::http_request_type;
    using http_response_type = http_server::http_response_type;
    http_handler() = default;
    http_handler(const http_handler&) = delete;
    http_handler(http_handler&&) = delete;
    virtual ~http_handler() = default;
    http_handler& operator=(const http_handler&) = delete;
    http_handler& operator=(http_handler&&) = delete;
    virtual bool async() = 0;
    virtual http_response_type handle_sync(const http_request_type& request,
                                           uint64_t sid, uint64_t req_n);
    virtual boost::asio::awaitable<http_response_type>
        handle_async(const http_request_type& request,
                     uint64_t sid, uint64_t req_n);
    static http_response_type error_response(uint64_t sid, uint64_t req_n,
                                             boost::beast::http::status status,
                                             std::string msg);
protected:
    static std::pair<bool, std::string_view>
        json_body(const http_request_type& headers, bool is_req);
    static std::pair<bool, std::string_view>
        protobuf_body(const http_request_type& headers, bool is_req);
    template <class Request>
        std::pair<std::optional<Request>, http_response_type>
        parse(const http_request_type& request, uint64_t sid, uint64_t req_n);
    template <class Response>
        bool serialize(const http_request_type& request, uint64_t sid,
                       uint64_t req_n, const Response& data,
                       http_response_type& response);
};

} // namespace acppsrv
