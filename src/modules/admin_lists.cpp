#include "advait/modules/admin_lists.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/config.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/telegram/tdlib.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace advait::modules {
namespace {

std::string code_id(int64_t id) {
    return "<code>" + std::to_string(id) + "</code>";
}

std::string get_command(const tg::Message& m) {
    auto fields = utils::split_fields(m.text);
    if (fields.empty()) return {};
    auto cmd = fields[0];
    auto at = cmd.find('@');
    if (at != std::string::npos) cmd = cmd.substr(0, at);
    return cmd;
}

std::optional<bool> strict_bool(const std::string& raw) {
    auto v = utils::to_lower(utils::trim(raw));
    if (v == "on" || v == "enable" || v == "enabled" || v == "true" || v == "1" || v == "yes" || v == "y")
        return true;
    if (v == "off" || v == "disable" || v == "disabled" || v == "false" || v == "0" || v == "no" || v == "n")
        return false;
    return std::nullopt;
}

bool parse_int64_arg(const std::string& raw, int64_t* out) {
    auto v = utils::trim(raw);
    if (!v.empty() && v.front() == '@') v.erase(v.begin());
    if (v.empty()) return false;
    try {
        *out = std::stoll(v);
    } catch (...) {
        return false;
    }
    return true;
}

bool arg_present(const tg::Message& m) {
    auto fields = utils::split_fields(m.text);
    return fields.size() >= 2 || m.reply_to != 0;
}

std::string first_arg(const tg::Message& m) {
    auto fields = utils::split_fields(m.text);
    if (fields.size() < 2) return {};
    return utils::trim(fields[1]);
}

bool require_owner(const tg::Message& m) {
    auto& cfg = Config::get();
    if (cfg.owner_id == 0 || m.sender_id != cfg.owner_id) {
        tg::bot().reply_text(m, F(m.chat_id, "only_owner"));
        return false;
    }
    return true;
}

bool require_owner_or_sudo(const tg::Message& m) {
    auto& cfg = Config::get();
    if ((cfg.owner_id != 0 && m.sender_id == cfg.owner_id) || db::is_sudo(m.sender_id)) return true;
    tg::bot().reply_text(m, F(m.chat_id, "only_sudo"));
    return false;
}

bool group_gate(const tg::Message& m) {
    auto& bot = tg::bot();
    if (m.is_private) {
        bot.reply_text(m, F(m.chat_id, "only_supergroup"));
        db::add_served_user(m.chat_id);
        return false;
    }
    if (m.is_channel) return false;
    db::add_served_chat(m.chat_id);
    return true;
}

// No GetMessage in the TDLib client: reply targets cannot be resolved; only
// raw numeric ids work (Go also resolved @username via SearchPublicChat).
bool extract_target_user(const tg::Message& m, int64_t* out, std::string* err) {
    if (m.reply_to != 0) {
        *err = "replied message is not available, pass a numeric user id";
        return false;
    }
    auto fields = utils::split_fields(m.text);
    if (fields.size() < 2) {
        *err = "no user identifier found";
        return false;
    }
    if (!parse_int64_arg(fields[1], out)) {
        *err = "username lookup is not supported, use a numeric user id";
        return false;
    }
    return true;
}

std::mutex g_flood_mu;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_flood;

int flood_left(const std::string& key) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_flood_mu);
    auto it = g_flood.find(key);
    if (it == g_flood.end() || it->second <= now) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(it->second - now).count());
}

void flood_set(const std::string& key, int seconds) {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::lock_guard<std::mutex> lk(g_flood_mu);
    g_flood[key] = until;
}

// No db API for these flags; process-local state (see admin_lists_NOTES.md).
std::mutex g_state_mu;
bool g_autoleave = false;
std::unordered_map<int64_t, bool> g_cleanmode;
std::unordered_map<int64_t, std::string> g_adminmode;

void handle_add_sudo(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner(m)) return;
    auto chat_id = m.chat_id;
    if (!arg_present(m)) {
        bot.reply_text(m, F(chat_id, "auth_no_user", {{"cmd", get_command(m)}}));
        return;
    }
    int64_t target = 0;
    std::string err;
    if (!extract_target_user(m, &target, &err)) {
        bot.reply_text(m, F(chat_id, "user_extract_fail", {{"error", err}}));
        return;
    }
    if (target == Config::get().owner_id) {
        bot.reply_text(m, F(chat_id, "sudo_owner_self"));
        return;
    }
    if (target == bot.me_id()) {
        bot.reply_text(m, F(chat_id, "sudo_bot_self"));
        return;
    }
    auto user = code_id(target);
    auto id_str = std::to_string(target);
    if (db::is_sudo(target)) {
        bot.reply_text(m, F(chat_id, "sudo_already", {{"user", user}, {"id", id_str}}));
        return;
    }
    if (!db::add_sudo(target)) {
        bot.reply_text(m, F(chat_id, "sudo_add_fail", {{"error", "storage error"}}));
        return;
    }
    bot.reply_text(m, F(chat_id, "sudo_add_success", {{"user", user}, {"id", id_str}}));
}

void handle_del_sudo(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner(m)) return;
    auto chat_id = m.chat_id;
    if (!arg_present(m)) {
        bot.reply_text(m, F(chat_id, "auth_no_user", {{"cmd", get_command(m)}}));
        return;
    }
    int64_t target = 0;
    std::string err;
    if (!extract_target_user(m, &target, &err)) {
        bot.reply_text(m, F(chat_id, "user_extract_fail", {{"error", err}}));
        return;
    }
    if (target == Config::get().owner_id) {
        bot.reply_text(m, F(chat_id, "sudo_owner_remove_block"));
        return;
    }
    auto user = code_id(target);
    auto id_str = std::to_string(target);
    if (!db::is_sudo(target)) {
        bot.reply_text(m, F(chat_id, "sudo_not_exists", {{"user", user}, {"id", id_str}}));
        return;
    }
    if (!db::del_sudo(target)) {
        bot.reply_text(m, F(chat_id, "sudo_remove_fail", {{"error", "storage error"}}));
        return;
    }
    bot.reply_text(m, F(chat_id, "sudo_remove_success", {{"user", user}, {"id", id_str}}));
}

void handle_get_sudoers(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    auto flood_key = "sudoers:" + std::to_string(chat_id) + std::to_string(m.sender_id);
    auto left = flood_left(flood_key);
    if (left > 0) {
        bot.reply_text(m, F(chat_id, "flood_seconds", {{"duration", std::to_string(left)}}));
        return;
    }
    flood_set(flood_key, 30);

    std::string sb;
    sb += F(chat_id, "sudo_list_header");
    sb += "\n\n";
    auto owner_id = Config::get().owner_id;
    sb += F(chat_id, "sudo_list_owner", {{"index", "1"}, {"user", code_id(owner_id)}});
    sb += "\n";

    int idx = 2;
    for (auto id : db::sudo_list()) {
        if (id == owner_id) continue;
        sb += F(chat_id, "sudo_list_entry", {{"index", std::to_string(idx)}, {"user", code_id(id)}});
        sb += "\n";
        idx++;
    }
    if (idx == 2) {
        sb += F(chat_id, "sudo_list_no_extra");
        sb += "\n";
    }
    bot.reply_text(m, sb);
}

void handle_block_user(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner(m)) return;
    auto chat_id = m.chat_id;
    if (!arg_present(m)) {
        bot.reply_text(m, F(chat_id, "auth_no_user", {{"cmd", get_command(m)}}));
        return;
    }
    int64_t target = 0;
    std::string err;
    if (!extract_target_user(m, &target, &err)) {
        bot.reply_text(m, F(chat_id, "user_extract_fail", {{"error", err}}));
        return;
    }
    if (!db::add_blacklisted_user(target)) {
        bot.reply_text(m, "Failed to block user: storage error");
        return;
    }
    bot.reply_text(m, F(chat_id, "blacklist_block_user_success", {{"id", std::to_string(target)}}));
}

void handle_unblock_user(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner(m)) return;
    auto chat_id = m.chat_id;
    if (!arg_present(m)) {
        bot.reply_text(m, F(chat_id, "auth_no_user", {{"cmd", get_command(m)}}));
        return;
    }
    int64_t target = 0;
    std::string err;
    if (!extract_target_user(m, &target, &err)) {
        bot.reply_text(m, F(chat_id, "user_extract_fail", {{"error", err}}));
        return;
    }
    if (!db::del_blacklisted_user(target)) {
        bot.reply_text(m, F(chat_id, "blacklist_unblock_user_fail", {{"error", "storage error"}}));
        return;
    }
    bot.reply_text(m, F(chat_id, "blacklist_unblock_user_success", {{"id", std::to_string(target)}}));
}

void handle_block_chat(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner(m)) return;
    auto chat_id = m.chat_id;
    auto arg = first_arg(m);
    if (arg.empty()) {
        bot.reply_text(m, F(chat_id, "blacklist_usage_blockchat"));
        return;
    }
    int64_t target = 0;
    if (!parse_int64_arg(arg, &target)) {
        bot.reply_text(m, F(chat_id, "blacklist_invalid_chat_identifier",
                                 {{"error", "username lookup is not supported, use a numeric chat id"}}));
        return;
    }
    if (!db::add_blacklisted_chat(target)) {
        bot.reply_text(m, "Failed to block chat: storage error");
        return;
    }
    bot.reply_text(m, F(chat_id, "blacklist_block_chat_success", {{"id", std::to_string(target)}}));
}

void handle_unblock_chat(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner(m)) return;
    auto chat_id = m.chat_id;
    auto arg = first_arg(m);
    if (arg.empty()) {
        bot.reply_text(m, F(chat_id, "blacklist_usage_unblockchat"));
        return;
    }
    int64_t target = 0;
    if (!parse_int64_arg(arg, &target)) {
        bot.reply_text(m, F(chat_id, "blacklist_invalid_chat_identifier",
                                 {{"error", "username lookup is not supported, use a numeric chat id"}}));
        return;
    }
    if (!db::del_blacklisted_chat(target)) {
        bot.reply_text(m, F(chat_id, "blacklist_unblock_chat_fail", {{"error", "storage error"}}));
        return;
    }
    bot.reply_text(m, F(chat_id, "blacklist_unblock_chat_success", {{"id", std::to_string(target)}}));
}

void handle_blacklisted(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner(m)) return;
    auto chat_id = m.chat_id;
    auto chats = db::blacklisted_chats();
    auto users = db::blacklisted_users();

    std::string sb;
    sb += F(chat_id, "blacklist_list_title");
    sb += "\n\n";
    sb += F(chat_id, "blacklist_list_chats");
    sb += "\n";
    if (chats.empty()) {
        sb += "\xE2\x80\xA2 None\n";
    } else {
        for (size_t i = 0; i < chats.size(); ++i)
            sb += std::to_string(i + 1) + ". " + code_id(chats[i]) + "\n";
    }
    sb += "\n";
    sb += F(chat_id, "blacklist_list_users");
    sb += "\n";
    if (users.empty()) {
        sb += "\xE2\x80\xA2 None";
    } else {
        for (size_t i = 0; i < users.size(); ++i)
            sb += std::to_string(i + 1) + ". " + code_id(users[i]) + "\n";
    }
    bot.reply_text(m, sb);
}

void add_auth(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    if (!arg_present(m)) {
        bot.reply_text(m, F(chat_id, "auth_no_user", {{"cmd", get_command(m)}}));
        return;
    }
    auto limit = Config::get().max_auth_users;
    if (static_cast<int>(db::auth_users(chat_id).size()) >= limit) {
        bot.reply_text(m, F(chat_id, "auth_limit_reached", {{"limit", std::to_string(limit)}}));
        return;
    }
    int64_t target = 0;
    std::string err;
    if (!extract_target_user(m, &target, &err)) {
        bot.reply_text(m, F(chat_id, "user_extract_fail", {{"error", err}}));
        return;
    }
    if (target == Config::get().owner_id || target == bot.me_id() || target == m.sender_id) {
        bot.reply_text(m, F(chat_id, "cannot_authorize_user"));
        return;
    }
    if (db::is_authorized(chat_id, target)) {
        bot.reply_text(m, F(chat_id, "already_authed"));
        return;
    }
    if (!db::add_auth_user(chat_id, target)) {
        bot.reply_text(m, F(chat_id, "addauth_add_fail", {{"error", "storage error"}}));
        return;
    }
    auto au = db::auth_users(chat_id);
    auto user = code_id(target);
    if (!au.empty()) {
        bot.reply_text(m, F(chat_id, "addauth_success_with_count",
                                 {{"user", user},
                                  {"count", std::to_string(au.size())},
                                  {"limit", std::to_string(limit)}}));
    } else {
        bot.reply_text(m, F(chat_id, "addauth_success", {{"user", user}}));
    }
}

void del_auth(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    if (!arg_present(m)) {
        bot.reply_text(m, F(chat_id, "auth_no_user", {{"cmd", get_command(m)}}));
        return;
    }
    int64_t target = 0;
    std::string err;
    if (!extract_target_user(m, &target, &err)) {
        bot.reply_text(m, F(chat_id, "user_extract_fail", {{"error", err}}));
        return;
    }
    if (!db::is_authorized(chat_id, target)) {
        bot.reply_text(m, F(chat_id, "del_auth_not_authorized"));
        return;
    }
    if (!db::del_auth_user(chat_id, target)) {
        bot.reply_text(m, F(chat_id, "del_auth_remove_fail", {{"error", "storage error"}}));
        return;
    }
    bot.reply_text(m, F(chat_id, "del_auth_success", {{"user", "User (" + code_id(target) + ")"}}));
}

void auth_list(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    auto au = db::auth_users(chat_id);
    if (au.empty()) {
        bot.reply_text(m, F(chat_id, "authlist_empty"));
        return;
    }
    std::string sb;
    sb += F(chat_id, "authlist_header");
    sb += "\n";
    for (size_t i = 0; i < au.size(); ++i) {
        sb += F(chat_id, "authlist_user_fail",
                {{"index", std::to_string(i + 1)}, {"user_id", std::to_string(au[i])}});
        sb += "\n";
    }
    sb += "\n" + F(chat_id, "authlist_total", {{"count", std::to_string(au.size())}});
    bot.reply_text(m, sb);
}

void autoleave(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner_or_sudo(m)) return;
    auto chat_id = m.chat_id;
    bool current = false;
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        current = g_autoleave;
    }
    auto fields = utils::split_fields(m.text);
    if (fields.size() < 2) {
        bot.reply_text(m, F(chat_id, "autoleave_status",
                                 {{"cmd", get_command(m)},
                                  {"action", F(chat_id, current ? "enabled" : "disabled")}}));
        return;
    }
    auto enabled = strict_bool(fields[1]);
    if (!enabled) {
        bot.reply_text(m, F(chat_id, "invalid_bool"));
        return;
    }
    auto action = F(chat_id, *enabled ? "enabled" : "disabled");
    if (*enabled == current) {
        bot.reply_text(m, F(chat_id, "autoleave_already", {{"action", action}}));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        g_autoleave = *enabled;
    }
    bot.reply_text(m, F(chat_id, "autoleave_updated", {{"action", action}}));
}

void playmode(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!group_gate(m)) return;
    auto chat_id = m.chat_id;
    auto fields = utils::split_fields(m.text);
    auto current = db::play_mode_admins_only(chat_id);
    if (fields.size() < 2) {
        bot.reply_text(m, F(chat_id, "playmode_help",
                                 {{"status", F(chat_id, current ? "playmode_status_admins"
                                                                 : "playmode_status_everyone")}}));
        return;
    }
    auto admins_only = strict_bool(fields[1]);
    if (!admins_only) {
        bot.reply_text(m, F(chat_id, "invalid_bool"));
        return;
    }
    db::set_play_mode_admins_only(chat_id, *admins_only);
    bot.reply_text(m, F(chat_id, *admins_only ? "playmode_success_admins" : "playmode_success_everyone"));
}

// Go passes the raw "enabled"/"disabled" word (un-localized) as {action} here.
void nothumb(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!group_gate(m)) return;
    auto chat_id = m.chat_id;
    auto fields = utils::split_fields(m.text);
    auto current = db::thumbnails_disabled(chat_id);
    if (fields.size() < 2) {
        bot.reply_text(m, F(chat_id, "nothumb_status",
                                 {{"cmd", get_command(m)},
                                  {"action", current ? "disabled" : "enabled"}}));
        return;
    }
    auto value = strict_bool(fields[1]);
    if (!value) {
        bot.reply_text(m, F(chat_id, "invalid_bool"));
        return;
    }
    auto action = *value ? "disabled" : "enabled";
    if (*value == current) {
        bot.reply_text(m, F(chat_id, "nothumb_already", {{"action", action}}));
        return;
    }
    db::set_thumbnails_disabled(chat_id, *value);
    bot.reply_text(m, F(chat_id, "nothumb_updated", {{"action", action}}));
}

std::string clean_mode_text(int64_t chat_id, bool enabled) {
    return F(chat_id, "cleanmode_status",
             {{"action", F(chat_id, enabled ? "enabled" : "disabled")}, {"duration", "15"}}) +
           "\n\n" + F(chat_id, "cleanmode_hint");
}

void clean_mode(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!group_gate(m)) return;
    auto chat_id = m.chat_id;
    auto fields = utils::split_fields(m.text);
    bool current = false;
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        auto it = g_cleanmode.find(chat_id);
        if (it != g_cleanmode.end()) current = it->second;
    }
    if (fields.size() < 2) {
        bot.reply_text(m, clean_mode_text(chat_id, current));
        return;
    }
    auto enabled = strict_bool(fields[1]);
    if (!enabled) {
        bot.reply_text(m, F(chat_id, "invalid_bool"));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        g_cleanmode[chat_id] = *enabled;
    }
    bot.reply_text(m, clean_mode_text(chat_id, *enabled));
}

std::string admin_mode_status_key(const std::string& mode) {
    if (mode == "admins") return "adminmode_status_admin";
    if (mode == "everyone") return "adminmode_status_everyone";
    return "adminmode_status_adminauth";
}

std::optional<std::string> parse_admin_mode(const std::string& input) {
    auto v = utils::to_lower(utils::trim(input));
    if (v == "admin" || v == "admins" || v == "adminonly" || v == "adminsonly" || v == "admins_only")
        return "admins";
    if (v == "adminauth" || v == "auth" || v == "admin+auth" || v == "dj" || v == "admin_auth")
        return "adminauth";
    if (v == "everyone" || v == "all") return "everyone";
    return std::nullopt;
}

void admin_mode(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!group_gate(m)) return;
    auto chat_id = m.chat_id;
    auto fields = utils::split_fields(m.text);
    std::string current;
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        auto it = g_adminmode.find(chat_id);
        current = it != g_adminmode.end() ? it->second : "adminauth";
    }
    if (fields.size() < 2) {
        bot.reply_text(m,
                       F(chat_id, "adminmode_help", {{"status", F(chat_id, admin_mode_status_key(current))}}));
        return;
    }
    auto mode = parse_admin_mode(fields[1]);
    if (!mode) {
        bot.reply_text(m, F(chat_id, "adminmode_invalid"));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        g_adminmode[chat_id] = *mode;
    }
    bot.reply_text(m, F(chat_id, "adminmode_updated", {{"status", F(chat_id, admin_mode_status_key(*mode))}}));
}

// Go resolves @username / "linked" via SearchPublicChat + supergroup metadata
// and verifies bot admin rights in the target; only raw numeric -100 ids work
// here (see admin_lists_NOTES.md).
void set_cplay(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!group_gate(m)) return;
    auto chat_id = m.chat_id;
    auto fields = utils::split_fields(m.text);
    if (fields.size() <= 1) {
        bot.reply_text(m, F(chat_id, "cplay_usage"));
        return;
    }
    auto arg = utils::to_lower(utils::trim(fields[1]));
    auto flag = strict_bool(arg);
    bool clear = arg == "reset" || arg == "none" || arg == "del" || (flag && !*flag);
    if (clear) {
        db::set_linked_channel(chat_id, 0);
        bot.reply_text(m, F(chat_id, "cplay_disabled"));
        return;
    }
    if (arg == "linked") {
        bot.reply_text(m, F(chat_id, "cplay_resolve_peer_fail"));
        return;
    }
    int64_t target = 0;
    if (!parse_int64_arg(arg, &target) || target >= 0) {
        bot.reply_text(m, F(chat_id, "cplay_invalid_target"));
        return;
    }
    db::set_linked_channel(chat_id, target);
    bot.reply_text(m, F(chat_id, "cplay_enabled", {{"channel_id", std::to_string(target)}}));
}

nlohmann::json autoplay_menu(int64_t chat_id, bool enabled) {
    auto state = F(chat_id, enabled ? "enabled" : "disabled");
    return markup_rows({
        {data_btn(F(chat_id, "autoplay_btn", {{"state", state}}), "autoplay:toggle")},
        {data_btn(F(chat_id, "CLOSE_BTN"), "close")},
    });
}

void autoplay(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!group_gate(m)) return;
    auto chat_id = m.chat_id;
    bool enabled = false;
    auto* r = room_for(chat_id);
    if (r && r->active()) {
        enabled = r->autoplay();
    } else {
        enabled = db::autoplay(chat_id);
    }
    auto text = F(chat_id, "autoplay_menu", {{"state", F(chat_id, enabled ? "enabled" : "disabled")}});
    bot.reply_text(m, text, autoplay_menu(chat_id, enabled));
}

nlohmann::json lang_markup(int64_t chat_id, const std::string& current, bool with_back) {
    std::vector<std::vector<nlohmann::json>> rows;
    std::vector<nlohmann::json> row;
    for (auto& l : Locales::instance().available()) {
        auto name = Locales::instance().get(l, "name");
        if (l == current) name = "\xE2\x9C\x94\xEF\xB8\x8F " + name;
        row.push_back(data_btn(name, "lang:" + l));
        if (row.size() == 2) {
            rows.push_back(row);
            row.clear();
        }
    }
    if (!row.empty()) rows.push_back(row);
    if (with_back) rows.push_back({data_btn(F(chat_id, "BACK_BTN"), "set:main")});
    return markup_rows(rows);
}

void lang_cmd(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!group_gate(m)) return;
    auto chat_id = m.chat_id;
    auto lang = db::language(chat_id);
    if (lang.empty()) lang = Config::get().default_lang;
    bot.reply_text(m, F(chat_id, "lang_select"), lang_markup(chat_id, lang, false));
}

void lang_cb(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    auto chat_id = cb.chat_id;
    if (cb.data == "lang:select") {
        auto cur = db::language(chat_id);
        if (cur.empty()) cur = Config::get().default_lang;
        bot.answer_callback(cb.query_id);
        bot.edit_text(chat_id, cb.message_id, F(chat_id, "lang_select"), lang_markup(chat_id, cur, true));
        return;
    }
    if (cb.data.size() < 5 || cb.data.compare(0, 5, "lang:") != 0) {
        bot.answer_callback(cb.query_id);
        return;
    }
    auto lang = cb.data.substr(5);
    bool valid = lang.size() == 2;
    for (char c : lang) {
        if (c < 'a' || c > 'z') valid = false;
    }
    if (!valid) {
        bot.answer_callback(cb.query_id);
        return;
    }
    auto current = db::language(chat_id);
    if (lang == current) {
        bot.answer_callback(cb.query_id);
        bot.edit_text(chat_id, cb.message_id, F(chat_id, "lang_same"));
        return;
    }
    db::set_language(chat_id, lang);
    auto msg = F(chat_id, "lang_success", {{"lang_name", Locales::instance().get(lang, "name")}});
    bot.answer_callback(cb.query_id);
    bot.edit_text(chat_id, cb.message_id, msg);
}

}  // namespace

void init_admin_lists(tg::TdClient& bot) {
    bot.on_command("addsudo", handle_add_sudo);
    bot.on_command("addsudoer", handle_add_sudo);
    bot.on_command("sudoadd", handle_add_sudo);
    bot.on_command("delsudo", handle_del_sudo);
    bot.on_command("delsudoer", handle_del_sudo);
    bot.on_command("sudodel", handle_del_sudo);
    bot.on_command("remsudo", handle_del_sudo);
    bot.on_command("rmsudo", handle_del_sudo);
    bot.on_command("sudorem", handle_del_sudo);
    bot.on_command("dropsudo", handle_del_sudo);
    bot.on_command("unsudo", handle_del_sudo);
    bot.on_command("sudoers", handle_get_sudoers);
    bot.on_command("listsudo", handle_get_sudoers);
    bot.on_command("sudolist", handle_get_sudoers);
    bot.on_command("blockchat", handle_block_chat);
    bot.on_command("blacklistchat", handle_block_chat);
    bot.on_command("blackchat", handle_block_chat);
    bot.on_command("blchat", handle_block_chat);
    bot.on_command("unblockchat", handle_unblock_chat);
    bot.on_command("unblacklistchat", handle_unblock_chat);
    bot.on_command("unblackchat", handle_unblock_chat);
    bot.on_command("whitechat", handle_unblock_chat);
    bot.on_command("unblchat", handle_unblock_chat);
    bot.on_command("blockuser", handle_block_user);
    bot.on_command("blacklistuser", handle_block_user);
    bot.on_command("blackuser", handle_block_user);
    bot.on_command("bluser", handle_block_user);
    bot.on_command("unblockuser", handle_unblock_user);
    bot.on_command("unblacklistuser", handle_unblock_user);
    bot.on_command("unbluser", handle_unblock_user);
    bot.on_command("whitelistuser", handle_unblock_user);
    bot.on_command("blocked", handle_blacklisted);
    bot.on_command("blacklisted", handle_blacklisted);
    bot.on_command("auth", add_auth);
    bot.on_command("addauth", add_auth);
    bot.on_command("authorize", add_auth);
    bot.on_command("delauth", del_auth);
    bot.on_command("unauth", del_auth);
    bot.on_command("delauthuser", del_auth);
    bot.on_command("authlist", auth_list);
    bot.on_command("authusers", auth_list);
    bot.on_command("autoleave", autoleave);
    bot.on_command("adminmode", admin_mode);
    bot.on_command("cleanmode", clean_mode);
    bot.on_command("playmode", playmode);
    bot.on_command("nothumb", nothumb);
    bot.on_command("nothumbs", nothumb);
    bot.on_command("channelplay", set_cplay);
    bot.on_command("setcplay", set_cplay);
    bot.on_command("autoplay", autoplay);
    bot.on_command("lang", lang_cmd);
    bot.on_command("language", lang_cmd);

    bot.on_callback("lang:", lang_cb);

    ADVAIT_LOG_INFO("modules", "admin lists handlers registered");
}

}  // namespace advait::modules
