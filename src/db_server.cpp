#include "db_server.hpp"
#include "configuration.hpp"
#include "http_hnd_db.pb.h"
#include "log.hpp"
#include "worker.hpp"
#include <tuple>
#include <utility>

namespace acppsrv {

db_server::db_server(const proto::SQLite3* cfg, thread_pool& workers):
    cfg(cfg), workers(workers)
{
}

void db_server::bind(sqlite::query& q, int i,
                     const http_hnd::proto::db::Value& v)
{
    using V = http_hnd::proto::db::Value;
    switch (v.val_case()) {
    case V::VAL_NOT_SET:
    case V::kVNull:
    default:
        q.bind(i, nullptr);
        break;
    case V::kVInt64:
        q.bind(i, v.v_int64());
        break;
    case V::kVDouble:
        q.bind(i, v.v_double());
        break;
    case V::kVText:
        q.bind(i, v.v_text());
        break;
    case V::kVBlob:
        q.bind_blob(i, v.v_blob());
    }
}

void db_server::interrupt()
{
    for (size_t tidx = 1; auto&& db_map: databases) {
        log_msg(log_level::debug) << "Interrupting database thread " <<
            tidx++ << '/' << workers.size();
        for (auto&& db: db_map)
            db.second.db.interrupt();
    }
}

bool db_server::run()
{
    try {
        for (size_t tidx = 0; tidx < workers.size(); ++tidx) {
            log_msg(log_level::notice) << "Initializing database thread " <<
                (tidx + 1) << '/' << workers.size();
            databases.emplace_back();
            auto& current = databases.back();
            auto level = tidx == 0 ? log_level::notice : log_level::debug;
            if (cfg) {
                for (auto&& [d_name, d_def]: cfg->sqlite3()) {
                    log_msg(level) << "Open database=\"" << d_name <<
                        "\" file=" << d_def.file() << '"';
                    auto& db = current.
                        emplace(std::piecewise_construct,
                                std::forward_as_tuple(d_name),
                                std::forward_as_tuple(d_def.file())).
                        first->second;
                    for (auto&& [q_name, q_def]: d_def.queries()) {
                            log_msg(level) << "Preparing database=\"" <<
                                d_name << "\" query=\"" << q_name <<
                                "\" sql=\"" << q_def << '"';
                            db.queries.emplace(std::piecewise_construct,
                                               std::forward_as_tuple(q_name),
                                               std::forward_as_tuple(db.db,
                                                                     q_def,
                                                                     q_name));
                    }
                }
            }
        }
    } catch (const sqlite::error& e) {
        log_msg(log_level::crit) << "Opening databases: " << e.what();
        return false;
    }
    return true;
}

http_hnd::proto::db::Response
db_server::run_query(http_hnd::proto::db::Request& request)
{
    http_hnd::proto::db::Response response;
    try {
        const std::string& db_id = request.db();
        const std::string& query_id = request.query();
        size_t tid = thread_pool::this_thread();
        auto& database = databases.at(tid);
        auto db = database.find(db_id);
        if (db == database.end()) {
            response.set_ok(false);
            response.set_msg("Unknown database \"" + db_id + '"');
            return response;
        }
        auto query = db->second.queries.find(query_id);
        if (query == db->second.queries.end()) {
            response.set_ok(false);
            response.set_msg("Unknown query \"" + query_id +
                             "\" for database \"" + db_id + '"');
            return response;
        }
        //auto& db_c = db->second.db;
        auto& db_q = query->second;
        db_q.start();
        for (int i = 0; i < request.args_size(); ++i)
            bind(db_q, i, request.args(i));
        DEBUG() << "Running database=\"" << request.db() <<
            "\" query=\"" << request.query() << '"' <<
            " thread=" << thread_pool::this_thread() << '/' << workers.size();
            response.set_ok(true);
            response.set_msg("ok");
    } catch (sqlite::error& e) {
        response.set_ok(false);
        response.set_msg(e.what());
    }
    return response;
}

} // namespace acppsrv
