#pragma once

#include "http_server.hpp"

namespace acppsrv::http_hnd {

class echo: public http_handler {
public:
    bool async() override {
        return false;
    }
    http_response_type handle_sync(const http_request_type& request,
                                   uint64_t sid, uint64_t req_n) override;
};

} // namespace acppsrv::http_hnd
