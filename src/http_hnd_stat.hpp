#pragma once

#include "http_server.hpp"

#include <atomic>

namespace acppsrv::http_hnd {

class stat: public http_handler {
public:
    struct data_t {
        std::atomic<uint64_t> connections{0};
        std::atomic<uint64_t> requests{0};
        std::atomic<uint64_t> data_req{0};
        std::atomic<uint64_t> data_resp{0};
    };
    bool async() override {
        return false;
    }
    http_response_type handle_sync(const http_request_type& request,
                                   uint64_t sid, uint64_t req_n) override;
    data_t data;
};

} // namespace acppsrv::http_hnd
