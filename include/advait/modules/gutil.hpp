#pragma once

// Shared helpers for the group admin / moderation / sticker modules — the C++
// port of Aishamusic-2's group_admin.go + group_moderation.go helpers.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "advait/core/config.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/telegram/tdlib.hpp"

namespace advait::modules::gutil {
using json = nlohmann::json;
using tg::Message;
using tg::TdClient;

inline std::string styled(const std::string& s) { return utils::stylish(s); }

inline std::string args_of(const Message& m) {
    auto t = utils::trim(m.text);
    if (t.empty() || t[0] != '/') return t;
    auto sp = t.find(' ');
    return sp == std::string::npos ? std::string() : t.substr(sp + 1);
}

inline std::string args_after_first(const Message& m) {
    auto full = args_of(m);
    auto toks = utils::split_fields(full);
    if (toks.empty()) return {};
    auto pos = full.find(toks[0]);
    if (pos == std::string::npos) return {};
    return utils::trim(full.substr(pos + toks[0].size()));
}

inline void reply(TdClient& bot, const Message& m, const std::string& text) { bot.reply_text(m, text); }

inline std::vector<std::string> fields(const std::string& s) { return utils::split_fields(s); }

// ── durations (10m / 2h / 1d) ─────────────────────────────────────────────────
struct Dur {
    int32_t seconds{0};
    std::string human;
    bool ok{false};
};

inline Dur parse_duration_token(const std::string& tok) {
    if (tok.size() < 2) return {};
    long v = 0;
    size_t i = 0;
    while (i < tok.size() && tok[i] >= '0' && tok[i] <= '9') {
        v = v * 10 + (tok[i] - '0');
        i++;
    }
    if (i == 0 || i + 1 != tok.size() || v <= 0) return {};
    long secs = 0;
    switch (std::tolower(static_cast<unsigned char>(tok[i]))) {
        case 'm': secs = v * 60; break;
        case 'h': secs = v * 3600; break;
        case 'd': secs = v * 86400; break;
        default: return {};
    }
    if (secs > INT32_MAX) return {};
    return Dur{static_cast<int32_t>(secs), std::to_string(v) + std::string(1, tok[i]), true};
}

// ── member status / rights ────────────────────────────────────────────────────
struct Rights {
    bool manage{false};
    bool change_info{false};
    bool del{false};
    bool invite{false};
    bool restrict{false};
    bool pin{false};
    bool promote{false};
    bool video{false};
};

inline json chat_member(TdClient& bot, int64_t chat_id, int64_t user_id) {
    auto resp = bot.call_sync({{"@type", "getChatMember"},
                               {"chat_id", chat_id},
                               {"member_id", {{"@type", "messageSenderUser"}, {"user_id", user_id}}}});
    if (!resp.is_object() || resp.value("@type", "") != "chatMember") return {};
    return resp;
}

// nlohmann::json value() throws type_error.306 when the receiver is not an
// object (a thrown error in a hook kills the whole dispatch), so reads in
// module code go through these guarded accessors.
inline std::string jstr(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

inline json jobj(const json& j, const char* key) {
    if (!j.is_object()) return json::object();
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return json::object();
    return *it;
}

inline int64_t jint(const json& j, const char* key, int64_t dflt = 0) {
    if (!j.is_object()) return dflt;
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return dflt;
    return it->get<int64_t>();
}

// Extracted plain text of a replied message when it is a text message.
inline std::string quoted_text(const json& rep) {
    auto content = rep.find("content");
    if (content == rep.end() || !content->is_object()) return {};
    auto ft = content->find("text");
    if (ft == content->end() || !ft->is_object()) return {};
    auto t = ft->find("text");
    if (t == ft->end() || !t->is_string()) return {};
    return t->get<std::string>();
}

inline bool status_is(const json& status, const char* kind) {
    if (!status.is_object()) return false;
    auto it = status.find("@type");
    return it != status.end() && it->is_string() && it->get<std::string>() == kind;
}

// TDLib >= 1.8 moved administrator flags out of chatMemberStatusAdministrator
// into a nested chatAdministratorRights ("rights"); older servers keep them
// flat on the status. Read both.
inline bool bool_field(const json& j, const char* key) {
    if (!j.is_object()) return false;
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

inline Rights rights_from_status(const json& status) {
    if (status_is(status, "chatMemberStatusCreator"))
        return Rights{true, true, true, true, true, true, true, true};
    if (status_is(status, "chatMemberStatusAdministrator")) {
        auto rights = jobj(status, "rights");
        const json& src = rights.is_object() ? rights : status;
        Rights r;
        r.manage = bool_field(src, "can_manage_chat");
        r.del = bool_field(src, "can_delete_messages") || bool_field(src, "can_edit_messages");
        r.invite = bool_field(src, "can_invite_users");
        r.restrict = bool_field(src, "can_restrict_members");
        r.pin = bool_field(src, "can_pin_messages");
        r.promote = bool_field(src, "can_promote_members");
        r.video = bool_field(src, "can_manage_video_chats");
        return r;
    }
    return {};
}

inline Rights bot_rights(TdClient& bot, int64_t chat_id) {
    auto member = chat_member(bot, chat_id, bot.me_id());
    if (member.is_null()) return {};
    return rights_from_status(member.value("status", json::object()));
}

inline Rights caller_rights(TdClient& bot, int64_t chat_id, int64_t user_id) {
    if (Config::get().owner_id == user_id || db::is_sudo(user_id)) {
        return Rights{true, true, true, true, true, true, true, true};
    }
    auto member = chat_member(bot, chat_id, user_id);
    if (member.is_null()) return {};
    return rights_from_status(member.value("status", json::object()));
}

inline bool is_telegram_admin(TdClient& bot, int64_t chat_id, int64_t user_id) {
    auto member = chat_member(bot, chat_id, user_id);
    if (member.is_null()) return false;
    auto st = member.value("status", json::object());
    return status_is(st, "chatMemberStatusAdministrator") || status_is(st, "chatMemberStatusCreator");
}

inline bool caller_is_owner_or_sudo(const Message& m) {
    return Config::get().owner_id == m.sender_id || db::is_sudo(m.sender_id);
}

// requireChatFlag parity: group-only + bot right + caller right (sudo/owner bypass).
template <typename Select>
bool require_chat_flag(TdClient& bot, const Message& m, const char* need, Select select) {
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return false;
    }
    if (!select(bot_rights(bot, m.chat_id))) {
        reply(bot, m, styled("I need " + std::string(need) + " permission to do that. Promote me as an admin first."));
        return false;
    }
    if (caller_is_owner_or_sudo(m)) return true;
    if (!select(caller_rights(bot, m.chat_id, m.sender_id))) {
        reply(bot, m, styled("You need " + std::string(need) + " permission to use this command."));
        return false;
    }
    return true;
}

// requireTelegramRight parity (restrict or promote variants).
inline bool require_right(TdClient& bot, const Message& m, const char* need, bool promote) {
    return require_chat_flag(bot, m, need, [promote](const Rights& r) {
        return promote ? r.promote : r.restrict;
    });
}

inline bool require_pin_right(TdClient& bot, const Message& m) {
    return require_chat_flag(bot, m, "pin messages", [](const Rights& r) { return r.pin; });
}

inline bool require_delete_right(TdClient& bot, const Message& m) {
    return require_chat_flag(bot, m, "delete messages", [](const Rights& r) { return r.del; });
}

// ── target resolution ─────────────────────────────────────────────────────────
inline std::string mention_html(TdClient& bot, int64_t user_id) {
    auto u = bot.get_user(user_id);
    if (u.is_null()) return "<code>" + std::to_string(user_id) + "</code>";
    std::string name = utils::trim(u.value("first_name", "") + " " + u.value("last_name", ""));
    if (name.empty()) name = std::to_string(user_id);
    return "<a href=\"tg://user?id=" + std::to_string(user_id) + "\">" + utils::escape_html(name) + "</a>";
}

inline int64_t resolve_from_reply(TdClient& bot, const Message& m, std::string* err) {
    auto rep = bot.get_replied_message(m.chat_id, m.reply_to);
    if (rep.is_null()) {
        if (err) *err = "failed to fetch replied message";
        return 0;
    }
    auto s = rep.value("sender_id", json::object());
    int64_t uid = s.value("user_id", 0LL);
    if (uid == 0 && err) *err = "replied message has no identifiable sender";
    return uid;
}

inline int64_t resolve_from_arg(TdClient& bot, std::string arg, std::string* err) {
    arg = utils::trim(arg);
    if (!arg.empty() && arg[0] == '@') arg.erase(0, 1);
    if (arg.empty()) {
        if (err) *err = "username cannot be empty";
        return 0;
    }
    if (std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return std::isdigit(c); })) {
        int64_t id = std::strtoll(arg.c_str(), nullptr, 10);
        if (id <= 0 && err) *err = "invalid user id";
        return id;
    }
    auto chat = bot.search_public_chat(arg);
    if (chat.is_null()) {
        if (err) *err = "username lookup failed for \"" + arg + "\"";
        return 0;
    }
    auto t = chat.value("type", json::object());
    if (t.value("@type", "") != "chatTypePrivate") {
        if (err) *err = "resolved peer is not a user";
        return 0;
    }
    return t.value("user_id", 0LL);
}

inline int64_t resolve_admin_target(TdClient& bot, const Message& m, const std::vector<std::string>& user_args,
                                    std::string* err) {
    ADVAIT_LOG_INFO("group_admin", "resolve target: reply_to=" + std::to_string(m.reply_to) +
                                       " args=" + std::to_string(user_args.size()));
    if (m.reply_to != 0) return resolve_from_reply(bot, m, err);
    if (user_args.empty()) {
        if (err) *err = "no target";
        return 0;
    }
    return resolve_from_arg(bot, user_args[0], err);
}

inline std::string protected_target(TdClient& bot, int64_t user_id) {
    if (user_id == bot.me_id()) return "I can't perform that action on myself.";
    if (Config::get().owner_id != 0 && user_id == Config::get().owner_id)
        return "That user is protected and can't be targeted.";
    if (db::is_sudo(user_id)) return "That user is protected and can't be targeted.";
    return "";
}

// ── TDLib member mutations ────────────────────────────────────────────────────
inline bool set_member_status(TdClient& bot, int64_t chat_id, int64_t user_id, const json& status,
                              std::string* err = nullptr) {
    auto resp = bot.send_query_sync_public(
        {{"@type", "setChatMemberStatus"},
         {"chat_id", chat_id},
         {"member_id", {{"@type", "messageSenderUser"}, {"user_id", user_id}}},
         {"status", status}},
        15000);
    if (!resp.is_object() || resp.value("@type", "") == "error") {
        if (err)
            *err = "telegram_error:" + std::to_string(resp.is_object() ? resp.value("code", 0) : 0) + ":" +
                   (resp.is_object() ? resp.value("message", std::string()) : std::string());
        return false;
    }
    return true;
}

inline json admin_rights_json(const Rights& r) {
    return {{"@type", "chatAdministratorRights"},
            {"can_manage_chat", r.manage},
            {"can_change_info", r.change_info},
            {"can_post_messages", r.manage},
            {"can_delete_messages", r.del},
            {"can_invite_users", r.invite},
            {"can_restrict_members", r.restrict},
            {"can_pin_messages", r.pin},
            {"can_promote_members", r.promote},
            {"can_manage_video_chats", r.video},
            {"is_anonymous", false}};
}

inline bool ban_user(TdClient& bot, int64_t chat_id, int64_t user_id, int32_t until_sec, std::string* err = nullptr) {
    int64_t until = 0;
    if (until_sec > 0) until = static_cast<int64_t>(std::time(nullptr)) + until_sec;
    return set_member_status(bot, chat_id, user_id,
                             {{"@type", "chatMemberStatusBanned"}, {"banned_until_date", until}}, err);
}

inline bool unban_user(TdClient& bot, int64_t chat_id, int64_t user_id, std::string* err = nullptr) {
    return set_member_status(bot, chat_id, user_id, {{"@type", "chatMemberStatusMember"}}, err);
}

inline std::string chat_title(TdClient& bot, int64_t chat_id) {
    auto chat = bot.call_sync({{"@type", "getChat"}, {"chat_id", chat_id}});
    if (chat.is_null()) return "Group";
    return chat.value("title", std::string("Group"));
}

}  // namespace advait::modules::gutil
