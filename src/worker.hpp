#pragma once

#include <boost/asio.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <string>

namespace acppsrv {

class application;

namespace proto {

class ThreadPool;

} // namespace proto

class thread_pool {
public:
    thread_pool(const proto::ThreadPool* cfg, std::string name);
    thread_pool(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = delete;
    ~thread_pool();
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool& operator=(thread_pool&&) = delete;
    void run(bool persistent);
    void stop();
    void wait();
    boost::asio::io_context ctx;
private:
    const std::string name;
    std::vector<std::thread> threads;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        work{ctx.get_executor()};
};

} // namespace acppsrv
