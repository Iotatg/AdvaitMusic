#pragma once

#include "advait/telegram/tdlib.hpp"

namespace advait {
class AssistantManager;
}

namespace advait::modules {

void init(tg::TdClient& bot, AssistantManager* assistants);

void handle_start(const tg::Message& m);
void handle_help(const tg::Message& m);
void handle_ping(const tg::Message& m);
void handle_play(const tg::Message& m, bool force, bool cplay, bool video);
// Cancels the in-flight download for a chat (Go downloads.cancel). True if a
// download was active and got signaled.
bool cancel_download(int64_t chat_id);
void handle_skip(const tg::Message& m, bool cplay);
void handle_pause(const tg::Message& m, bool cplay);
void handle_resume(const tg::Message& m, bool cplay);
void handle_queue(const tg::Message& m, bool cplay);
void handle_stop(const tg::Message& m, bool cplay);
void handle_start_cb(const tg::CallbackQuery& cb);
void handle_help_cb(const tg::CallbackQuery& cb);
void handle_help_page_cb(const tg::CallbackQuery& cb);
void handle_room_cb(const tg::CallbackQuery& cb);

}  // namespace advait::modules
