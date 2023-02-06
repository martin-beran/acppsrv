#include "log.hpp"

#include <boost/asio.hpp>

#include <cstdlib>
#include <iostream>

int main(int /*argc*/, char* /*argv*/[])
{
    acppsrv::log(acppsrv::log::level::notice) << " Started";
    for (int i = 0; i < 1000000; ++i)
        acppsrv::DEBUG() << " i=" << i;

    return EXIT_SUCCESS;
}
