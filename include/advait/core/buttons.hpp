#pragma once

#include "advait/core/locales.hpp"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace advait {

class RoomState;

nlohmann::json url_btn(const std::string& text, const std::string& url);
nlohmann::json data_btn(const std::string& text, const std::string& cb);
nlohmann::json markup_rows(const std::vector<std::vector<nlohmann::json>>& rows);

nlohmann::json add_me_markup(int64_t chat_id);
nlohmann::json cancel_keyboard(int64_t chat_id);
nlohmann::json broadcast_cancel_keyboard(int64_t chat_id);
nlohmann::json supp_markup(int64_t chat_id);
nlohmann::json stop_confirm_markup(int64_t chat_id, RoomState* r, bool is_paused);
nlohmann::json play_markup(int64_t chat_id, RoomState* r, bool queued);
nlohmann::json group_help_keyboard(int64_t chat_id);
nlohmann::json start_markup(int64_t chat_id);
nlohmann::json repo_markup(int64_t chat_id, const std::string& dev_url);
nlohmann::json help_keyboard(int64_t chat_id);
nlohmann::json back_keyboard(int64_t chat_id);
nlohmann::json restart_confirm_markup(int64_t chat_id);

std::string bot_username();
void set_bot_username(const std::string& u);

}  // namespace advait
