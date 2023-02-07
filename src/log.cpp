#include "log.hpp"

#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <ctime>
#include <map>
#include <span>
#include <thread>

namespace acppsrv {

std::string to_string(log_level l)
{
    using namespace std::string_literals;
    switch (l) {
    case log_level::off:
        return "-"s;
    case log_level::emerg:
        return "X"s;
    case log_level::alert:
        return "A"s;
    case log_level::crit:
        return "C"s;
    case log_level::err:
        return "E"s;
    case log_level::warning:
        return "W"s;
    case log_level::notice:
        return "N"s;
    case log_level::info:
        return "I"s;
    case log_level::debug:
        return "D"s;
    default:
        return "?"s;
    }
}

std::optional<log_level> from_string(std::string_view str)
{
    using namespace std::string_view_literals;
    static const std::map<std::string_view, log_level> levels{
        {"-", log_level::off},
        {"off", log_level::off},

        {"X", log_level::emerg},
        {"emerg", log_level::emerg},
        {"emergency", log_level::emerg},

        {"A", log_level::alert},
        {"alert", log_level::alert},

        {"C", log_level::crit},
        {"crit", log_level::crit},
        {"critical", log_level::crit},

        {"E", log_level::err},
        {"err", log_level::err},
        {"error", log_level::err},

        {"W", log_level::warning},
        {"warning", log_level::warning},
        {"warn", log_level::warning},

        {"N", log_level::notice},
        {"notice", log_level::notice},

        {"I", log_level::info},
        {"info", log_level::info},

        {"D", log_level::debug},
        {"debug", log_level::debug},
    };
    if (auto l = levels.find(str); l != levels.end())
        return l->second;
    else
        return std::nullopt;
}

/*** log_msg *****************************************************************/

log_msg::log_msg(logger& obj, log_level level):
     _logger(obj), _level(level)
{
    namespace sc = std::chrono;
    if (_level > log_level::off && _level <= _logger.level()) {
        _os.emplace(std::cerr);
        auto now = sc::system_clock::now().time_since_epoch();
        std::array<char, 64> buf{};
        time_t time = sc::duration_cast<sc::seconds>(now).count();
        auto usec =
            std::to_string(sc::duration_cast<sc::microseconds>(now).count() %
                           1'000'000);
        tm t{};
        localtime_r(&time, &t);
        assert(strftime(buf.data(), buf.size(), "%FT%T.000000%z", &t) != 0);
        static constexpr size_t time_end = 26; // after .000000
        std::copy(usec.begin(), usec.end(),
                  buf.data() + time_end - usec.size());
        std::move(*this) << buf.data();
        std::move(*this) << " [" << getpid();
        std::move(*this) << '.' << std::this_thread::get_id() << ']';
        std::move(*this) << ' ' << to_string(_level) << ' ';
    }
}

log_msg::~log_msg()
{
    if (_os)
        *_os << std::endl;
}

/*** logger ******************************************************************/

logger& logger::global()
{
    static logger gl_logger{};
    return gl_logger;
}

} // namespace acppsrv
