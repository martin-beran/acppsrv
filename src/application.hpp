#pragma once

#include "sqlite3.hpp"

#include <map>
#include <string_view>

namespace acppsrv {

class configuration;
class thread_pool;

class application {
public:
    static constexpr std::string_view name = "ACppSrv";
    static constexpr std::string_view version = GIT_VERSION;
    static constexpr std::string_view pool_main_name{"main"};
    static constexpr std::string_view pool_control_name{"control"};
    static constexpr std::string_view pool_database_name{"database"};
    explicit application(const configuration& cfg): cfg(cfg) {}
    application(const application&) = delete;
    application(application&&) = delete;
    ~application() = default;
    application& operator=(const application&) = delete;
    application& operator=(application&&) = delete;
    bool run();
private:
    const configuration& cfg;
};

} // namespace acppsrv
