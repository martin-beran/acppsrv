#include "http_hnd_echo.hpp"

namespace acppsrv::http_hnd {

http_handler::http_response_type
echo::handle_sync(const http_request_type& request,
                  uint64_t sid, uint64_t req_n)
{
    auto response = http_handler::handle_sync(request, sid, req_n);
    if (auto ct = request.find(boost::beast::http::field::content_type);
        ct != request.end())
    {
        response.set(boost::beast::http::field::content_type, ct->value());
    } else
        response.set(boost::beast::http::field::content_type,
                     "application/octet-stream");
    response.body() = request.body();
    return response;
}

} // namespace acppsrv::http_hnd
