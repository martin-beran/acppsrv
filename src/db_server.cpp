#include "db_server.hpp"
#include "configuration.hpp"
#include "finally.hpp"
#include "http_hnd_db.pb.h"
#include "log.hpp"
#include "worker.hpp"
#include <chrono>
#include <mutex>
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

void db_server::execute_query(const db_def_t& db, sqlite::query& q,
                              uint32_t retries,
                              http_hnd::proto::db::Response& response)
{
    int columns = q.column_count();
    util::finally signal_finished([&db]() noexcept {
        std::lock_guard lck(db.sync->mtx);
        db.sync->finished = true;
        db.sync->cond.notify_one();
    });
    for (;;) {
        auto res = q.next_row(retries);
        if (res == sqlite::query::status::done)
            break;
        if (res == sqlite::query::status::locked) {
            assert(retries > 0);
            --retries;
            if (db.retry_wait > std::chrono::seconds{}) {
                std::unique_lock lck(db.sync->mtx);
                db.sync->cond.wait_for(lck, db.retry_wait,
                                       [&db]() { return db.sync->finished; });
                db.sync->finished = false;
            }
            q.start(true);
            continue;
        }
        auto row = response.add_rows();
        using ct = sqlite::query::column_type;
        for (int c = 0; c < columns; ++c) {
            auto val = q.get_column(c);
            auto col = row->add_columns();
            switch (static_cast<ct>(val.index())) {
            case ct::ct_null:
            default:
                col->set_v_null(http_hnd::proto::db::Null::NULL_);
                break;
            case ct::ct_int64:
                col->set_v_int64(std::get<int64_t>(val));
                break;
            case ct::ct_double:
                col->set_v_double(std::get<double>(val));
                break;
            case ct::ct_string:
                col->set_v_text(std::get<int(ct::ct_string)>(val));
                break;
            case ct::ct_blob:
                col->set_v_blob(std::get<int(ct::ct_blob)>(val));
                break;
            }
        }
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
                    db.retries = d_def.retries();
                    if (auto t = configuration::get_time(d_def.retry_wait()))
                        db.retry_wait = *t < max_retry_wait ?
                            *t : max_retry_wait;
                    if (tidx == 0)
                        db.sync = std::make_shared<db_sync_t>();
                    else
                        db.sync = databases[0].at(d_name).sync;
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
        db_q.start(false);
        for (int i = 0; i < request.args_size(); ++i)
            bind(db_q, i, request.args(i));
        execute_query(db->second, db_q,
                      request.retry_if_locked() ? db->second.retries : 0,
                      response);
        response.set_ok(true);
        response.set_msg("ok");
    } catch (sqlite::error& e) {
        response.clear_rows();
        response.set_ok(false);
        response.set_msg(e.what());
    }
    return response;
}

} // namespace acppsrv
