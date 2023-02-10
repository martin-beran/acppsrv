#include "configuration.hpp"

#include <google/protobuf/stubs/common.h>
#include <google/protobuf/stubs/logging.h>
#include <google/protobuf/util/json_util.h>

#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <type_traits>

namespace acppsrv {

configuration::configuration(const std::filesystem::path& cfg_file)
{
    // Read configuration file
    std::ifstream ifs{cfg_file};
    if (!ifs) {
        log_msg(log_level::crit) << "Cannot read configuration file " <<
            cfg_file;
        return;
    }
    std::stringbuf isb;
    ifs >> &isb;
    if (!ifs.eof()) {
        log_msg(log_level::crit) << "Configuration file " << cfg_file <<
            " not read to the end";
        return;
    }
    // Parse JSON configuration
    namespace gpb = google::protobuf;
    gpb::util::JsonParseOptions opts{};
    opts.ignore_unknown_fields = false;
    opts.case_insensitive_enum_parsing = false;
    if (auto status = gpb::util::JsonStringToMessage(isb.str(), &_cfg, opts);
        !status.ok())
    {
        log_err(cfg_file) << "(" << status.error_code() << ") " <<
            status.error_message();
        return;
    }
    // Validate configuration
    _valid = validate(cfg_file);
}

log_msg configuration::log_err(const std::filesystem::path& cfg_file) const
{
    return log_msg(log_level::crit) << "Error in configuration file " <<
        cfg_file << ": ";
}

int configuration::num_threads(const proto::ThreadPool* cfg)
{
    static_assert(std::is_same_v<
                  std::common_type_t<int, decltype(cfg->threads())>, int>);
    int n = cfg ? cfg->threads() : 0;
    if (n == 0)
        n = int(std::thread::hardware_concurrency());
    return std::max(n, 1);
}

bool configuration::validate(const std::filesystem::path& cfg_file)
{
    if (data().has_log())
        if (auto l = data().log().level(); l != proto::unspec) {
            if (auto ll = log_level(l - 2); acppsrv::valid(ll))
                _log_level = ll;
            else {
                log_err(cfg_file) << "Invalid log level " << l;
                return false;
            }
        }
    if (data().has_thread_pools()) {
        auto test = [this, &cfg_file](const proto::ThreadPool& p,
                                      std::string_view name)
        {
            if (p.threads() < 0) {
                log_err(cfg_file) << "Negative number of threads in pool " <<
                    name;
                return false;
            }
            return true;
        };
        using namespace std::string_view_literals;
        auto&& tp = data().thread_pools();
        if ((tp.has_main() && !test(tp.main(), "main"sv)) ||
            (tp.has_control() && !test(tp.control(), "control"sv)))
        {
            return false;
        }
    }
    return true;
}

} // namespace acppsrv
