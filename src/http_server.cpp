#include "http_server.hpp"
#include "configuration.hpp"
#include "worker.hpp"

#include <boost/asio.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/system/detail/error_code.hpp>

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

boost::asio::awaitable<void>
http_server::handle_connection(boost::asio::ip::tcp::socket conn)
{
    log_msg(log_level::info) << "Finished handling connection client=" <<
        conn.remote_endpoint();
    --active_connections;
    co_return;
}

boost::asio::awaitable<void> http_server::accept_loop()
{
    for (;;) {
        auto [ec, conn] = co_await acceptor.async_accept(
                             boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec)
            log_msg(log_level::err) << "Cannot accept connection: " <<
                ec.message();
        else {
            auto ac = ++active_connections;
            log_msg(log_level::info) << "Accepted connection client=" <<
                conn.remote_endpoint() << " active=" << ac;
            co_spawn(workers.ctx, handle_connection(std::move(conn)),
                     boost::asio::detached);
        }
    }
}

bool http_server::run()
{
    boost::system::error_code ec;
    boost::asio::ip::tcp::endpoint addr{boost::asio::ip::address{}, port};
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
                          boost::asio::detached);
    return true;
}

} // namespace acppsrv
