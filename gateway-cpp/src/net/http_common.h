// ---------------------------------------------------------------------------
// 模块职责：所有 HTTP 服务实现（blocking / io_uring / epoll）共用的工具函数。
// 对外暴露：HTTP 解析（method/path/header）、请求 ID 生成、错误体格式化、
//           HTTP 响应行/头拼装。
// 不含 I/O 操作——读写由各服务实现自行处理，保持 I/O 模型解耦。
// ---------------------------------------------------------------------------
#pragma once

#include "common/errors.h"
#include "common/json_util.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace cyrus::net::http {

inline void trim_inplace(std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
}

inline std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::optional<std::string> header_value(const std::string& headers,
                                               std::string_view name_lower) {
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = line.substr(0, colon);
        trim_inplace(key);
        if (to_lower(key) != name_lower) continue;
        auto val = line.substr(colon + 1);
        trim_inplace(val);
        return val;
    }
    return std::nullopt;
}

inline bool parse_method_path(const std::string& headers,
                              std::string& method, std::string& path) {
    const auto end = headers.find("\r\n");
    if (end == std::string::npos) return false;
    std::istringstream first(headers.substr(0, end));
    if (!(first >> method >> path)) return false;
    const auto q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    if (path.size() > 1 && path.back() == '/') path.pop_back();
    for (auto& c : method) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return true;
}

inline bool is_health_get(const std::string& m, const std::string& p) {
    return m == "GET" && p == "/health";
}

inline bool is_chat_post(const std::string& m, const std::string& p) {
    return m == "POST" && p == "/chat";
}

inline std::string make_request_id() {
    using clock = std::chrono::steady_clock;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        clock::now().time_since_epoch())
                        .count();
    std::random_device rd;
    std::mt19937_64 gen(rd());
    const std::uint64_t r = (static_cast<std::uint64_t>(gen()) << 32) ^
                            static_cast<std::uint64_t>(gen()) ^
                            static_cast<std::uint64_t>(ns);
    char buf[40];
    const auto n = std::snprintf(buf, sizeof(buf), "req_%016llx",
                                 static_cast<unsigned long long>(r));
    if (n <= 0) return "req_unknown";
    return std::string(buf, static_cast<std::size_t>(n));
}

inline std::string extract_request_id_from_body(const std::string& body) {
    const std::string key = "\"request_id\"";
    const std::size_t pos = body.find(key);
    if (pos == std::string::npos) return "";
    std::size_t i = pos + key.size();
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size() || body[i] != ':') return "";
    ++i;
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size() || body[i] != '"') return "";
    ++i;
    std::string out;
    while (i < body.size()) {
        const char c = body[i++];
        if (c == '"') return out;
        out.push_back(c);
    }
    return "";
}

inline std::string request_id_from_headers_or_body(const std::string& headers,
                                                   const std::string& body) {
    if (const auto rid = header_value(headers, "x-request-id"); rid && !rid->empty())
        return *rid;
    const std::string rid = extract_request_id_from_body(body);
    if (!rid.empty()) return rid;
    return make_request_id();
}

inline std::string gateway_error_body(std::string_view error,
                                      std::string_view request_id,
                                      cyrus::ErrorCode code) {
    std::ostringstream oss;
    oss << "{\"error\":\"" << cyrus::json_escape(error)
        << "\",\"error_code\":" << static_cast<int>(code)
        << ",\"error_layer\":\"gateway\",\"request_id\":\""
        << cyrus::json_escape(request_id) << "\"}";
    return oss.str();
}

inline std::string rate_limited_body(std::string_view request_id) {
    std::ostringstream oss;
    oss << "{\"error\":\"rate_limited\",\"error_code\":"
        << static_cast<int>(cyrus::ErrorCode::kGwRateLimited)
        << ",\"error_layer\":\"gateway\",\"request_id\":\""
        << cyrus::json_escape(request_id) << "\"}";
    return oss.str();
}

// ---------- HTTP response formatting (返回完整报文字符串) ----------

inline const char* status_phrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        default:  return "Error";
    }
}

inline std::string format_json_response(int status_code, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << status_phrase(status_code)
        << "\r\nContent-Type: application/json"
        << "\r\nConnection: close"
        << "\r\nContent-Length: " << body.size()
        << "\r\n\r\n" << body;
    return oss.str();
}

inline std::string format_sse_headers() {
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/event-stream\r\n"
           "Cache-Control: no-cache\r\n"
           "Connection: close\r\n\r\n";
}

inline std::string format_health_body() {
    return R"({"status":"ok","service":"gateway"})";
}

// ---------- 从原始 socket 数据中解析 Content-Length 并读齐 body ----------

inline std::size_t parse_content_length(const std::string& headers) {
    if (const auto cl = header_value(headers, "content-length")) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(cl->c_str(), &end, 10);
        if (end != cl->c_str() && v < 1024ul * 1024ul) {
            return static_cast<std::size_t>(v);
        }
    }
    return 0;
}

}  // namespace cyrus::net::http
