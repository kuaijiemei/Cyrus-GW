#include "api/chat_handler.h"

#include "common/errors.h"
#include "common/json_util.h"
#include "upstream/agent_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace cyrus::api {
namespace {

struct ParsedChatRequest {
    std::string request_id;
    std::string message;
    std::string session_id;
    bool stream{false};
};

void trim_inplace(std::string& s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string make_request_id() {
    using clock = std::chrono::steady_clock;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
    std::random_device rd;
    std::mt19937_64 gen(rd());
    const std::uint64_t r = (static_cast<std::uint64_t>(gen()) << 32) ^ static_cast<std::uint64_t>(gen()) ^
                            static_cast<std::uint64_t>(ns);
    char buf[40];
    const auto n = std::snprintf(buf, sizeof(buf), "req_%016llx", static_cast<unsigned long long>(r));
    if (n <= 0) {
        return "req_unknown";
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

std::optional<std::string> header_value(const std::string& headers, std::string_view name_lower) {
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto key = line.substr(0, colon);
        trim_inplace(key);
        if (to_lower(key) != name_lower) {
            continue;
        }
        auto val = line.substr(colon + 1);
        trim_inplace(val);
        return val;
    }
    return std::nullopt;
}

std::optional<std::string> extract_json_string(const std::string& json, std::string_view key) {
    const std::string quoted_key = std::string("\"") + std::string(key) + "\"";
    std::size_t pos = 0;
    while ((pos = json.find(quoted_key, pos)) != std::string::npos) {
        if (pos > 0) {
            const unsigned char prev = static_cast<unsigned char>(json[pos - 1]);
            if (std::isalnum(prev) != 0 || prev == '_') {
                ++pos;
                continue;
            }
        }
        std::size_t i = pos + quoted_key.size();
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            ++i;
        }
        if (i >= json.size() || json[i] != ':') {
            return std::nullopt;
        }
        ++i;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            ++i;
        }
        if (i >= json.size() || json[i] != '"') {
            return std::nullopt;
        }
        ++i;
        std::string out;
        while (i < json.size()) {
            const char c = json[i++];
            if (c == '"') {
                return out;
            }
            if (c == '\\' && i < json.size()) {
                const char e = json[i++];
                if (e == '"' || e == '\\' || e == '/') {
                    out += e;
                } else if (e == 'n') {
                    out += '\n';
                } else if (e == 'r') {
                    out += '\r';
                } else if (e == 't') {
                    out += '\t';
                } else if (e == 'b') {
                    out += '\b';
                } else if (e == 'f') {
                    out += '\f';
                } else if (e == 'u' && i + 4 <= json.size()) {
                    const std::string hex = json.substr(i, 4);
                    i += 4;
                    unsigned int cp = 0;
                    if (std::sscanf(hex.c_str(), "%4x", &cp) == 1 && cp <= 0x7f) {
                        out += static_cast<char>(cp);
                    }
                } else {
                    out += e;
                }
            } else {
                out += c;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool extract_json_bool(const std::string& json, std::string_view key, bool default_value) {
    const std::string quoted_key = std::string("\"") + std::string(key) + "\"";
    std::size_t pos = 0;
    while ((pos = json.find(quoted_key, pos)) != std::string::npos) {
        if (pos > 0) {
            const unsigned char prev = static_cast<unsigned char>(json[pos - 1]);
            if (std::isalnum(prev) != 0 || prev == '_') {
                ++pos;
                continue;
            }
        }
        std::size_t i = pos + quoted_key.size();
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            ++i;
        }
        if (i >= json.size() || json[i] != ':') {
            return default_value;
        }
        ++i;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            ++i;
        }
        if (i + 4 <= json.size() && json.compare(i, 4, "true") == 0) {
            return true;
        }
        if (i + 5 <= json.size() && json.compare(i, 5, "false") == 0) {
            return false;
        }
        return default_value;
    }
    return default_value;
}

bool content_type_allows_json(const std::string& headers) {
    const auto ct = header_value(headers, "content-type");
    if (!ct) {
        return true;
    }
    return ct->find("application/json") != std::string::npos;
}

std::string error_body(std::string_view error, const std::string& request_id, ErrorCode code,
           std::string_view error_layer, std::string_view detail = "", int upstream_status = 0) {
    std::ostringstream oss;
    oss << "{\"error\":\"" << json_escape(error) << "\",\"error_code\":" << static_cast<int>(code)
        << ",\"error_layer\":\"" << json_escape(error_layer) << "\",\"request_id\":\""
        << json_escape(request_id) << "\"";
    if (!detail.empty()) {
        oss << ",\"detail\":\"" << json_escape(detail) << "\"";
    }
    if (upstream_status > 0) {
        oss << ",\"upstream_status\":" << upstream_status;
    }
    oss << "}";
    return oss.str();
}

std::optional<ParsedChatRequest> parse_chat_request(const std::string& request_headers, const std::string& body,
                                                    ChatHttpResponse& out) {
    std::string request_id = make_request_id();
    if (const auto h = header_value(request_headers, "x-request-id"); h && !h->empty()) {
        request_id = *h;
    }

    if (!content_type_allows_json(request_headers)) {
        out.status_code = 400;
        out.request_id = request_id;
        out.json_body =
            error_body("content_type_must_be_application_json", request_id, ErrorCode::kGwBadRequest, "gateway");
        return std::nullopt;
    }

    std::string trimmed = body;
    trim_inplace(trimmed);
    if (trimmed.empty()) {
        out.status_code = 400;
        out.request_id = request_id;
        out.json_body = error_body("empty_body", request_id, ErrorCode::kGwBadRequest, "gateway");
        return std::nullopt;
    }

    if (trimmed.front() != '{' || trimmed.back() != '}') {
        out.status_code = 400;
        out.request_id = request_id;
        out.json_body = error_body("invalid_json", request_id, ErrorCode::kGwBadRequest, "gateway");
        return std::nullopt;
    }

    if (const auto rid = extract_json_string(trimmed, "request_id"); rid && !rid->empty()) {
        request_id = *rid;
    }

    const auto message = extract_json_string(trimmed, "message");
    if (!message || message->empty()) {
        out.status_code = 400;
        out.request_id = request_id;
        out.json_body = error_body("message_required", request_id, ErrorCode::kGwBadRequest, "gateway");
        return std::nullopt;
    }

    ParsedChatRequest req;
    req.request_id = request_id;
    req.message = *message;
    req.stream = extract_json_bool(trimmed, "stream", false);
    if (const auto session = extract_json_string(trimmed, "session_id"); session && !session->empty()) {
        req.session_id = *session;
    }
    return req;
}

std::string build_agent_payload(const ParsedChatRequest& req) {
    std::ostringstream oss;
    oss << "{\"request_id\":\"" << json_escape(req.request_id) << "\",\"message\":\""
        << json_escape(req.message) << "\",\"stream\":" << (req.stream ? "true" : "false");
    if (!req.session_id.empty()) {
        oss << ",\"session_id\":\"" << json_escape(req.session_id) << "\"";
    }
    oss << "}";
    return oss.str();
}

}  // namespace

ChatHttpResponse handle_post_chat(const GatewayConfigSnapshot& cfg, const std::string& request_headers,
                                  const std::string& body) {
    ChatHttpResponse out;

    const auto parsed = parse_chat_request(request_headers, body, out);
    if (!parsed) {
        return out;
    }

    out.request_id = parsed->request_id;
    out.stream = parsed->stream;

    const std::string agent_payload = build_agent_payload(*parsed);
    const auto upstream = upstream::post_agent_chat(cfg, agent_payload);

    if (!upstream.transport_ok) {
        if (upstream.timed_out) {
            out.status_code = 504;
            out.json_body = error_body("agent_timeout", parsed->request_id, ErrorCode::kGwUpstreamTimeout, "agent",
                   upstream.error);
        } else {
            out.status_code = 502;
            out.json_body = error_body("agent_unavailable", parsed->request_id, ErrorCode::kGwUpstreamUnavailable,
                   "agent", upstream.error);
        }
        return out;
    }

    if (upstream.http_status == 200) {
        out.status_code = 200;
        out.json_body = upstream.http_body;
        if (const auto tool = extract_json_string(upstream.http_body, "tool_used"); tool) {
            out.tool_used = *tool;
        }
        return out;
    }

    if (upstream.http_status == 504) {
        out.status_code = 504;
        out.json_body = error_body("agent_timeout", parsed->request_id, ErrorCode::kGwUpstreamTimeout, "agent",
                 "agent returned timeout", upstream.http_status);
        return out;
    }

    out.status_code = 502;
    out.json_body = error_body("agent_upstream_error", parsed->request_id, ErrorCode::kGwUpstreamUnavailable,
               "agent", "agent returned non-200", upstream.http_status);
    return out;
}

}  // namespace cyrus::api
