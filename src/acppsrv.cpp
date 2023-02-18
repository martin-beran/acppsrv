#include "log.hpp"
#include "application.hpp"
#include "configuration.hpp"

#include <boost/asio.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>

using namespace acppsrv;

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

void usage(const std::filesystem::path& argv0)
{
    log_msg(log_level::crit) << "Invalid command line arguments";
    std::cout << "usage: " << argv0.filename().native() <<
        " cfg_file.json" << std::endl;
}

} // namespace

/*** main*********************************************************************/

int main(int argc, char* argv[])
{
    int result = EXIT_FAILURE;
    try {
        {
            // Process command line
            log_msg(log_level::notice) << "Starting " << application::name <<
                " version " << application::version;
            if (argc != 2) {
                usage(argv[0]);
                goto finish;
            }
            std::filesystem::path cfg_file{argv[1]};
            log_msg(log_level::notice) << "Configuration file " << cfg_file;
            // Read configuration
            configuration cfg{cfg_file};
            if (!cfg)
                goto finish;
            // Set log level
            if (auto ll = cfg.get_log_level()) {
                logger::global().level(*ll);
                log_msg(log_level::notice) << "Log level set to " <<
                    to_string(*ll);
            }
            // Start operation
            application app(cfg);
            if (app.run())
                result = EXIT_SUCCESS;
        }
        // Report unhandled exceptions and finish
finish:
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
