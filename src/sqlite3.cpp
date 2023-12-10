#include "sqlite3.hpp"
#include "log.hpp"

#include <cassert>
#include <sqlite3.h>
#include <utility>

namespace acppsrv::sqlite {

/*** connection::impl ********************************************************/

class connection::impl {
public:
    explicit impl(connection& conn, bool create);
    impl(const impl&) = delete;
    impl(impl&&) = delete;
    ~impl();
    impl& operator=(const impl&) = delete;
    impl& operator=(impl&&) = delete;
    connection& conn;
    sqlite3* db = nullptr;
};

connection::impl::impl(connection& conn, bool create): conn(conn)
{
    if (int status = sqlite3_open_v2(conn._file.c_str(), &db,
                                     SQLITE_OPEN_READWRITE |
                                     (create ? SQLITE_OPEN_CREATE : 0) |
                                     SQLITE_OPEN_URI |
                                     SQLITE_OPEN_NOMUTEX |
                                     SQLITE_OPEN_PRIVATECACHE |
                                     SQLITE_OPEN_EXRESCODE,
                                     nullptr);
        status != SQLITE_OK)
    {
        if (!db)
            throw error("sqlite3_open_v2", conn._file);
        throw error("sqlite3_open_v2", conn, *this);
    }
    if (int status = sqlite3_exec(db, "pragma synchronous = normal", nullptr,
                                  nullptr, nullptr);
        status != SQLITE_OK)
    {
        throw error("sqlite3_exec(pragma)", conn, *this);
    }
    assert(db);
}

connection::impl::~impl()
{
    if (sqlite3_close_v2(db) != SQLITE_OK) {
        assert(db);
        log_msg(log_level::emerg) << "Cannot close database \"" << conn._file <<
            "\" handle: " << sqlite3_errmsg(db);
    } else
        log_msg(log_level::debug) << "Closed database \"" << conn._file << '"';
}

/*** connection **************************************************************/

connection::connection(std::string file, bool create):
    _file(std::move(file)), _impl(std::make_unique<impl>(*this, create))
{
}

connection::~connection() = default;

void connection::interrupt()
{
    if (_impl->db) {
        sqlite3_interrupt(_impl->db);
        log_msg(log_level::debug) << "Interrupted database file \"" << _file <<
            '"';
    }
}

/*** query::impl *************************************************************/

class query::impl {
public:
    explicit impl(query& q);
    impl(const impl&) = delete;
    impl(impl&&) = delete;
    ~impl();
    impl& operator=(const impl&) = delete;
    impl& operator=(impl&&) = delete;
    [[maybe_unused]] query& q;
    sqlite3_stmt* stmt = nullptr;
};

query::impl::impl(query& q): q(q)
{
    log_msg(log_level::debug) << "Prepare SQL query: " << q._sql;
    // sqlite3 allows passing size incl. terminating NUL
    if (sqlite3_prepare_v3(q._db._impl->db, q._sql.c_str(),
                           int(q._sql.size() + 1),
                           SQLITE_PREPARE_PERSISTENT, &stmt,
                           nullptr) != SQLITE_OK)
    {
        assert(!stmt);
        throw error("sqlite3_prepare_v3", q._db, q._sql_id);
    }
    assert(stmt);
}

query::impl::~impl()
{
    sqlite3_finalize(stmt);
}

/*** query *******************************************************************/

query::query(connection& db, std::string sql, std::string sql_id):
    _db(db), _sql(std::move(sql)), _sql_id(std::move(sql_id)),
    _impl(std::make_unique<impl>(*this))
{
}

query::~query() = default;

query& query::bind(int i, std::nullptr_t)
{
    if (sqlite3_bind_null(_impl->stmt, i + 1) != SQLITE_OK)
        throw error("sqlite3_bind_null", _db, _sql_id);
    return *this;
}

query& query::bind(int i, int64_t v)
{
    if (sqlite3_bind_int64(_impl->stmt, i + 1, v) != SQLITE_OK)
        throw error("sqlite3_bind_int64", _db, _sql_id);
    return *this;
}

query& query::bind(int i, double v)
{
    if (sqlite3_bind_double(_impl->stmt, i + 1, v) != SQLITE_OK)
        throw error("sqlite3_bind_double", _db, _sql_id);
    return *this;
}

query& query::bind(int i, const std::string& v)
{
    if (sqlite3_bind_text(_impl->stmt, i + 1, v.c_str(), int(v.size()),
                          SQLITE_STATIC) != SQLITE_OK)
    {
        throw error("sqlite3_bind_text", _db, _sql_id);
    }
    DEBUG() << "bind i=" << i << " v=" << v;
    return *this;
}

query& query::bind_blob(int i, const std::string& v)
{
    if (sqlite3_bind_blob(_impl->stmt, i + 1, v.c_str(), int(v.size()),
                          SQLITE_STATIC) != SQLITE_OK)
    {
        throw error("sqlite3_bind_blob", _db, _sql_id);
    }
    return *this;
}

int query::column_count()
{
    return sqlite3_column_count(_impl->stmt);
}

query::column_value query::get_column(int i)
{
    switch (sqlite3_column_type(_impl->stmt, i)) {
    case SQLITE_NULL:
    default:
        return nullptr;
    case SQLITE_INTEGER:
        return sqlite3_column_int64(_impl->stmt, i);
    case SQLITE_FLOAT:
        return sqlite3_column_double(_impl->stmt, i);
    case SQLITE_TEXT:
        return column_value(
            std::in_place_index<int(column_type::ct_string)>,
            reinterpret_cast<const char*>(sqlite3_column_text(_impl->stmt, i)),
            sqlite3_column_bytes(_impl->stmt, i));
    case SQLITE_BLOB:
        return column_value(
            std::in_place_index<int(column_type::ct_blob)>,
            reinterpret_cast<const char*>(sqlite3_column_blob(_impl->stmt, i)),
            sqlite3_column_bytes(_impl->stmt, i));
    }
}

query::status query::next_row(uint32_t retries)
{
    switch (auto status = sqlite3_step(_impl->stmt); status % 256) {
    case SQLITE_DONE:
        return status::done;
    case SQLITE_ROW:
        return status::row;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        if (retries > 0) {
            log_msg(log_level::debug) << "Database \"" + _db._file +
                "\" locked in query \"" + _sql_id + "\" retries=" << retries;
            return status::locked;
        }
        [[fallthrough]];
    default:
        throw error("sqlite3_step", _db, _sql_id);
    }
}

query& query::start(bool restart)
{
    sqlite3_reset(_impl->stmt);
    if (!restart && sqlite3_clear_bindings(_impl->stmt) != SQLITE_OK)
        throw error("sqlite3_clear_bindings", _db, _sql_id);
    return *this;
}

/*** error *******************************************************************/

error::error(std::string_view fun, const std::string& file):
    runtime_error("sqlite3 error in db \"" + file + "\"" +
                  ": Cannot allocate database handle")
{
    log_msg(log_level::debug) << "Failed call " << fun << "()";
}

error::error(std::string_view fun, connection& db, const std::string& sql_id):
        runtime_error("sqlite3 error in db \"" + db._file + "\"" +
                      (sql_id.empty() ? "" : (" (" + sql_id + ")")) +
                      ": " + sqlite3_errmsg(db._impl->db))
{
    log_msg(log_level::debug) << "Failed call " << fun << "()";
}

error::error(std::string_view fun, connection& db, connection::impl& impl):
        runtime_error("sqlite3 error in db \"" + db._file + "\"" +
                      ": " + sqlite3_errmsg(impl.db))
{
    log_msg(log_level::debug) << "Failed call " << fun << "()";
}

} // namespace acppsrv::sqlite
