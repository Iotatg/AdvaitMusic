#pragma once

#include "advait/telegram/tdlib.hpp"

namespace advait::modules {

void init_vc_control(tg::TdClient& bot);

void handle_position(const tg::Message& m, bool cplay);
void handle_replay(const tg::Message& m, bool cplay);
void handle_loop(const tg::Message& m, bool cplay);
void handle_speed(const tg::Message& m, bool cplay);
void handle_shuffle(const tg::Message& m, bool cplay);
void handle_seek(const tg::Message& m, bool cplay, bool is_back);
void handle_jump(const tg::Message& m, bool cplay);
void handle_remove(const tg::Message& m, bool cplay);
void handle_clear(const tg::Message& m, bool cplay);
void handle_move(const tg::Message& m, bool cplay);
void handle_mute(const tg::Message& m, bool cplay);
void handle_unmute(const tg::Message& m, bool cplay);
void handle_autoplay_skip(const tg::Message& m, bool cplay);
void handle_active(const tg::Message& m);

}  // namespace advait::modules
