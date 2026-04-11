#pragma once

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <string_view>

namespace cyrus {

// 最小 JSON 字符串转义（日志与响应体共用）。
inline std::string json_escape(std::string_view in) {
    std::string o;
    o.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':
                o += "\\\"";
                break;
            case '\\':
                o += "\\\\";
                break;
            case '\b':
                o += "\\b";
                break;
            case '\f':
                o += "\\f";
                break;
            case '\n':
                o += "\\n";
                break;
            case '\r':
                o += "\\r";
                break;
            case '\t':
                o += "\\t";
                break;
            default:
                if (c < 0x20) {
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                    o += oss.str();
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

}  // namespace cyrus
