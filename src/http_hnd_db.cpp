#include "http_hnd_db.hpp"
#include "http_hnd_db.pb.h"
#include "db_server.hpp"
#include "db_server_impl.hpp"
#include "http_server_impl.hpp"
#include "log.hpp"
#include <boost/asio/use_awaitable.hpp>

namespace acppsrv::http_hnd {

template <class Executor,
         boost::asio::completion_token_for<void(proto::db::Response)> CT>
auto db::call_query(Executor executor, proto::db::Request&& request, CT&& token)
{
    auto init =
        [this, executor, &request]<class Handler>(Handler&& handler) mutable {
            db_srv.query(std::move(request),
                [executor, handler = std::forward<Handler>(handler)](
                    proto::db::Response response
                ) mutable {
                    boost::asio::post(executor,
                        [handler = std::move(handler),
                        response = std::move(response)]() mutable {
                            handler(std::move(response));
                        });
                });
        };
    return boost::asio::async_initiate<CT, void(proto::db::Response)>(init,
                                                                      token);
}

boost::asio::awaitable<http_handler::http_response_type>
// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
db::handle_async(const http_request_type& request, uint64_t sid, uint64_t req_n)
{
    auto [in, response] = parse<proto::db::Request>(request, sid,  req_n);
    if (!in)
        co_return response;
    auto executor = co_await boost::asio::this_coro::executor;
    proto::db::Response out = co_await call_query(executor, std::move(*in),
                                                  boost::asio::use_awaitable);
    serialize(request, sid, req_n, out, response);
    co_return response;
}

} // namespace acppsrv::http_hnd
