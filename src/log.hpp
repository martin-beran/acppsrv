#pragma once

#include <atomic>
#include <iostream>
#include <optional>
#include <string>
#include <syncstream>

namespace acppsrv {

class logger;

class log {
public:
    enum class level {
        emerg = 0,
        alert = 1,
        crit = 2,
        err = 3,
        warning = 4,
        notice = 5,
        info = 6,
        debug = 7,
    };
    explicit log(level l);
    log(logger& obj, level l);
    log(const log&) = delete;
    log(log&&) = delete;
    ~log();
    log& operator=(const log&) = delete;
    log& operator=(log&&) = delete;
    template <class T> log&& operator<<(T&& v) && {
        if (_os)
            *_os << v;
        return std::move(*this);
    }
    [[nodiscard]] static std::string to_string(level l);
private:
    logger& _logger;
    level _level;
    std::optional<std::osyncstream> _os;
};

class DEBUG: public log {
#ifdef ENABLE_TEMPORARY_DEBUG
public:
#endif
    DEBUG(): log(level::alert) {
        std::move(*this) << " DEBUG";
    }
};

class logger {
public:
    [[nodiscard]] log::level level() const noexcept {
        return _level;
    }
    void level(log::level l) noexcept {
        _level = l;
    }
    static logger& global();
private:
    std::atomic<log::level> _level = log::level::info;
};

/*** log *********************************************************************/

inline log::log(level l): log(logger::global(), l)
{
}

} // namespace acppsrv
