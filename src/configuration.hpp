#pragma once

#include "configuration.pb.h"
#include "log.hpp"

#include <filesystem>
#include <optional>

namespace acppsrv {

class configuration {
public:
    explicit configuration(const std::filesystem::path& cfg_file);
    [[nodiscard]] bool valid() const noexcept {
        return _valid;
    }
    explicit operator bool() const noexcept {
        return valid();
    }
    const proto::Configuration& data() const noexcept {
        return _cfg;
    }
    std::optional<log_level> get_log_level() const noexcept {
        return _log_level;
    }
private:
    bool validate(const std::filesystem::path& cfg_file);
    bool _valid = false;
    proto::Configuration _cfg{};
    std::optional<log_level> _log_level{};
};

} // namespace acppsrv
