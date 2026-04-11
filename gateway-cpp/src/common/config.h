#pragma once

#include "common/models.h"

#include <filesystem>
#include <optional>
#include <string>

namespace cyrus {

std::optional<GatewayConfigSnapshot> load_gateway_config(const std::filesystem::path& path);

// 解析失败返回 nullopt；调用方可回退默认端口等。
std::filesystem::path resolve_default_config_path(int argc, char** argv);

}  // namespace cyrus
