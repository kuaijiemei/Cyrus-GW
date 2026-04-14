#include "common/config.h"

#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>

#if defined(__linux__)
#include <linux/limits.h>
#include <unistd.h>
#endif

namespace cyrus {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

std::optional<std::string> strip_quotes(std::string s) {
    s = trim(std::move(s));
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

// 极简 key: value 解析，满足 MVP gateway.yaml；嵌套用 "section.key" 展平。
bool load_flat_key_values(const std::filesystem::path& path,
                                                    std::unordered_map<std::string, std::string>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (!line.empty() && line.back() == ':') {
            section = line.substr(0, line.size() - 1);
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto key = trim(line.substr(0, colon));
        auto val = strip_quotes(trim(line.substr(colon + 1))).value_or("");
        if (!section.empty()) {
            key = section + "." + key;
        }
        out[key] = val;
    }
    return true;
}

std::uint16_t parse_u16(const std::string& s, std::uint16_t fallback) {
    try {
        const auto v = std::stoul(s);
        if (v > 65535) {
            return fallback;
        }
        return static_cast<std::uint16_t>(v);
    } catch (...) {
        return fallback;
    }
}

std::int32_t parse_i32(const std::string& s, std::int32_t fallback) {
    try {
        return static_cast<std::int32_t>(std::stol(s));
    } catch (...) {
        return fallback;
    }
}

}  // namespace

std::filesystem::path resolve_default_config_path(int argc, char** argv) {
    if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
        return std::filesystem::path(argv[1]);
    }
    const std::filesystem::path cwd_cfg("configs/gateway.yaml");
    if (std::filesystem::exists(cwd_cfg)) {
        return cwd_cfg;
    }
#if defined(__linux__)
    char self[PATH_MAX + 1];
    const ssize_t n = ::readlink("/proc/self/exe", self, PATH_MAX);
    if (n > 0) {
        self[n] = '\0';
        const std::filesystem::path exe(self);
        const std::filesystem::path from_build = exe.parent_path() / ".." / "configs" / "gateway.yaml";
        if (std::filesystem::exists(from_build)) {
            return std::filesystem::weakly_canonical(from_build);
        }
    }
#endif
    return cwd_cfg;
}

std::optional<GatewayConfigSnapshot> load_gateway_config(const std::filesystem::path& path) {
    std::unordered_map<std::string, std::string> kv;
    if (!load_flat_key_values(path, kv)) {
        return std::nullopt;
    }
    GatewayConfigSnapshot c;
    if (auto it = kv.find("listen_host"); it != kv.end()) {
        c.listen_host = it->second;
    } else {
        c.listen_host = "0.0.0.0";
    }
    if (auto it = kv.find("listen_port"); it != kv.end()) {
        c.listen_port = parse_u16(it->second, 8080);
    }
    if (auto it = kv.find("agent_base_url"); it != kv.end()) {
        c.agent_base_url = it->second;
    } else {
        c.agent_base_url = "http://127.0.0.1:8001";
    }
    if (auto it = kv.find("agent_timeout_ms"); it != kv.end()) {
        c.agent_timeout_ms = parse_i32(it->second, 8000);
    }
    if (auto it = kv.find("agent_retry_max"); it != kv.end()) {
        c.agent_retry_max = parse_i32(it->second, 1);
    }
    if (auto it = kv.find("rate_limit.capacity"); it != kv.end()) {
        c.rate_limit_capacity = parse_i32(it->second, 500);
    }
    if (auto it = kv.find("rate_limit.refill_per_sec"); it != kv.end()) {
        c.rate_limit_refill_per_sec = parse_i32(it->second, 200);
    }
    if (auto it = kv.find("sse.first_chunk_timeout_ms"); it != kv.end()) {
        c.sse_first_chunk_timeout_ms = parse_i32(it->second, 5000);
    }
    if (auto it = kv.find("sse.total_timeout_ms"); it != kv.end()) {
        c.sse_total_timeout_ms = parse_i32(it->second, 120000);
    }
    return c;
}

}  // namespace cyrus
