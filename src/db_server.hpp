#pragma once

#include "sqlite3.hpp"

#include <map>
#include <vector>

namespace acppsrv {

class thread_pool;

namespace proto {

class SQLite3;

} // namespace proto

class db_server {
public:
    db_server(const proto::SQLite3* cfg, thread_pool& workers);
    bool run();
private:
    struct db_def_t {
        explicit db_def_t(const std::string& file): db(file) {}
        sqlite::connection db;
        std::map<std::string, sqlite::query> queries;
    };
    const proto::SQLite3* cfg;
    thread_pool& workers;
    std::vector<std::map<std::string, db_def_t>> databases;
};

} // namespace acppsrv
