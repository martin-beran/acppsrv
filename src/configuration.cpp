#include "configuration.hpp"

#include <google/protobuf/stubs/common.h>
#include <google/protobuf/stubs/logging.h>
#include <google/protobuf/util/json_util.h>

#include <fstream>
#include <sstream>

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
        log_msg(log_level::crit) << "Error in configuration file " <<
            cfg_file << ": (" << status.error_code() << ") " <<
            status.error_message();
        return;
    }
    // Validate configuration
    _valid = validate(cfg_file);
}

bool configuration::validate(const std::filesystem::path& cfg_file)
{
    if (data().has_log())
        if (auto l = data().log().level(); l != proto::unspec) {
            if (auto ll = log_level(l - 2); acppsrv::valid(ll))
                _log_level = ll;
            else {
                log_msg(log_level::crit) << "Error in onfiguration file " <<
                    cfg_file << ": Invalid log level " << l;
                return false;
            }
        }
    return true;
}

} // namespace acppsrv
