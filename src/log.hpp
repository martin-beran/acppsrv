#pragma once

#include <atomic>
#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <syncstream>
#include <variant>

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
    using session_type = std::array<std::optional<uint64_t>, 2>;
    explicit log_msg(log_level level, session_type session = {});
    log_msg(logger& obj, log_level level, session_type = {});
    log_msg(const log_msg&) = delete;
    log_msg(log_msg&&) = default;
    ~log_msg();
    log_msg& operator=(const log_msg&) = delete;
    log_msg& operator=(log_msg&&) = delete;
    template <class T> log_msg& operator<<(T&& v) & {
        if (_os)
            *_os << v;
        return *this;
    }
    template <class ...T> log_msg& operator<<(std::variant<T...>&& v) & {
        if (_os)
            std::visit([this]<class V>(V&& v) {
                *_os << std::forward<V>(v);
            }, std::forward<std::variant<T...>>(v));
        return *this;
    }
    template <class T> log_msg&& operator<<(T&& v) && {
        *this << std::forward<T>(v);
        return std::move(*this);
    }
private:
    logger& _logger;
    log_level _level;
    std::optional<std::osyncstream> _os;
    std::optional<session_type> _session;
};

class DEBUG: public log_msg {
#ifdef ENABLE_TEMPORARY_DEBUG
public:
#endif
    explicit DEBUG(session_type session = {}):
        log_msg(log_level::emerg, session)
    {
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

inline log_msg::log_msg(log_level level, session_type session):
    log_msg(logger::global(), level, session)
{
}

} // namespace acppsrv
