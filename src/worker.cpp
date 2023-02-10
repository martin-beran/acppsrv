#include "worker.hpp"
#include "application.hpp"
#include "configuration.hpp"
#include <boost/asio/executor_work_guard.hpp>

namespace acppsrv {

thread_pool::thread_pool(const proto::ThreadPool* cfg, std::string name):
    ctx(configuration::num_threads(cfg)), name(std::move(name)),
    threads(size_t(configuration::num_threads(cfg)))
{
    assert(!threads.empty());
    log_msg(log_level::notice) << "Created thread pool \"" << this->name <<
        "\" with " << threads.size() << " threads";
}

thread_pool::~thread_pool()
{
    wait();
    log_msg(log_level::notice) << "Destroyed thread pool \"" << name << '"';
}

void thread_pool::run(bool persistent)
{
    assert(!threads.front().joinable()); // not already started
    if (!persistent)
        work.reset();
    for (size_t t = 0; t < threads.size(); ++t)
        threads[t] = std::thread(
            [this, t, num_threads = threads.size()]() {
                log_msg(log_level::debug) << "Starting thread " << (t + 1) <<
                    '/' << num_threads << " in pool \"" << name << '"';
                while (!ctx.stopped())
                    try {
                        ctx.run();
                    } catch (std::exception& e) {
                        log_msg(log_level::warning) <<
                            "Restarting thread in pool \"" << name <<
                            "\" after exception: " << e.what();
                    } catch (...) {
                        log_msg(log_level::warning) <<
                            "Restarting thread in pool\"" << name <<
                            "\" after unknown exception";
                    }
                log_msg(log_level::debug) << "Stopped thread " << (t + 1) <<
                    '/' << num_threads << " in pool \"" << name << '"';
            });
    log_msg(log_level::notice) << "Thread pool \"" << name <<
        "\" started with " << threads.size() << " threads";
}

void thread_pool::stop()
{
    log_msg(log_level::notice) << "Stopping thread pool \"" << name <<
        "\" with " << threads.size() << " threads";
    ctx.stop();
}

void thread_pool::wait()
{
    if (threads.front().joinable()) {
        log_msg(log_level::notice) << "Waiting for " << threads.size() <<
            " threads in pool \"" << name << '"';
        for (auto&& t: threads)
            t.join();
        log_msg(log_level::notice) << "Finished threads in pool \"" <<
            name << '"';
    }
}

} // namespace acppsrv
