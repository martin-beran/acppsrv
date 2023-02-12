#include "http_server.hpp"
#include "configuration.hpp"
#include "finally.hpp"
#include "worker.hpp"

#include <boost/asio.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/detail/error_code.hpp>
#include <exception>

// declared const, but not constexpr or inline, therefore a definition is
// needed at namespace scope
const int boost::asio::socket_base::max_listen_connections;

namespace acppsrv {

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
        if (auto v = http.max_req_line())
            max_request_line = v;
        if (auto v = http.max_req_headers())
            max_request_headers = v;
        if (auto v = http.max_req_body())
            max_request_body = v;
    }
}

template <boost::asio::completion_token_for<void()> CompletionToken>
auto http_server::conn_limit(CompletionToken&& token)
{
    auto init = [this]<class Handler>(Handler&& handler) {
        std::unique_lock lck{active_connections_mtx};
        if (!max_connections || active_connections < *max_connections)
            boost::asio::post(acceptor.get_executor(),
                              std::forward<Handler>(handler));
        else {
            active_connections_hnd.emplace(std::forward<Handler>(handler));
            lck.unlock();
            log_msg(log_level::warning) <<
                "Reached maximum number of connections " << *max_connections;
        }
    };
    return boost::asio::async_initiate<CompletionToken, void()>(std::move(init),
                                                                token);
}

boost::asio::awaitable<void> http_server::accept_loop()
{
    for (;;) {
        co_await conn_limit(boost::asio::use_awaitable);
        {
            auto msg = log_msg(log_level::debug) <<
                "Waiting for connection active_connections=" <<
                active_connections << " maximum=";
            log_limit(msg, max_connections);
        }
        auto [ec, conn] = co_await acceptor.async_accept(
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
                {
                    auto msg = log_msg(log_level::info) <<
                        "Accepted connection client=" <<
                        conn.remote_endpoint() << " active=" << ac <<
                        " maximum=";
                    log_limit(msg, max_connections);
                }
                co_spawn(workers.ctx, handle_connection(std::move(conn),
                                                        std::move(client)),
                         co_spawn_handler);
        }
    }
}

void http_server::co_spawn_handler(std::exception_ptr e)
{
    if (e)
        std::rethrow_exception(e);
}

template <class Endpoint>
void http_server::active_connection_end(const Endpoint& client)
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
    auto msg = log_msg(log_level::info) <<
        "Finished handling connection client=" << client << " active=" << ac <<
        " maximum=";
    log_limit(msg, max_connections);
}

boost::asio::awaitable<void>
http_server::handle_connection(socket_type conn, endpoint_type client)
{
    util::finally at_end([this, &client]() { active_connection_end(client); });
    log_msg(log_level::debug) << "Waiting for data from client";
    boost::system::error_code ec;
    std::array<char, 1> buf{};
    co_await conn.async_read_some(boost::asio::buffer(buf),
                         boost::asio::redirect_error(boost::asio::use_awaitable,
                                                     ec));
    if (ec)
        log_msg(log_level::err) << "Cannot read from client " << client <<
            ": " << ec.message();
    co_return;
}

template <class T> void http_server::log_limit(log_msg& msg,
                                               const std::optional<T>& limit)
{
    if (limit)
        msg << *limit;
    else
        msg << "unlimited";
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
