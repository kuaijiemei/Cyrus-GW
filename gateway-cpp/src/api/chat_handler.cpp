// -----------------------------------------------------------------------------
// POST /chat 网关侧处理：校验与解析请求、组装发往 Agent 的 JSON、映射上游 HTTP/传输错误。
// 在网关尽早返回 400，把「客户端不合法」与「Agent/网络失败」分离，便于排障与监控归因（error_layer）。
// request_id 贯穿错误体与日志；成功路径尽量透传 Agent 响应体，避免网关重复解析业务字段。
// MVP 用手写 JSON 解析与 json_escape 拼包：轻依赖，但须与 Agent Pydantic 字段契约保持一致。
// -----------------------------------------------------------------------------
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

    // 显式非 application/json 时拒绝：避免下游把二进制当 JSON 解析，错误语义固定在网关（gateway）。
    if (!content_type_allows_json(request_headers)) {
        out.status_code = 400;
        out.request_id = request_id;
        out.error_layer = "gateway";
        out.json_body =
            error_body("content_type_must_be_application_json", request_id, ErrorCode::kGwBadRequest, "gateway");
        return std::nullopt;
    }

    std::string trimmed = body;
    trim_inplace(trimmed);
    // 空 body 无解析意义，不调 Agent，直接 400 降低无效负载。
    if (trimmed.empty()) {
        out.status_code = 400;
        out.request_id = request_id;
        out.error_layer = "gateway";
        out.json_body = error_body("empty_body", request_id, ErrorCode::kGwBadRequest, "gateway");
        return std::nullopt;
    }

    // 粗略 JSON 外形检查失败即 400：比传到 Agent 再失败更早、且错误层仍归 gateway。
    if (trimmed.front() != '{' || trimmed.back() != '}') {
        out.status_code = 400;
        out.request_id = request_id;
        out.error_layer = "gateway";
        out.json_body = error_body("invalid_json", request_id, ErrorCode::kGwBadRequest, "gateway");
        return std::nullopt;
    }

    // body 内 request_id 若合法则覆盖 Header 默认值：便于单条链路在多个 hop 间对齐同一主键。
    if (const auto rid = extract_json_string(trimmed, "request_id"); rid && !rid->empty()) {
        request_id = *rid;
    }

    // message 为对外契约必填：在网关拦截，减少 Agent 无效调用与模糊错误。
    const auto message = extract_json_string(trimmed, "message");
    if (!message || message->empty()) {
        out.status_code = 400;
        out.request_id = request_id;
        out.error_layer = "gateway";
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
    // 字段经 json_escape 再拼接：防止用户内容打断 JSON 结构，否则 Agent 解析失败且难以归因。
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
                                  const std::string& body, const StreamCallbacks* stream_callbacks) {
    ChatHttpResponse out;

    const auto parsed = parse_chat_request(request_headers, body, out);
    if (!parsed) {
        return out;
    }

    out.request_id = parsed->request_id;
    out.stream = parsed->stream;

    const std::string agent_payload = build_agent_payload(*parsed);
    if (parsed->stream) {
        // 流式分支：由上游逐 chunk 返回，网关只做透传与超时守卫，不缓存全量内容。
        if (stream_callbacks == nullptr || !stream_callbacks->write_chunk) {
            out.status_code = 500;
            out.error_layer = "gateway";
            out.json_body = error_body("stream_callbacks_missing", parsed->request_id, ErrorCode::kGwInternal,
                                       "gateway");
            return out;
        }
        const auto upstream_stream = upstream::stream_agent_chat(cfg, agent_payload, stream_callbacks->write_chunk);
        if (!upstream_stream.transport_ok) {
            if (upstream_stream.ttft_ms >= 0) {
                std::ostringstream oss;
                oss << "event: error\ndata: {\"request_id\":\"" << json_escape(parsed->request_id)
                    << "\",\"error\":\"agent_timeout\",\"detail\":\"" << json_escape(upstream_stream.error)
                    << "\"}\n\n";
                (void)stream_callbacks->write_chunk(oss.str());
                out.status_code = 200;
                out.ttft_ms = upstream_stream.ttft_ms;
                return out;
            }
            if (upstream_stream.timed_out) {
                out.status_code = 504;
                out.error_layer = "agent";
                out.json_body = error_body("agent_timeout", parsed->request_id, ErrorCode::kGwUpstreamTimeout, "agent",
                                           upstream_stream.error);
            } else {
                out.status_code = 502;
                out.error_layer = "agent";
                out.json_body = error_body("agent_unavailable", parsed->request_id, ErrorCode::kGwUpstreamUnavailable,
                                           "agent", upstream_stream.error);
            }
            return out;
        }
        if (upstream_stream.http_status != 200) {
            out.status_code = (upstream_stream.http_status == 504) ? 504 : 502;
            out.error_layer = "agent";
            out.json_body = error_body(
                (upstream_stream.http_status == 504) ? "agent_timeout" : "agent_upstream_error", parsed->request_id,
                (upstream_stream.http_status == 504) ? ErrorCode::kGwUpstreamTimeout : ErrorCode::kGwUpstreamUnavailable,
                "agent", "agent stream returned non-200", upstream_stream.http_status);
            return out;
        }
        out.status_code = 200;
        out.ttft_ms = upstream_stream.ttft_ms;
        if (stream_callbacks->on_ttft_ms && upstream_stream.ttft_ms >= 0) {
            stream_callbacks->on_ttft_ms(upstream_stream.ttft_ms);
        }
        return out;
    }

    // 关键分支原因：Gateway 仅在「Agent 请求超时」时做有限重试，最多 1 次，避免无限重试放大故障。
    const int retry_limit = std::clamp(cfg.agent_retry_max, 0, 1);
    upstream::AgentClientResult upstream;
    for (int attempt = 0;; ++attempt) {
        upstream = upstream::post_agent_chat(cfg, agent_payload);
        if (!(upstream.timed_out && !upstream.transport_ok) || attempt >= retry_limit) {
            out.retry_count = attempt;
            break;
        }
    }

    // 未拿到合法 HTTP 响应：区分超时（504）与连接/发送/解析失败（502），对应「慢」与「不可用」两类运维动作。
    if (!upstream.transport_ok) {
        if (upstream.timed_out) {
            out.status_code = 504;
            out.error_layer = "agent";
            out.json_body = error_body("agent_timeout", parsed->request_id, ErrorCode::kGwUpstreamTimeout, "agent",
                   upstream.error);
        } else {
            out.status_code = 502;
            out.error_layer = "agent";
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

    // Agent 进程已响应但声明自身超时：保持 504，不把 LLM/Agent 超时伪装成网关 502。
    if (upstream.http_status == 504) {
        out.status_code = 504;
        out.error_layer = "agent";
        out.json_body = error_body("agent_timeout", parsed->request_id, ErrorCode::kGwUpstreamTimeout, "agent",
                 "agent returned timeout", upstream.http_status);
        return out;
    }

    // 其余非 200：MVP 统一 502，由 upstream_status/detail 保留排障信息，避免状态码矩阵爆炸。
    out.status_code = 502;
    out.error_layer = "agent";
    out.json_body = error_body("agent_upstream_error", parsed->request_id, ErrorCode::kGwUpstreamUnavailable,
               "agent", "agent returned non-200", upstream.http_status);
    return out;
}

}  // namespace cyrus::api
