#include "advait/core/assistant.hpp"
#include "advait/core/config.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/logger.hpp"
#include "advait/database/database.hpp"
#include "advait/modules/modules.hpp"
#include "advait/telegram/tdlib.hpp"
#include "advait/ubot/context.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

bool refresh_dirs() {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* d : {"cache", "downloads"}) {
        fs::create_directories(d, ec);
        if (ec) return false;
    }
    return true;
}

void start_http(const std::string& port) {
    std::thread([port] {
        int p = 8000;
        try {
            p = std::stoi(port);
        } catch (...) {
        }
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(p));
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return;
        }
        listen(fd, 8);
        ADVAIT_LOG_INFO("http", "HTTP server listening on 0.0.0.0:" + port);
        while (!g_stop) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            timeval tv{1, 0};
            if (select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
            int c = accept(fd, nullptr, nullptr);
            if (c < 0) continue;
            const char resp[] =
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK";
            (void)write(c, resp, sizeof(resp) - 1);
            ::close(c);
        }
        ::close(fd);
    }).detach();
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    auto& cfg = advait::Config::get();
    if (auto err = cfg.load(); !err.empty()) {
        ADVAIT_LOG_FATAL("main", err);
        return 1;
    }
    if (!refresh_dirs()) {
        ADVAIT_LOG_FATAL("main", "failed to refresh directories");
        return 1;
    }
    if (!advait::db::init(cfg.mongo_uri)) {
        ADVAIT_LOG_FATAL("main", "failed to initialize database");
        return 1;
    }
    if (auto err = advait::Locales::instance().load("locales"); !err.empty()) {
        auto fallback = advait::Locales::instance().load(ADVAIT_LOCALES_DIR);
        if (!fallback.empty()) {
            ADVAIT_LOG_FATAL("main", err);
            return 1;
        }
    }

    advait::AssistantManager assistants;
    advait::g_assistants = &assistants;
    for (size_t i = 0; i < cfg.string_sessions.size(); ++i) {
        auto a = std::make_unique<advait::Assistant>();
        a->index = static_cast<int>(i + 1);
        std::shared_ptr<advait::mtproto::Client> mt;
        try {
            auto ses = advait::mtproto::Client::parse_session_string(cfg.string_sessions[i]);
            a->user_id = ses.user_id;
            mt = std::make_shared<advait::mtproto::Client>(ses, advait::mtproto::ClientConfig{});
            ADVAIT_LOG_INFO("main", "assistant #" + std::to_string(a->index) + " user id " + std::to_string(a->user_id));
        } catch (const std::exception& e) {
            ADVAIT_LOG_ERROR("main", std::string("assistant session parse failed: ") + e.what());
        }
        a->ntg = std::make_unique<advait::ubot::Context>(std::move(mt));
        assistants.add(std::move(a));
    }
    advait::db::init_assistant_indexes(assistants.count());

    auto& bot = advait::tg::bot();
    advait::modules::init(bot, &assistants);

    start_http(cfg.port);

    std::string err;
    if (!bot.start_bot(cfg.api_id, cfg.api_hash, cfg.token, &err)) {
        ADVAIT_LOG_FATAL("main", "tdlib start failed: " + err);
        return 1;
    }
    ADVAIT_LOG_INFO("main", "AdvaitMusic-cpp ready");
    bot.idle();
    advait::db::shutdown();
    return 0;
}
