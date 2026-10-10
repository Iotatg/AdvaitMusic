#include "advait/core/buttons.hpp"
#include "advait/core/config.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"

#include <random>

namespace advait {

namespace {
std::string g_bot_username;

nlohmann::json random_style() {
    static const char* styles[] = {
        "buttonStyleDanger", "buttonStylePrimary", "buttonStyleSuccess"};
    if (Config::get().disable_colour) return {};
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int> dist(0, 2);
    return {{"@type", styles[dist(rng)]}};
}

nlohmann::json btn_url(const std::string& text, const std::string& url) {
    return {{"@type", "inlineKeyboardButton"},
            {"icon_custom_emoji_id", 0},
            {"style", random_style()},
            {"text", text},
            {"type", {{"@type", "inlineKeyboardButtonTypeUrl"}, {"url", url}}}};
}
nlohmann::json btn_cb(const std::string& text, const std::string& cb) {
    return {{"@type", "inlineKeyboardButton"},
            {"icon_custom_emoji_id", 0},
            {"style", random_style()},
            {"text", text},
            {"type", {{"@type", "inlineKeyboardButtonTypeCallback"},
                      {"data", utils::base64_encode(cb)}}}};
}
}  // namespace

void set_bot_username(const std::string& u) { g_bot_username = u; }
std::string bot_username() { return g_bot_username; }

nlohmann::json url_btn(const std::string& text, const std::string& url) { return btn_url(text, url); }
nlohmann::json data_btn(const std::string& text, const std::string& cb) { return btn_cb(text, cb); }

nlohmann::json markup_rows(const std::vector<std::vector<nlohmann::json>>& rows) {
    nlohmann::json mk = {{"@type", "replyMarkupInlineKeyboard"}, {"rows", nlohmann::json::array()}};
    for (auto& row : rows) {
        nlohmann::json r = nlohmann::json::array();
        for (auto& b : row) r.push_back(b);
        mk["rows"].push_back(r);
    }
    return mk;
}

nlohmann::json add_me_markup(int64_t chat_id) {
    return markup_rows({{url_btn(F(chat_id, "ADD_ME_BTN"),
                                 "https://t.me/" + bot_username() + "?startgroup&admin=invite_users")}});
}

nlohmann::json cancel_keyboard(int64_t chat_id) {
    return markup_rows({{data_btn(F(chat_id, "DOWNLOAD_CANCEL_BTN"), "cancel")}});
}

nlohmann::json broadcast_cancel_keyboard(int64_t chat_id) {
    return markup_rows({{data_btn(F(chat_id, "BROADCAST_CANCEL_BTN"), "bcast_cancel")}});
}

nlohmann::json supp_markup(int64_t chat_id) {
    return markup_rows({{url_btn(F(chat_id, "SUPPORT_BTN"), Config::get().support_chat)}});
}

nlohmann::json stop_confirm_markup(int64_t chat_id, RoomState* r, bool is_paused) {
    std::string prefix = "room:" + std::to_string(r->id) + ":";
    std::string text = is_paused ? "CONFIRM_RESUME_BTN" : "CONFIRM_UNMUTE_BTN";
    std::string cb = is_paused ? prefix + "resume" : prefix + "unmute";
    return markup_rows({{data_btn(F(chat_id, text), cb), data_btn(F(chat_id, "CONFIRM_STOP_BTN"), prefix + "stop")}});
}

nlohmann::json play_markup(int64_t chat_id, RoomState* r, bool queued) {
    std::string prefix = "room:" + std::to_string(r->id) + ":";
    int duration = 0;
    if (auto t = r->track()) duration = t->duration;
    auto progress = utils::format_time(r->position()) + " " + utils::get_progress_bar(r->position(), duration) + " " +
                    utils::format_time(duration);
    std::vector<std::vector<nlohmann::json>> rows;
    if (!queued) rows.push_back({data_btn(progress, "progress")});
    rows.push_back({data_btn("▷", prefix + "resume"), data_btn("II", prefix + "pause"),
                    data_btn("⟳", prefix + "replay"), data_btn("‣‣I", prefix + "skip"),
                    data_btn("▢", prefix + "stop")});
    rows.push_back({data_btn("-𝟣𝟧ˢ", prefix + "seekback_15"), data_btn("𝟣𝟧ˢ+", prefix + "seek_15")});
    std::string ap = r->autoplay() ? "enabled" : "disabled";
    rows.push_back({data_btn(F(chat_id, "autoplay_btn", {{"state", F(chat_id, ap)}}), prefix + "autoplay_toggle")});
    rows.push_back({data_btn(F(chat_id, "CLOSE_BTN"), "close")});
    return markup_rows(rows);
}

nlohmann::json group_help_keyboard(int64_t chat_id) {
    return markup_rows(
        {{url_btn(F(chat_id, "GC_HELP_BTN"), "https://t.me/" + bot_username() + "?start=pm_help")}});
}

nlohmann::json start_markup(int64_t chat_id) {
    return markup_rows({
        {url_btn(F(chat_id, "ADD_ME_BTN"),
                 "https://t.me/" + bot_username() + "?startgroup&admin=invite_users")},
        {data_btn(F(chat_id, "START_HELP_BTN"), "help_cb")},
        {url_btn(F(chat_id, "UPDATES_BTN"), Config::get().support_channel),
         url_btn(F(chat_id, "SUPPORT_BTN"), Config::get().support_chat)},
        {url_btn(F(chat_id, "SOURCE_BTN"), "https://github.com/Iotatg/AdvaitMusic")},
    });
}

nlohmann::json repo_markup(int64_t chat_id, const std::string& dev_url) {
    std::string d = dev_url;
    auto& c = Config::get();
    if (d.empty()) {
        if (!c.dev_url.empty())
            d = c.dev_url;
        else if (!c.owner_username.empty()) {
            d = "https://t.me/" + c.owner_username;
            if (!d.empty() && d.find('@') != std::string::npos) {
                auto at = d.find('@');
                d = "https://t.me/" + c.owner_username.substr(c.owner_username[0] == '@' ? 1 : 0);
            }
        } else if (c.owner_id != 0)
            d = "tg://openmessage?user_id=" + std::to_string(c.owner_id);
        else
            d = "https://t.me/+Q493A_VBAUpkMmRl";
    }
    return markup_rows({{url_btn(F(chat_id, "REPO_DEV_BTN"), d),
                         url_btn(F(chat_id, "REPO_BTN"), "https://github.com/Iotatg/AdvaitMusic")}});
}

nlohmann::json help_keyboard(int64_t chat_id) {
    return markup_rows({
        {data_btn(F(chat_id, "HELP_ADMIN_BTN"), "help:admin"), data_btn(F(chat_id, "HELP_AUTH_BTN"), "help:auth"),
         data_btn(F(chat_id, "HELP_BCAST_BTN"), "help:bcast")},
        {data_btn(F(chat_id, "HELP_PLAY_BTN"), "help:play"), data_btn(F(chat_id, "HELP_SUDO_BTN"), "help:sudo"),
         data_btn(F(chat_id, "HELP_RESTRICT_BTN"), "help:restrict")},
        {data_btn(F(chat_id, "HELP_THUMB_BTN"), "help:thumb"), data_btn(F(chat_id, "HELP_START_BTN"), "help:start"),
         data_btn(F(chat_id, "HELP_AUTOPLAY_BTN"), "help:autoplay")},
        {data_btn(F(chat_id, "HELP_PLAYLIST_BTN"), "help:playlist"),
         data_btn(F(chat_id, "HELP_VCLOGS_BTN"), "help:vclogs"),
         data_btn(F(chat_id, "HELP_INLINE_BTN"), "help:inline")},
        {data_btn(F(chat_id, "BACK_BTN"), "start")},
    });
}

nlohmann::json back_keyboard(int64_t chat_id) {
    return markup_rows({{data_btn(F(chat_id, "BACK_BTN"), "help:main")}});
}

nlohmann::json restart_confirm_markup(int64_t chat_id) {
    return markup_rows({{data_btn(F(chat_id, "restart_btn_bot"), "restart:bot"),
                         data_btn(F(chat_id, "restart_btn_replay"), "restart:replay")}});
}

}  // namespace advait
