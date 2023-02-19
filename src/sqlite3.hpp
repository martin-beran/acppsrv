#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <variant>

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
    enum class column_type: int {
        ct_null = 0,
        ct_int64 = 1,
        ct_double = 2,
        ct_string = 3,
        ct_blob = 4,
    };
    using column_value = std::variant<
        std::nullptr_t,
        int64_t,
        double,
        std::string,
        std::string
    >;
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
    int column_count();
    bool next_row();
    column_value get_column(int i);
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
