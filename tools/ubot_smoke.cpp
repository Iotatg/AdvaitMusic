// End-to-end assistant voice chat smoke: parse session, join the active group
// call of a chat and stream a file into it via the real Context code path.
// Usage: ubot_smoke <chat_id> <media_path> [seconds]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "advait/core/config.hpp"
#include "advait/mtproto/client.hpp"
#include "advait/ntgcalls/client.hpp"
#include "advait/ubot/context.hpp"

int main(int argc, char** argv) try {
    using namespace advait;
    if (argc < 3) {
        std::printf("USAGE: ubot_smoke <chat_id> <media_path> [seconds]\n");
        return 1;
    }
    long long chat_id = std::atoll(argv[1]);
    const std::string media = argv[2];
    int seconds = argc > 3 ? std::atoi(argv[3]) : 8;

    auto& cfg = Config::get();
    std::string err = cfg.load();
    if (cfg.string_sessions.empty()) {
        std::printf("NO_SESSIONS %s\n", err.c_str());
        return 2;
    }
    auto ses = mtproto::Client::parse_session_string(cfg.string_sessions[0]);
    auto mt = std::make_shared<mtproto::Client>(ses, mtproto::ClientConfig{});
    ubot::Context ctx(mt);

    auto desc = ntg::media_from_file(media, 0, 1.0, false);
    if (!ctx.play(chat_id, desc, &err)) {
        std::printf("PLAY_FAIL %s\n", err.c_str());
        return 3;
    }
    std::printf("PLAY_OK streaming %s for %ds\n", media.c_str(), seconds);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    ctx.stop(chat_id);
    std::printf("STOPPED\n");
    return 0;
} catch (const std::exception& e) {
    std::printf("EXCEPTION %s\n", e.what());
    return 99;
}
