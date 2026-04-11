#include "common/config.h"
#include "common/logger.h"
#include "common/models.h"
#include "net/http_server.h"

#include <iostream>

int main(int argc, char** argv) {
    const auto cfg_path = cyrus::resolve_default_config_path(argc, argv);
    cyrus::GatewayConfigSnapshot snapshot;
    if (const auto loaded = cyrus::load_gateway_config(cfg_path)) {
        snapshot = *loaded;
    } else {
        std::cerr << "failed to load config: " << cfg_path << " (using defaults)\n";
    }
    cyrus::log_startup("config_loaded");
    cyrus::net::HttpServer server(snapshot);
    server.run();
    return 0;
}
