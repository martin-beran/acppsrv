#include "sqlite3.hpp"
#include "log.hpp"

#include <cassert>
#include <sqlite3.h>

namespace acppsrv::sqlite {

/*** connection::impl ********************************************************/

class connection::impl {
public:
    explicit impl(connection& conn);
    impl(const impl&) = delete;
    impl(impl&&) = delete;
    ~impl();
    impl& operator=(const impl&) = delete;
    impl& operator=(impl&&) = delete;
    connection& conn;
    sqlite3* db = nullptr;
};

connection::impl::impl(connection& conn): conn(conn)
{
    if (int status = sqlite3_open_v2(conn._file.c_str(), &db,
                                     SQLITE_OPEN_READWRITE |
                                     SQLITE_OPEN_URI |
                                     SQLITE_OPEN_NOMUTEX |
                                     SQLITE_OPEN_EXRESCODE,
                                     nullptr);
        status != SQLITE_OK)
    {
        if (!db)
            throw error(conn._file);
        throw error(conn, *this);
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

connection::connection(std::string file):
    _file(std::move(file)), _impl(std::make_unique<impl>(*this))
{
}

connection::~connection() = default;

/*** query::impl *************************************************************/

class query::impl {
public:
    explicit impl(query& q);
    impl(const impl&) = delete;
    impl(impl&&) = delete;
    ~impl();
    impl& operator=(const impl&) = delete;
    impl& operator=(impl&&) = delete;
private:
    query& q;
    sqlite3_stmt* stmt = nullptr;
};

query::impl::impl(query& q): q(q)
{
    // sqlite3 allows passing size incl. terminating NUL
    if (sqlite3_prepare_v3(q._db._impl->db, q._sql.c_str(),
                           int(q._sql.size() + 1),
                           SQLITE_PREPARE_PERSISTENT, &stmt,
                           nullptr) != SQLITE_OK)
    {
        assert(!stmt);
        // this->q to silence compiler warning about unused q
        throw error(this->q._db, this->q._sql_id);
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

/*** error *******************************************************************/

error::error(const std::string& file):
    runtime_error("sqlite3 error in db \"" + file + "\"" +
                  ": Cannot allocate database handle")
{
}

error::error(connection& db, const std::string& sql_id):
        runtime_error("sqlite3 error in db \"" + db._file + "\"" +
                      (sql_id.empty() ? "" : (" (" + sql_id + ")")) +
                      ": " + sqlite3_errmsg(db._impl->db))
{
}

error::error(connection& db, connection::impl& impl):
        runtime_error("sqlite3 error in db \"" + db._file + "\"" +
                      ": " + sqlite3_errmsg(impl.db))
{
}

} // namespace acppsrv::sqlite
