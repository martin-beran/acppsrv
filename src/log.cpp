#include "log.hpp"

#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <ctime>
#include <span>
#include <thread>

namespace acppsrv {

/*** log *********************************************************************/

log::log(logger& obj, level l):
     _logger(obj), _level(l)
{
    namespace sc = std::chrono;
    if (_level <= _logger.level()) {
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
        std::move(*this) << ' ' << to_string(_level);
    }
}

log::~log()
{
    if (_os)
        *_os << std::endl;
}

std::string log::to_string(level l)
{
    using namespace std::string_literals;
    switch (l) {
    case level::emerg:
        return "E"s;
    case level::alert:
        return "A"s;
    case level::crit:
        return "C"s;
    case level::err:
        return "E"s;
    case level::warning:
        return "W"s;
    case level::notice:
        return "N"s;
    case level::info:
        return "I"s;
    case level::debug:
        return "D"s;
    default:
        return "?"s;
    }
}

/*** logger ******************************************************************/

logger& logger::global()
{
    static logger gl_logger{};
    return gl_logger;
}

} // namespace acppsrv
