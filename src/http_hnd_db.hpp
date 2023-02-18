#pragma once

#include "http_server.hpp"

namespace acppsrv {

class db_server;

namespace http_hnd {

namespace proto::db {

class Request;
class Response;

} // namespace proto::db

class db: public http_handler {
public:
    explicit db(db_server& db_srv): db_srv(db_srv) {}
    bool async() override {
        return true;
    }
    boost::asio::awaitable<http_response_type>
        handle_async(const http_request_type& request,
                     uint64_t sid, uint64_t req_n) override;
private:
    template <class Executor,
             boost::asio::completion_token_for<void(proto::db::Response)> CT>
        auto call_query(Executor executor, proto::db::Request&& request,
                        CT&& token);
    db_server& db_srv;
};

} // namespace http_hnd

} // namespace acppsrv
