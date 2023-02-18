#pragma once

#include "sqlite3.hpp"

#include <map>
#include <vector>

namespace acppsrv {

class thread_pool;

namespace proto {

class SQLite3;

} // namespace proto

namespace http_hnd::proto::db {

class Request;
class Response;

} // namespace http_hnd::proto::db

class db_server {
public:
    db_server(const proto::SQLite3* cfg, thread_pool& workers);
    bool run();
    // May be called from any thread
    template <std::invocable<http_hnd::proto::db::Response> Handler>
    void query(http_hnd::proto::db::Request&& request, Handler&& handler); 
    // May be called from any thread
    void interrupt();
private:
    struct db_def_t {
        explicit db_def_t(const std::string& file): db(file) {}
        sqlite::connection db;
        std::map<std::string, sqlite::query> queries;
    };
    http_hnd::proto::db::Response
        run_query(http_hnd::proto::db::Request& request);
    const proto::SQLite3* cfg;
    thread_pool& workers;
    std::vector<std::map<std::string, db_def_t>> databases;
};

} // namespace acppsrv
