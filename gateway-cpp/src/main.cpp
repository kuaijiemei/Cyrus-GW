// ---------------------------------------------------------------------------
// 模块职责：Gateway 入口——加载配置、解析 --mode 选择并发模型、启动对应服务。
// 对外暴露：main()
//
// 用法：
//   ./cyrus-gateway                      # 默认 blocking 模式
//   ./cyrus-gateway --mode=iouring       # 主实现：io_uring + C++20 协程
//   ./cyrus-gateway --mode=epoll         # 对照实现：epoll + 线程池
//   ./cyrus-gateway --mode=blocking      # Week1-3 使用的单线程阻塞模式
//   ./cyrus-gateway [config_path]        # 显式指定配置文件
//   ./cyrus-gateway --mode=iouring configs/gateway.yaml
//
// 关键分支原因：
//   三种模式共享 config / limiter / chat_handler / upstream_client / logger，
//   仅网络 I/O 层不同，便于同环境对比压测并讲解 Reactor vs Proactor 差异。
// ---------------------------------------------------------------------------
#include "common/config.h"
#include "common/logger.h"
#include "common/models.h"
#include "net/epoll_server.h"
#include "net/http_server.h"
#include "net/iouring_server.h"

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct CliArgs {
    std::string mode{"blocking"};
    std::string config_path;
};

CliArgs parse_cli(int argc, char** argv) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg.substr(0, 7) == "--mode=") {
            args.mode = std::string(arg.substr(7));
        } else if (arg == "--mode" && i + 1 < argc) {
            args.mode = argv[++i];
        } else if (arg[0] != '-') {
            args.config_path = std::string(arg);
        }
    }
    return args;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cli = parse_cli(argc, argv);

    // 配置加载：未指定路径时用 resolve（跳过 argv 以免把 --mode 当路径）
    cyrus::GatewayConfigSnapshot snapshot;
    if (!cli.config_path.empty()) {
        if (const auto loaded = cyrus::load_gateway_config(cli.config_path)) {
            snapshot = *loaded;
        } else {
            std::cerr << "failed to load config: " << cli.config_path << " (using defaults)\n";
        }
    } else {
        // 传 0 跳过 argv[1]，让 resolve 只走 cwd / exe 相邻路径
        const auto cfg_path = cyrus::resolve_default_config_path(0, nullptr);
        if (const auto loaded = cyrus::load_gateway_config(cfg_path)) {
            snapshot = *loaded;
        } else {
            std::cerr << "failed to load config: " << cfg_path.string() << " (using defaults)\n";
        }
    }

    cyrus::log_startup("config_loaded");
    std::cerr << "{\"event\":\"mode\",\"mode\":\"" << cli.mode << "\"}\n";

    if (cli.mode == "iouring") {
        cyrus::net::IoUringServer server(snapshot);
        server.run();
    } else if (cli.mode == "epoll") {
        cyrus::net::EpollServer server(snapshot);
        server.run();
    } else {
        // blocking — 兼容 Week 1-3 单线程模式
        cyrus::net::HttpServer server(snapshot);
        server.run();
    }
    return 0;
}
