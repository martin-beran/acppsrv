#include "http_hnd_stat.hpp"
#include "http_hnd_stat.pb.h"
#include "http_server_impl.hpp"

namespace acppsrv::http_hnd {

http_handler::http_response_type
stat::handle_sync(const http_request_type& request,
                  uint64_t sid, uint64_t req_n)
{
    auto [in, response] = parse<proto::stat::Request>(request, sid, req_n);
    if (!in)
        return response;
    proto::stat::Response out;
    if (in->reset()) {
        out.set_connections(data.connections.exchange(0));
        out.set_requests(data.requests.exchange(0));
        out.set_data_req(data.data_req.exchange(0));
        out.set_data_resp(data.data_resp.exchange(0));
        log_msg(log_level::notice, {sid, req_n}) << "Reset statistics";
    } else {
        out.set_connections(data.connections);
        out.set_requests(data.requests);
        out.set_data_req(data.data_req);
        out.set_data_resp(data.data_resp);
    }
    serialize(request, sid, req_n, out, response);
    return response;
}

} // namespace acppsrv::http_hnd
