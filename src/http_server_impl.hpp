#pragma once

#include "http_server.hpp"
#include "log.hpp"

#include <google/protobuf/util/json_util.h>
#include <string>

namespace acppsrv {

template <class Request>
std::pair<std::optional<Request>, http_handler::http_response_type>
http_handler::parse(const http_request_type& request,
                    uint64_t sid, uint64_t req_n)
{
    using namespace std::string_literals;
    namespace gpb = google::protobuf;
    auto response = http_handler::handle_sync(request, sid, req_n);
    Request parsed;
    if (json_body(request, true).first) {
        gpb::util::JsonParseOptions opts{};
        opts.ignore_unknown_fields = false;
        opts.case_insensitive_enum_parsing = false;
        if (auto status = gpb::util::JsonStringToMessage(request.body(),
                                                         &parsed, opts);
            !status.ok())
        {
            std::string msg = "Cannot parse JSON request: ("s +
                std::to_string(status.error_code()) + ") " +
                status.ToString();
            log_msg(log_level::err, {sid, req_n}) << msg;
            return {
                std::nullopt,
                error_response(sid, req_n,
                               boost::beast::http::status::bad_request, msg)
            };
        }
    } else if (protobuf_body(request, true).first) {
        if (!parsed.ParseFromString(request.body())) {
            std::string msg = "Cannot parse protobuf request";
            log_msg(log_level::err, {sid, req_n}) << msg;
            return {
                std::nullopt,
                error_response(sid, req_n,
                               boost::beast::http::status::bad_request, msg)
            };
        }
    } else
        return {
            std::nullopt,
            error_response(sid, req_n,
                           boost::beast::http::status::unsupported_media_type,
                           "Unsupported Content-Type")
        };
    return {std::move(parsed), std::move(response)};
}

template <class Response>
bool http_handler::serialize(const http_request_type& request, uint64_t sid,
                             uint64_t req_n, const Response& data,
                             http_response_type& response)
{
    using namespace std::string_literals;
    namespace gpb = google::protobuf;
    Response serialized;
    if (auto [json, ct] = json_body(request, false); json) {
        gpb::util::JsonPrintOptions opts{};
        opts.always_print_primitive_fields = true;
        if (auto status = gpb::util::MessageToJsonString(data,
                                                         &response.body(),
                                                         opts);
            !status.ok())
        {
            std::string msg = "Cannot serialize response to JSON: ("s +
                std::to_string(status.error_code()) + ") " +
                status.ToString();
            log_msg(log_level::err, {sid, req_n}) << msg;
            error_response(sid, req_n,
                           boost::beast::http::status::internal_server_error,
                           msg);
            return false;
        }
    } else if (auto [protobuf, ct] = protobuf_body(request, false); protobuf) {
        if (!data.SerializeToString(&response.body())) {
            std::string msg = "Cannot serialze response to protobuf";
            log_msg(log_level::err, {sid, req_n}) << msg;
            error_response(sid, req_n,
                           boost::beast::http::status::internal_server_error,
                           msg);
            return false;
        }
    } else {
        response =
            error_response(sid, req_n,
                   boost::beast::http::status::not_acceptable,
                   "Unsupported Content-Type requested by client for response");
        return false;
        };
    return true;
}

} // namespace acppsrv
