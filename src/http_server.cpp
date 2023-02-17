#include "http_server.hpp"
#include "configuration.hpp"
#include "finally.hpp"
#include "worker.hpp"

#include <boost/asio.hpp>
#include <exception>
#include <optional>

using namespace std::string_literals;
using namespace std::string_view_literals;

// declared const, but not constexpr or inline, therefore a definition is
// needed at namespace scope
const int boost::asio::socket_base::max_listen_connections;

namespace acppsrv {

/*** http_server (templates) *************************************************/

// These must be defined before they are used, so not in lexicographic order

struct http_server::conn_limit_init {
    explicit conn_limit_init(http_server& self,
                             std::unique_lock<std::mutex>&& lck):
        self{self}, lck{std::move(lck)} {}
    http_server& self;
    std::unique_lock<std::mutex> lck;
    template <class Handler> auto operator()(Handler&& handler) {
        self.active_connections_hnd.emplace(std::forward<Handler>(handler));
        lck.unlock();
        log_msg(log_level::warning) <<
            "Reached maximum number of connections " << *self.max_connections;
    }
};

template <boost::asio::completion_token_for<void()> CompletionToken>
auto http_server::conn_limit(CompletionToken&& token)
{
    std::unique_lock lck{active_connections_mtx};
    auto async_initiate = []<class Init>(Init&& init, CompletionToken&& token) {
        return boost::asio::async_initiate<CompletionToken, void()>(
                                            std::forward<Init>(init), token);
    };
    conn_limit_init init{*this, std::move(lck)};
    using result_t = decltype(async_initiate(std::move(init), token));
    if (max_connections && active_connections >= *max_connections)
        return std::optional<result_t>(async_initiate(std::move(init), token));
    else
        return std::optional<result_t>{};
}

/*** http_server *************************************************************/

http_server::http_server(const configuration& cfg, thread_pool& workers):
    port(cfg.http_port()), workers(workers), acceptor(workers.ctx)
{
    if (cfg.data().has_http_server()) {
        auto&& http = cfg.data().http_server();
        if (auto v = http.listen_queue(); v != 0) {
            // An int value is needed for listen queue, ensure correct
            // conversion with capping at the max. int
            using ct = std::common_type_t<int, decltype(v)>;
            if (auto m = std::numeric_limits<int>::max(); ct(v) > ct(m))
                v = decltype(v)(m);
            listen_queue = v;
        }
        if (auto v = http.max_connections())
            max_connections = v;
        idle_timeout = configuration::get_time(http.idle_timeout());
        keepalive_timeout = configuration::get_time(http.keepalive_timeout());
        if (auto v = http.keepalive_requests())
            keepalive_requests = v;
        if (auto v = http.max_request_headers())
            max_request_headers = v;
        if (auto v = http.max_request_body())
            max_request_body = v;
    }
}

boost::asio::awaitable<void> http_server::accept_loop()
{
    for (;;) {
        if (auto cl = conn_limit(boost::asio::use_awaitable))
            co_await std::move(*cl);
        log_msg(log_level::debug) <<
            "Waiting for connection active_connections=" <<
            active_connections << " maximum=" << log_limit(max_connections);
        auto [ec, conn] = co_await acceptor.async_accept(
                             boost::asio::make_strand(acceptor.get_executor()),
                             boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec)
            log_msg(log_level::err) << "Cannot accept connection: " <<
                ec.message();
        else
            if (auto client = conn.remote_endpoint(ec); ec) {
                log_msg(log_level::err) << "Cannot get client address: " <<
                    ec.message();
            } else {
                auto ac = ++active_connections;
                uint64_t sid = session++;
                log_msg(log_level::info, {sid}) <<
                    "Accepted connection client=" << conn.remote_endpoint() <<
                    " active=" << ac <<
                    " maximum=" << log_limit(max_connections);
                co_spawn(conn.get_executor(),
                         handle_connection(std::move(conn), std::move(client),
                                           sid),
                         co_spawn_handler);
        }
    }
}

template <class Endpoint>
void http_server::active_connection_end(const Endpoint& client, uint64_t sid,
                                        const boost::system::error_code& ec,
                                        std::string_view stage)
{
    std::unique_lock lck{active_connections_mtx};
    auto ac = --active_connections;
    if (max_connections && active_connections < *max_connections &&
        active_connections_hnd)
    {
        auto hnd = std::move(*active_connections_hnd);
        active_connections_hnd.reset();
        boost::asio::post(acceptor.get_executor(), std::move(hnd));
    }
    lck.unlock();
    auto msg = log_msg(ec ? log_level::err : log_level::info, {sid}) <<
        "Finished handling connection client=" << client << " active=" << ac <<
        " maximum=" << log_limit(max_connections);
    if (ec)
        msg << " stage=" << stage << " error=" << ec.message();
}

void http_server::before_request_timeout(boost::beast::tcp_stream& stream,
                                         bool first)
{
    auto& timeout = first ? idle_timeout : keepalive_timeout;
    if (timeout)
        stream.expires_after(*timeout);
    else
        stream.expires_never();
}

void http_server::co_spawn_handler(const std::exception_ptr& e)
{
    if (e)
        std::rethrow_exception(e);
}

boost::asio::awaitable<void>
http_server::handle_connection(socket_type conn, endpoint_type client,
                               uint64_t sid)
{
    boost::system::error_code ec;
    std::string_view stage = {};
    util::finally at_end([this, &client, sid, &ec, &stage]() {
        active_connection_end(client, sid, ec, stage);
    });
    log_msg(log_level::debug, {sid}) << "Waiting for data from client";
    boost::beast::tcp_stream stream(std::move(conn));
    boost::beast::flat_buffer buffer;
    for (auto [req_n, close] = std::pair{uint64_t{1}, false};
         !close && (!keepalive_requests || req_n - 1U < *keepalive_requests);
         ++ req_n)
    {
        stage = ""sv;
        log_msg(log_level::debug, {sid, req_n}) << "Waiting for request " <<
            req_n << '/' << log_limit(keepalive_requests);
        boost::beast::http::request_parser<http_body_type> parser;
        if (max_request_headers)
            parser.header_limit(*max_request_headers);
        if (max_request_body)
            parser.body_limit(*max_request_body);
        before_request_timeout(stream, req_n == 1);
        co_await boost::beast::http::async_read_header(
            stream, buffer, parser,
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        stage = "read_request_headers";
        if (ec)
            goto abort_connection;
        http_request_type& request = parser.get();
        log_msg(log_level::info, {sid, req_n}) << "Request headers" <<
            " method=" << request.method_string() <<
            " uri=" << request.target() <<
            " version=" << request.version() / 10 << '.' <<
            request.version() % 10;
        // Create a response
        http_response_type response = co_await handle_request(request, sid,
                                                              req_n);
        if (!response.keep_alive())
            close = true;
        // Send the response
        stream.expires_never();
        co_await boost::beast::http::async_write(
            stream, response,
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        stage = "write_response";
        if (ec)
            goto abort_connection;
        auto msg = log_msg(log_level::info, {sid, req_n}) <<
            "Request " << req_n << '/' <<
            log_limit(keepalive_requests) <<
            " finished method=" << request.method_string() <<
            " uri=" << request.target() <<
            " status=" << response.result_int();
        if (auto size = response.payload_size())
            msg << " content_length=" << *size;
    }
    ec = {};
    stage = ""sv;
abort_connection:
    co_return;
}

boost::asio::awaitable<http_server::http_response_type>
http_server::handle_request(const http_request_type& request, uint64_t sid,
                            uint64_t req_n)
{
    http_response_type response;
    response.keep_alive(request.keep_alive());
    DEBUG({sid, req_n}) << "Generating response";
    co_return response;
}

void http_server::in_request_timeout(boost::beast::tcp_stream& stream)
{
    if (idle_timeout)
        stream.expires_after(*idle_timeout);
    else
        stream.expires_never();
}

template <std::integral T> std::variant<std::string_view, T>
http_server::log_limit(const std::optional<T>& limit)
{
    if (limit)
        return *limit;
    else
        return "unlimited"sv;
}

bool http_server::run()
{
    boost::system::error_code ec;
    endpoint_type addr{boost::asio::ip::address{}, port};
    assert(addr.address().is_unspecified());
    acceptor.open(addr.protocol());
    if (ec) {
        log_msg(log_level::crit) << "Cannot create server socket: " <<
            ec.message();
        return false;
    }
    acceptor.set_option(decltype(acceptor)::reuse_address{true}, ec);
    if (ec) {
        log_msg(log_level::crit) <<
            "Cannot set reuse_addres on server socket: " << ec.message();
        return false;
    }
    acceptor.bind(addr, ec);
    if (ec) {
        log_msg(log_level::crit) << "Cannot bind server socket: " <<
            ec.message();
        return false;
    }
    // listen_queue <= max. int ensured 
    acceptor.listen(
        int(listen_queue.value_or(decltype(acceptor)::max_listen_connections)),
        ec);
    if (ec) {
        log_msg(log_level::crit) << "Cannot listen on server socket: " <<
            ec.message();
        return false;
    }
    boost::asio::co_spawn(acceptor.get_executor(), accept_loop(),
                          co_spawn_handler);
    return true;
}

} // namespace acppsrv
