#pragma once

#include "db_server.hpp"
#include "http_hnd_db.pb.h"
#include "worker.hpp"

namespace acppsrv {

template <std::invocable<http_hnd::proto::db::Response> Handler>
void db_server::query(http_hnd::proto::db::Request&& request, Handler&& handler)
{
    boost::asio::post(workers.ctx,
        [this, request = std::move(request),
        handler = std::forward<Handler>(handler)]() mutable
        {
            auto response = run_query(request);
            handler(std::move(response));
        });
}

} // namespace acppsrv
