#pragma once

#include "sqlite3.hpp"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

namespace acppsrv {

class thread_pool;

namespace proto {

class SQLite3;

} // namespace proto

namespace http_hnd::proto::db {

class Value;
class Request;
class Response;

} // namespace http_hnd::proto::db

class db_server {
public:
    // now() + retry_wait must not overflow
    static constexpr std::chrono::minutes max_retry_wait{1};
    db_server(const proto::SQLite3* cfg, thread_pool& workers);
    bool run();
    // May be called from any thread
    template <std::invocable<http_hnd::proto::db::Response> Handler>
    void query(http_hnd::proto::db::Request&& request, Handler&& handler); 
    // May be called from any thread
    void interrupt();
private:
    struct db_sync_t {
        bool finished = false;
        std::mutex mtx;
        std::condition_variable cond;
    };
    struct db_def_t {
        explicit db_def_t(const std::string& file): db(file) {}
        sqlite::connection db;
        std::map<std::string, sqlite::query> queries;
        uint32_t retries = 0;
        std::chrono::nanoseconds retry_wait = {};
        std::shared_ptr<db_sync_t> sync;
    };
    http_hnd::proto::db::Response
        run_query(http_hnd::proto::db::Request& request);
    static void bind(sqlite::query& q, int i,
                     const http_hnd::proto::db::Value& v);
    static void execute_query(const db_def_t& db, sqlite::query& q,
                              uint32_t retries,
                              http_hnd::proto::db::Response& response);
    const proto::SQLite3* cfg;
    thread_pool& workers;
    std::vector<std::map<std::string, db_def_t>> databases;
};

} // namespace acppsrv
