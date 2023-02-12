#include "application.hpp"
#include "configuration.hpp"
#include "http_server.hpp"
#include "worker.hpp"
#include <boost/asio/signal_set.hpp>

namespace acppsrv {

bool application::run()
{
    auto tp = cfg.data().has_thread_pools() ?
        &cfg.data().thread_pools() : nullptr;
    // Create threads pools
    thread_pool control_pool(tp && tp->has_control() ? &tp->control() : nullptr,
                             std::string{pool_control_name});
    thread_pool main_pool(tp && tp->has_main() ? &tp->main() : nullptr,
                          std::string{pool_main_name});
    // Set up the control pool
    boost::asio::signal_set termsig{control_pool.ctx, SIGINT, SIGTERM};
    termsig.async_wait(
        [&main_pool](const boost::system::error_code& ec, int sig) {
            if (ec)
                log_msg(log_level::warning) <<
                    "Waiting for termination signal failed: " << ec.message();
            else {
                auto msg = log_msg(log_level::notice) <<
                    "Received termination signal ";
                switch (sig) {
                case SIGINT:
                    msg << "SIGINT";
                    break;
                case SIGTERM:
                    msg << "SIGTERM";
                    break;
                default:
                    msg << sig;
                    break;
                }
            }
            main_pool.stop();
        });
    // Set up the main pool
    http_server http_srv(cfg, main_pool);
    if (!http_srv.run()) {
        log_msg(log_level::crit) << "Cannot initialize HTTP server";
        return false;
    }
    // Start processing
    control_pool.run(false);
    main_pool.run(true);
    // We need pool's ctx to register an async op, so any async object
    // belonging to the pool must be created after the pool. But this causes
    // destruction of async objects before destruction of the pool, causing
    // cancellation of all async ops. Therefore we must call wait() explicitly
    // and not let it be executed implicitly by pool's destructor, because it
    // would cancel at least termsig, causing immediate termination of the
    // whole program.
    // Also, destruction of an async object from the main thread would create a
    // race condition with any operation on the same object performed by pool's
    // threads.
    control_pool.wait();
    main_pool.wait();
    return true;
}

} // namespace acppsrv
