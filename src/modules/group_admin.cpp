// Group admin toolkit: promote/demote/ban/kick/warns/pin/purge — a faithful
// port of Aishamusic-2 modules/group_admin.go on TDLib.
#include "advait/modules/group_admin.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "advait/core/logger.hpp"
#include "advait/modules/gutil.hpp"

namespace advait::modules {
namespace {
using namespace advait::modules::gutil;
using tg::Message;
using tg::TdClient;

// promoteLevels parity: 1 junior, 2 senior, 3 full.
struct PromoteLevel {
    const char* title;
    Rights rights;
};
PromoteLevel promote_level(int lvl) {
    switch (lvl) {
        case 1: return {"Junior Admin", Rights{false, false, true, false, true, false, false, false}};
        case 2: return {"Senior Admin", Rights{false, true, true, true, true, true, false, false}};
        case 3: return {"Full Admin", Rights{true, true, true, true, true, true, false, true}};
        default: return {"", Rights{}};
    }
}

Rights promote_rights() { return Rights{true, true, true, true, true, true, false, true}; }

// ── handlers ────────────────────────────────────────────────────────────────
void promote_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "promote members", true)) return;
    auto toks = fields(args_of(m));
    int level = 0;
    std::vector<std::string> user_args;
    for (auto& t : toks) {
        if (level == 0 && (t == "1" || t == "2" || t == "3")) {
            level = t[0] - '0';
            continue;
        }
        user_args.push_back(t);
    }
    std::string rerr;
    int64_t uid = resolve_admin_target(bot, m, user_args, &rerr);
    if (uid == 0) {
        reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /promote [user] [1|2|3]"));
        return;
    }
    if (auto reason = protected_target(bot, uid); !reason.empty()) {
        reply(bot, m, styled(reason));
        return;
    }
    Rights rights = promote_rights();
    std::string title;
    if (level > 0) {
        auto info = promote_level(level);
        if (level > 3) {
            reply(bot, m, styled("Promote level must be 1, 2, or 3."));
            return;
        }
        rights = info.rights;
        title = info.title;
    }
    json status = {{"@type", "chatMemberStatusAdministrator"},
                   {"can_be_edited", false},
                   {"rights", admin_rights_json(rights)}};
    if (!title.empty()) status["custom_title"] = title;
    std::string serr;
    if (!set_member_status(bot, m.chat_id, uid, status, &serr)) {
        ADVAIT_LOG_WARN("group_admin", "promote failed: " + serr);
        reply(bot, m, styled("Failed to promote: " + utils::escape_html(serr)));
        return;
    }
    if (!title.empty()) {
        reply(bot, m, styled("Promoted " + mention_html(bot, uid) + " to " + title + "."));
    } else {
        reply(bot, m, styled("Promoted " + mention_html(bot, uid) + " to admin."));
    }
}

void full_promote_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "promote members", true)) return;
    std::string rerr;
    int64_t uid = resolve_admin_target(bot, m, fields(args_of(m)), &rerr);
    if (uid == 0) {
        reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /fullpromote [user]"));
        return;
    }
    if (auto reason = protected_target(bot, uid); !reason.empty()) {
        reply(bot, m, styled(reason));
        return;
    }
    json status = {{"@type", "chatMemberStatusAdministrator"},
                   {"custom_title", "Full Admin"},
                   {"can_be_edited", false},
                   {"rights", admin_rights_json(Rights{true, true, true, true, true, true, true, true})}};
    std::string ferr;
    if (!set_member_status(bot, m.chat_id, uid, status, &ferr)) {
        ADVAIT_LOG_WARN("group_admin", "fullpromote failed: " + ferr);
        reply(bot, m, styled("Failed to promote: " + utils::escape_html(ferr)));
        return;
    }
    reply(bot, m, styled("Fully promoted " + mention_html(bot, uid) + "."));
}

void demote_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "promote members", true)) return;
    std::string rerr;
    int64_t uid = resolve_admin_target(bot, m, fields(args_of(m)), &rerr);
    if (uid == 0) {
        reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /demote [user]"));
        return;
    }
    if (auto reason = protected_target(bot, uid); !reason.empty()) {
        reply(bot, m, styled(reason));
        return;
    }
    auto member = chat_member(bot, m.chat_id, uid);
    if (!member.is_null() && status_is(jobj(member, "status"), "chatMemberStatusCreator")) {
        reply(bot, m, styled("You can't demote the chat owner."));
        return;
    }
    std::string derr;
    if (!set_member_status(bot, m.chat_id, uid, {{"@type", "chatMemberStatusMember"}}, &derr)) {
        ADVAIT_LOG_WARN("group_admin", "demote failed: " + derr);
        reply(bot, m, styled("Failed to demote: " + utils::escape_html(derr)));
        return;
    }
    reply(bot, m, styled("Demoted " + mention_html(bot, uid) + "."));
}

void ban_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    auto toks = fields(args_of(m));
    Dur dur;
    std::vector<std::string> user_args;
    for (auto& t : toks) {
        if (auto d = parse_duration_token(t); d.ok) {
            dur = d;
            continue;
        }
        user_args.push_back(t);
    }
    std::string rerr;
    int64_t uid = resolve_admin_target(bot, m, user_args, &rerr);
    if (uid == 0) {
        reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /ban [user] [10m|2h|1d]"));
        return;
    }
    if (auto reason = protected_target(bot, uid); !reason.empty()) {
        reply(bot, m, styled(reason));
        return;
    }
    if (is_telegram_admin(bot, m.chat_id, uid)) {
        reply(bot, m, styled("You can't restrict an admin."));
        return;
    }
    std::string berr;
    if (!ban_user(bot, m.chat_id, uid, dur.seconds, &berr)) {
        ADVAIT_LOG_WARN("group_admin", "ban failed: " + berr);
        reply(bot, m, styled("Failed to ban: " + utils::escape_html(berr)));
        return;
    }
    reply(bot, m, styled("Banned " + mention_html(bot, uid) +
                         (dur.human.empty() ? std::string("") : " (" + dur.human + ")") + "."));
}

void unban_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    std::string rerr;
    int64_t uid = resolve_admin_target(bot, m, fields(args_of(m)), &rerr);
    if (uid == 0) {
        reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /unban [user]"));
        return;
    }
    std::string uerr;
    if (!unban_user(bot, m.chat_id, uid, &uerr)) {
        ADVAIT_LOG_WARN("group_admin", "unban failed: " + uerr);
        reply(bot, m, styled("Failed to unban: " + utils::escape_html(uerr)));
        return;
    }
    reply(bot, m, styled("Unbanned " + mention_html(bot, uid) + "."));
}

void kick_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    std::string rerr;
    int64_t uid = resolve_admin_target(bot, m, fields(args_of(m)), &rerr);
    if (uid == 0) {
        reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /kick [user]"));
        return;
    }
    if (auto reason = protected_target(bot, uid); !reason.empty()) {
        reply(bot, m, styled(reason));
        return;
    }
    if (is_telegram_admin(bot, m.chat_id, uid)) {
        reply(bot, m, styled("You can't restrict an admin."));
        return;
    }
    std::string kerr;
    if (!ban_user(bot, m.chat_id, uid, 0, &kerr)) {
        ADVAIT_LOG_WARN("group_admin", "kick failed: " + kerr);
        reply(bot, m, styled("Failed to kick: " + utils::escape_html(kerr)));
        return;
    }
    unban_user(bot, m.chat_id, uid);
    reply(bot, m, styled("Kicked " + mention_html(bot, uid) + "."));
}

// ── warns ───────────────────────────────────────────────────────────────────
// warnReasonFromArgs: with a reply, everything is the reason; otherwise the
// first token is the user and the rest the reason.
void warn_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    auto toks = fields(args_of(m));
    std::string reason;
    std::vector<std::string> user_args;
    if (m.reply_to != 0) {
        reason = utils::trim(args_of(m));
    } else if (!toks.empty()) {
        user_args.push_back(toks[0]);
        for (size_t i = 1; i < toks.size(); i++) {
            if (!reason.empty()) reason += " ";
            reason += toks[i];
        }
    }
    std::string rerr;
    int64_t uid = resolve_admin_target(bot, m, user_args, &rerr);
    if (uid == 0) {
        reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /warn [user] [reason]"));
        return;
    }
    if (auto pr = protected_target(bot, uid); !pr.empty()) {
        reply(bot, m, styled(pr));
        return;
    }
    if (is_telegram_admin(bot, m.chat_id, uid)) {
        reply(bot, m, styled("You can't warn an admin."));
        return;
    }
    int count = db::add_warning(m.chat_id, uid, m.sender_id, reason);
    int limit = db::warn_limit(m.chat_id);
    std::string mode = db::warn_mode(m.chat_id);
    std::string text = "Warned " + mention_html(bot, uid) + " (" + std::to_string(count) + "/" +
                       std::to_string(limit) + ").";
    if (!reason.empty()) text += "\nReason: " + utils::escape_html(reason);
    if (count >= limit) {
        bool penalty_ok = true;
        std::string perr = "unknown error";
        if (mode == "mute") {
            penalty_ok = set_member_status(bot, m.chat_id, uid,
                                           {{"@type", "chatMemberStatusRestricted"},
                                            {"is_member", true},
                                            {"until_date", 0},
                                            {"permissions", {{"@type", "chatPermissions"}}}},
                                           &perr);
        } else if (mode == "kick") {
            penalty_ok = ban_user(bot, m.chat_id, uid, 0, &perr) && unban_user(bot, m.chat_id, uid);
        } else {
            penalty_ok = ban_user(bot, m.chat_id, uid, 0, &perr);
        }
        if (!penalty_ok) {
            text += "\nLimit reached, but penalty failed: " + utils::escape_html(perr);
        } else {
            db::clear_warnings(m.chat_id, uid);
            text += "\nLimit reached — user was " + mode + ".";
        }
    }
    reply(bot, m, styled(text));
}

void unwarn_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    std::string rerr;
    int64_t uid = resolve_admin_target(bot, m, fields(args_of(m)), &rerr);
    if (uid == 0) {
        reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /unwarn [user]"));
        return;
    }
    if (!db::remove_last_warning(m.chat_id, uid)) {
        reply(bot, m, styled(mention_html(bot, uid) + " has no warnings to remove."));
        return;
    }
    int count = db::count_warnings(m.chat_id, uid);
    int limit = db::warn_limit(m.chat_id);
    reply(bot, m, styled("Removed last warning from " + mention_html(bot, uid) + " (" +
                         std::to_string(count) + "/" + std::to_string(limit) + ")."));
}

void warns_handler(TdClient& bot, const Message& m) {
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return;
    }
    int64_t uid = m.sender_id;
    auto toks = fields(args_of(m));
    if (m.reply_to != 0 || !toks.empty()) {
        std::string rerr;
        int64_t resolved = resolve_admin_target(bot, m, toks, &rerr);
        if (resolved == 0) {
            reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /warns [user]"));
            return;
        }
        uid = resolved;
    }
    int count = db::count_warnings(m.chat_id, uid);
    int limit = db::warn_limit(m.chat_id);
    std::string mode = db::warn_mode(m.chat_id);
    std::string text = mention_html(bot, uid) + ": <b>" + std::to_string(count) + "/" +
                       std::to_string(limit) + "</b> warnings (mode: " + utils::escape_html(mode) + ")";
    auto rows = db::warnings_list(m.chat_id, uid);
    for (size_t i = 0; i < rows.size() && i < 8; i++) {
        std::string reason = rows[i].reason.empty() ? "no reason" : rows[i].reason;
        text += "\n• " + utils::escape_html(reason);
    }
    if (rows.size() > 8) {
        text += "\n...and " + std::to_string(rows.size() - 8) + " more";
    }
    reply(bot, m, styled(text));
}

void clear_warn_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    std::string rerr;
    int64_t uid = resolve_admin_target(bot, m, fields(args_of(m)), &rerr);
    if (uid == 0) {
        reply(bot, m, styled("Reply to a user or pass @username / id.\nUsage: /clearwarn [user]"));
        return;
    }
    int n = db::clear_warnings(m.chat_id, uid);
    reply(bot, m, styled("Cleared " + std::to_string(n) + " warning(s) from " + mention_html(bot, uid) + "."));
}

void reset_warns_handler(TdClient& bot, const Message& m) {
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return;
    }
    if (!require_right(bot, m, "restrict members", false)) return;
    int n = db::clear_all_warnings(m.chat_id);
    reply(bot, m, styled("Cleared " + std::to_string(n) + " warning(s) in this chat."));
}

void warn_limit_handler(TdClient& bot, const Message& m) {
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return;
    }
    if (!require_right(bot, m, "restrict members", false)) return;
    auto toks = fields(args_of(m));
    if (toks.empty()) {
        reply(bot, m, styled("Warn limit: <b>" + std::to_string(db::warn_limit(m.chat_id)) +
                             "</b>\nMode: <b>" + utils::escape_html(db::warn_mode(m.chat_id)) +
                             "</b>\nUsage: /warnlimit [2-20]"));
        return;
    }
    int n = utils::parse_int(toks[0], -1);
    if (n < 2 || n > 20) {
        reply(bot, m, styled("Warn limit must be a number from 2 to 20."));
        return;
    }
    db::set_warn_limit(m.chat_id, n);
    reply(bot, m, styled("Warn limit set to <b>" + std::to_string(n) + "</b>."));
}

void set_warn_mode_handler(TdClient& bot, const Message& m) {
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return;
    }
    if (!require_right(bot, m, "restrict members", false)) return;
    auto toks = fields(args_of(m));
    if (toks.empty()) {
        reply(bot, m, styled("Usage: /setwarnmode ban|mute|kick"));
        return;
    }
    std::string mode = utils::to_lower(toks[0]);
    if (mode != "ban" && mode != "mute" && mode != "kick") {
        reply(bot, m, styled("Warn mode must be ban, mute, or kick."));
        return;
    }
    db::set_warn_mode(m.chat_id, mode);
    reply(bot, m, styled("Warn mode set to <b>" + mode + "</b>."));
}

// ── pins / purge ────────────────────────────────────────────────────────────
void pin_handler(TdClient& bot, const Message& m) {
    if (!require_pin_right(bot, m)) return;
    if (m.reply_to == 0) {
        reply(bot, m, styled("Reply to a message to pin it.\nUsage: /pin [silent|quiet|s]"));
        return;
    }
    bool silent = false;
    for (auto& t : fields(args_of(m))) {
        auto l = utils::to_lower(t);
        if (l == "silent" || l == "quiet" || l == "s") silent = true;
    }
    auto resp = bot.call_sync({{"@type", "pinChatMessage"},
                               {"chat_id", m.chat_id},
                               {"message_id", m.reply_to},
                               {"disable_notification", silent}});
    if (resp.is_null()) {
        reply(bot, m, styled("Failed to pin."));
        return;
    }
    reply(bot, m, styled("Message pinned."));
}

void unpin_handler(TdClient& bot, const Message& m) {
    if (!require_pin_right(bot, m)) return;
    nlohmann::json q = {{"@type", "unpinChatMessage"}, {"chat_id", m.chat_id}};
    if (m.reply_to != 0) q["message_id"] = m.reply_to;
    if (bot.call_sync(q).is_null()) {
        reply(bot, m, styled("Failed to unpin."));
        return;
    }
    reply(bot, m, styled("Message unpinned."));
}

void unpin_all_handler(TdClient& bot, const Message& m) {
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return;
    }
    if (!require_pin_right(bot, m)) return;
    if (bot.call_sync({{"@type", "unpinAllChatMessages"}, {"chat_id", m.chat_id}}).is_null()) {
        reply(bot, m, styled("Failed to unpin."));
        return;
    }
    reply(bot, m, styled("All pinned messages were unpinned."));
}

// collectPurgeIDs parity: page back with getChatHistory; on error only the two
// boundary ids are deleted.
std::vector<int64_t> collect_purge_ids(TdClient& bot, int64_t chat_id, int64_t start_id, int64_t end_id) {
    std::vector<int64_t> ids{start_id, end_id};
    int64_t cursor = end_id;
    for (int i = 0; i < 20; i++) {
        auto page = bot.call_sync({{"@type", "getChatHistory"},
                                   {"chat_id", chat_id},
                                   {"from_message_id", cursor},
                                   {"offset", 0},
                                   {"limit", 100}});
        if (page.is_null() || !page.contains("messages") || !page["messages"].is_array() ||
            page["messages"].empty())
            break;
        bool hit_start = false;
        int64_t next = cursor;
        for (auto& msg : page["messages"]) {
            int64_t id = jint(msg, "id");
            if (id == 0) continue;
            next = id;
            if (id < start_id) {
                hit_start = true;
                continue;
            }
            if (id > end_id) continue;
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        if (hit_start || next == cursor) break;
        cursor = next;
    }
    return ids;
}

void purge_handler(TdClient& bot, const Message& m) {
    if (!require_delete_right(bot, m)) return;
    int64_t start_id = m.reply_to;
    if (start_id == 0) {
        reply(bot, m, styled("Reply to the message you want to purge from."));
        return;
    }
    int64_t end_id = m.id;
    if (end_id <= start_id) {
        reply(bot, m, styled("Nothing to purge."));
        return;
    }
    auto ids = collect_purge_ids(bot, m.chat_id, start_id, end_id);
    int deleted = 0;
    for (size_t i = 0; i < ids.size(); i += 100) {
        size_t n = std::min<size_t>(100, ids.size() - i);
        json chunk = json::array();
        for (size_t k = 0; k < n; k++) chunk.push_back(ids[i + k]);
        auto resp = bot.call_sync(
            {{"@type", "deleteMessages"}, {"chat_id", m.chat_id}, {"message_ids", chunk}, {"revoke", true}});
        if (!resp.is_null()) deleted += static_cast<int>(n);
    }
    bot.send_text_sync(m.chat_id, styled("Purged " + std::to_string(deleted) + " message(s)."));
    ADVAIT_LOG_INFO("group_admin",
                    "purged " + std::to_string(deleted) + " messages in chat " + std::to_string(m.chat_id));
}

}  // namespace

void init_group_admin(tg::TdClient& bot) {
    bot.on_command("promote", [&bot](const tg::Message& m) { promote_handler(bot, m); });
    bot.on_command("fullpromote", [&bot](const tg::Message& m) { full_promote_handler(bot, m); });
    bot.on_command("demote", [&bot](const tg::Message& m) { demote_handler(bot, m); });
    bot.on_command("unpromote", [&bot](const tg::Message& m) { demote_handler(bot, m); });
    bot.on_command("ban", [&bot](const tg::Message& m) { ban_handler(bot, m); });
    bot.on_command("unban", [&bot](const tg::Message& m) { unban_handler(bot, m); });
    bot.on_command("kick", [&bot](const tg::Message& m) { kick_handler(bot, m); });
    bot.on_command("warn", [&bot](const tg::Message& m) { warn_handler(bot, m); });
    bot.on_command("unwarn", [&bot](const tg::Message& m) { unwarn_handler(bot, m); });
    bot.on_command("warns", [&bot](const tg::Message& m) { warns_handler(bot, m); });
    bot.on_command("warnings", [&bot](const tg::Message& m) { warns_handler(bot, m); });
    bot.on_command("clearwarn", [&bot](const tg::Message& m) { clear_warn_handler(bot, m); });
    bot.on_command("resetwarns", [&bot](const tg::Message& m) { reset_warns_handler(bot, m); });
    bot.on_command("warnlimit", [&bot](const tg::Message& m) { warn_limit_handler(bot, m); });
    bot.on_command("setwarnlimit", [&bot](const tg::Message& m) { warn_limit_handler(bot, m); });
    bot.on_command("setwarnmode", [&bot](const tg::Message& m) { set_warn_mode_handler(bot, m); });
    bot.on_command("pin", [&bot](const tg::Message& m) { pin_handler(bot, m); });
    bot.on_command("unpin", [&bot](const tg::Message& m) { unpin_handler(bot, m); });
    bot.on_command("unpinall", [&bot](const tg::Message& m) { unpin_all_handler(bot, m); });
    bot.on_command("purge", [&bot](const tg::Message& m) { purge_handler(bot, m); });
    ADVAIT_LOG_INFO("group_admin", "group admin commands registered");
}

}  // namespace advait::modules
