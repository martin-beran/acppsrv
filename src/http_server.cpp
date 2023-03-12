#include "http_server.hpp"
#include "configuration.hpp"
#include "finally.hpp"
#include "worker.hpp"

#include "http_hnd_db.hpp"
#include "http_hnd_echo.hpp"
#include "http_hnd_stat.hpp"

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/detail/error_code.hpp>
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

http_server::http_server(const configuration& cfg,
                         thread_pools_t& workers,
                         std::string_view app_name,
                         std::string_view app_version,
                         db_server& db_srv):
    header_server(std::string(app_name) + "/" + std::string(app_version)),
    port(cfg.http_port()), workers(workers), use_workers(workers.size()),
    acceptor(workers.back()->ctx)
{
    if (use_workers > 1 && cfg.data().has_thread_pools() &&
        cfg.data().thread_pools().accept_pool())
    {
        --use_workers;
    }
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
    // Initialize handlers
    handlers["/db"] = std::make_unique<http_hnd::db>(db_srv);
    handlers["/echo"] = std::make_unique<http_hnd::echo>();
    auto p_stat = std::make_unique<http_hnd::stat>();
    stat = p_stat.get();
    handlers["/stat"] = std::move(p_stat);
}

boost::asio::awaitable<void> http_server::accept_loop()
{
    for (size_t worker_i = 0;; worker_i = (worker_i + 1) % use_workers) {
        if (auto cl = conn_limit(boost::asio::use_awaitable))
            co_await std::move(*cl);
        log_msg(log_level::debug) <<
            "Waiting for connection active_connections=" <<
            active_connections << '/' << log_limit(max_connections);
        auto [ec, conn] = co_await acceptor.async_accept(
                             boost::asio::make_strand(workers[worker_i]->ctx),
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
                    " maximum=" << log_limit(max_connections) <<
                    " pool=" << (worker_i + 1) << '/' << use_workers;
                co_spawn(conn.get_executor(),
                         handle_connection(std::move(conn), std::move(client),
                                           sid),
                         co_spawn_handler);
        }
    }
}

template <class Endpoint>
void http_server::active_connection_end(const Endpoint& client,
                                        uint64_t sid, uint64_t requests,
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
        "Finished handling connection client=" << client <<
        " active_connections=" << ac << '/' << log_limit(max_connections) <<
        " requests=" << requests;
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
    // This is one long coroutine, because splitting it into several smaller
    // coroutines would require allocating many coroutine states.
    // Prepare for final logging and tracking active connections
    boost::system::error_code ec;
    uint64_t req_n = 0U;
    std::string_view stage = {};
    util::finally at_end([this, &client, sid, &req_n, &ec, &stage]() {
        active_connection_end(client, sid, req_n, ec, stage);
    });
    // Process requests from the connection
    ++stat->data.connections;
    log_msg(log_level::debug, {sid}) << "Waiting for data from client";
    boost::beast::tcp_stream stream(std::move(conn));
    boost::beast::flat_buffer buffer;
    for (bool keepalive = true; keepalive;)
    {
        // Read request headers
        stage = ""sv;
        log_msg(log_level::debug, {sid, req_n + 1}) << "Waiting for request " <<
            (req_n + 1) << '/' << log_limit(keepalive_requests);
        boost::beast::http::request_parser<http_body_type> parser;
        if (max_request_headers)
            parser.header_limit(*max_request_headers);
        if (max_request_body)
            parser.body_limit(*max_request_body);
        before_request_timeout(stream, req_n == 0);
        co_await boost::beast::http::async_read_header(
            stream, buffer, parser,
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        stage = "read_request_headers"sv;
        if (ec)
            goto abort_connection;
        http_request_type& request = parser.get();
        ++req_n;
        ++stat->data.requests;
        log_msg(log_level::debug, {sid, req_n}) << "Request headers" <<
            " method=" << request.method_string() <<
            " uri=" << request.target() <<
            " version=" << request.version() / 10 << '.' <<
            request.version() % 10;
        // Handle 100-continue
        if (boost::beast::iequals(request[boost::beast::http::field::expect],
                                  "100-continue"sv))
        {
            empty_response_type response;
            response.version(11);
            response.result(boost::beast::http::status::continue_);
            response.set(boost::beast::http::field::server, header_server);
            in_request_timeout(stream);
            co_await boost::beast::http::async_write(stream, response,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            stage = "write_100_continue"sv;
            if (ec)
                goto abort_connection;
        }
        // Read request body
        while (!parser.is_done()) {
            in_request_timeout(stream);
            co_await boost::beast::http::async_read_some(
                stream, buffer, parser,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            stage = "read_request_body"sv;
            if (ec)
                goto abort_connection;
        }
        log_msg(log_level::debug, {sid, req_n}) <<
            "Request body content_length=" << request.body().size();
        stat->data.data_req += request.body().size();
        // Create a response
        auto handler = handlers.find(request.target());
        http_response_type response = handler == handlers.end() ?
            http_handler::error_response(sid, req_n,
                                     boost::beast::http::status::bad_request,
                                     "Unknown URI path") :
            handler->second->async() ?
                co_await handler->second->handle_async(request, sid, req_n) :
                handler->second->handle_sync(request, sid, req_n);
        response.prepare_payload();
        if (request.method() == boost::beast::http::verb::head)
            response.body().clear();
        stat->data.data_resp += response.body().size();
        response.set(boost::beast::http::field::server, header_server);
        keepalive = request.keep_alive() &&
            (!keepalive_requests || req_n < *keepalive_requests);
        response.keep_alive(keepalive);
        // Send the response
        stream.expires_never();
        co_await boost::beast::http::async_write(
            stream, response,
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        stage = "write_response"sv;
        if (ec)
            goto abort_connection;
        log_msg(log_level::info, {sid, req_n}) <<
            "Request finished " << req_n << '/' <<
            log_limit(keepalive_requests) <<
            " client=" << client <<
            " method=" << request.method_string() <<
            " uri=" << request.target() <<
            " status=" << response.result_int() <<
            " request_body=" << request.payload_size().value_or(0) <<
            " response_body=" << response.payload_size().value_or(0);
    }
    ec = {};
    stage = ""sv;
abort_connection:
    // Send error response for selected errors
    if (ec == boost::beast::http::error::body_limit) {
        http_response_type
            response{boost::beast::http::status::payload_too_large, 11};
        in_request_timeout(stream);
        boost::system::error_code ece;
        co_await boost::beast::http::async_write(
            stream, response,
            boost::asio::redirect_error(boost::asio::use_awaitable, ece));
        if (ece)
            log_msg(log_level::err, {sid}) <<
                "Failed sending error message: " << ec.message();
    }
    co_return;
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
    log_msg(log_level::notice) <<
        "Listening port=" << port <<
        " pools=" << use_workers <<
        " accept_pool=" << (use_workers < workers.size());
    return true;
}

/*** http_handler ************************************************************/

http_server::http_response_type
http_handler::error_response(uint64_t sid, uint64_t req_n,
                             boost::beast::http::status status,
                             std::string msg)
{
    log_msg(log_level::debug, {sid, req_n}) <<
        "Returning error response status=" << unsigned(status);
    http_response_type response{status, 11};
    response.set(boost::beast::http::field::content_type, "text/plain");
    response.body() = std::move(msg);
    response.body() += '\n';
    return response;
}

boost::asio::awaitable<http_server::http_response_type>
// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
http_handler::handle_async(const http_request_type& request,
                           uint64_t sid, uint64_t req_n)
{
    co_return handle_sync(request, sid, req_n);
}

http_server::http_response_type
http_handler::handle_sync(const http_request_type&, uint64_t, uint64_t)
{
    http_response_type response{boost::beast::http::status::ok, 11};
    return response;
}

std::pair<bool, std::string_view>
http_handler::json_body(const http_request_type& request, bool is_req)
{
    std::string_view ct{};
    if (!is_req) {
        ct = request[boost::beast::http::field::accept];
        if (ct == "*"sv || ct == "*/*"sv)
            ct = {};
    }
    if (ct.empty())
        ct = request[boost::beast::http::field::content_type];
    return {
        std::find_if(content_type_json.begin(), content_type_json.end(),
            [ct](auto&& v) {
                return boost::beast::iequals(ct, v);
            }) != content_type_json.end(),
        ct
    };
}

std::pair<bool, std::string_view>
http_handler::protobuf_body(const http_request_type& request, bool is_req)
{
    std::string_view ct{};
    if (!is_req)
        ct = request[boost::beast::http::field::accept];
    if (ct.empty())
        ct = request[boost::beast::http::field::content_type];
    return {
        std::find_if(content_type_protobuf.begin(),
            content_type_protobuf.end(),
            [ct](auto&& v) {
                return boost::beast::iequals(ct, v);
            }) != content_type_protobuf.end(),
        ct
    };
}

} // namespace acppsrv
