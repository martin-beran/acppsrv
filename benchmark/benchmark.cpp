#include "log.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

using namespace acppsrv;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace {

int final_exception(std::string_view prefix, std::string_view msg)
{
    try {
        log_msg(log_level::emerg) << prefix << msg;
    } catch (std::exception& e) {
    } catch (...) {
    }
    std::cout << prefix << msg << std::endl;
    return EXIT_FAILURE;
}

int usage(const std::filesystem::path& argv0, bool ok = false)
{
    if (!ok)
        log_msg(log_level::crit) << "Invalid command line arguments";
    std::cout << "usage: " << argv0.filename().native() <<
        " COMMAND [ARGS]" << std::endl;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

namespace cmd {

int help(std::string_view argv0, [[maybe_unused]] std::string_view cmd,
         [[maybe_unused]] std::span<const std::string_view> args)
{
    return usage(argv0, true);
}

} // namespace cmd

int run(std::span<const std::string_view> argv)
{
    auto argv0 = argv[0];
    if (argv.size() < 2)
        return usage(argv0);
    auto cmd = argv[1];
    auto cmd_args = argv.subspan(2);
    log_msg(log_level::notice) << "Running command \"" << cmd <<"\"";
    if (argv[1] == "help")
        return cmd::help(argv0, cmd, cmd_args);
    // TODO
    log_msg(log_level::crit) << "Unknown command \"" << cmd << "\"";
    return usage(argv0);
}

} // namespace

/*** main*********************************************************************/

int main(int argc, char* argv[])
{
    try {
        int result = run(std::vector<std::string_view>(argv, argv + argc));
        if (result == EXIT_SUCCESS)
            log_msg(log_level::notice) << "Terminating successfully";
        else
            log_msg(log_level::crit) << "Terminating after error";
        return result;
    } catch (std::exception& e) {
        return final_exception("Terminated by unhandled exception: ", e.what());
    } catch (...) {
        return final_exception("Terminated by unhandled unknown exception", "");
    }
}
