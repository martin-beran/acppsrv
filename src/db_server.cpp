#include "db_server.hpp"
#include "configuration.hpp"
#include "log.hpp"
#include "worker.hpp"
#include <tuple>
#include <utility>

namespace acppsrv {

db_server::db_server(const proto::SQLite3* cfg, thread_pool& workers):
    cfg(cfg), workers(workers)
{
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

} // namespace acppsrv
