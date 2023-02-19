#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace acppsrv::sqlite {

class connection;
class query;
class eval;

class connection {
public:
    // not using string_view, because sqlite3 requires null-terminated strings
    explicit connection(std::string file);
    connection(const connection&) = delete;
    connection(connection&&) = delete;
    ~connection();
    connection& operator=(const connection&) = delete;
    connection& operator=(connection&&) = delete;
    // May be called from any thread
    void interrupt();
private:
    class impl;
    std::string _file;
    std::unique_ptr<impl> _impl;
    friend class error;
    friend class query;
};

class query {
public:
    explicit query(connection& db, std::string sql, std::string sql_id = {});
    query(const query&) = delete;
    query(query&&) = delete;
    ~query();
    query& operator=(const query&) = delete;
    query& operator=(query&&) = delete;
    void start();
    void bind(int i, std::nullptr_t v);
    void bind(int i, int64_t v);
    void bind(int i, double v);
    void bind(int i, const std::string& v);
    void bind_blob(int i, const std::string& v);
private:
    class impl;
    connection& _db;
    std::string _sql;
    std::string _sql_id;
    std::unique_ptr<impl> _impl;
};

class eval {
public:
    eval(query& q);
};

class error: public std::runtime_error {
public:
    explicit error(std::string_view fun, const std::string& file);
    explicit error(std::string_view fun, connection& db,
                   const std::string& sql_id = {});
    // To be used before db._impl is initialized
    error(std::string_view fun, connection& db, connection::impl& impl);
};

} // namespace acppsrv::sqlite
