#pragma once

#include <atomic>
#include <iostream>
#include <optional>
#include <string>
#include <syncstream>

namespace acppsrv {

class logger;

enum class log_level: int {
    off = -1, // disable logging
    emerg = 0, // this and following values are synced with syslog.h
    alert = 1,
    crit = 2,
    err = 3,
    warning = 4,
    notice = 5,
    info = 6,
    debug = 7,
};

[[nodiscard]] constexpr bool valid(log_level level) noexcept
{
    return level >= log_level::off && level <= log_level::debug;
}

[[nodiscard]] std::string to_string(log_level level);

[[nodiscard]] std::optional<log_level> from_string(std::string_view str);

class log_msg {
public:
    explicit log_msg(log_level level);
    log_msg(logger& obj, log_level level);
    log_msg(const log_msg&) = delete;
    log_msg(log_msg&&) = default;
    ~log_msg();
    log_msg& operator=(const log_msg&) = delete;
    log_msg& operator=(log_msg&&) = delete;
    template <class T> log_msg&& operator<<(T&& v) {
        if (_os)
            *_os << v;
        return std::move(*this);
    }
private:
    logger& _logger;
    log_level _level;
    std::optional<std::osyncstream> _os;
};

class DEBUG: public log_msg {
#ifdef ENABLE_TEMPORARY_DEBUG
public:
#endif
    DEBUG(): log_msg(log_level::emerg) {
        *this << "DEBUG ";
    }
};

class logger {
public:
    [[nodiscard]] log_level level() const noexcept {
        return _level;
    }
    void level(log_level level) noexcept {
        _level = level;
    }
    static logger& global();
private:
    std::atomic<log_level> _level = log_level::info;
};

/*** log_msg *****************************************************************/

inline log_msg::log_msg(log_level level): log_msg(logger::global(), level)
{
}

} // namespace acppsrv
