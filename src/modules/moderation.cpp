// Welcome system, notes and filters — port of Aishamusic-2
// modules/group_moderation.go (welcome/save/get/notes/clearnote/filter family)
// plus the #hashtag and filter message hook (handleGroupTextExtras) and the
// updateChatMember welcome trigger (maybeWelcomeMember).
#include "advait/modules/moderation.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "advait/core/assistant.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/modules/gutil.hpp"

namespace advait::modules {
namespace {
using namespace advait::modules::gutil;
using tg::Message;
using tg::TdClient;

// welcomeText parity: "<b>Welcome</b> {user} to {title}".
std::string welcome_text(TdClient& bot, int64_t chat_id, int64_t user_id) {
    return styled("<b>Welcome</b> " + mention_html(bot, user_id) + " to " +
                  utils::escape_html(chat_title(bot, chat_id)));
}

// ── /welcome on|off|preview ────────────────────────────────────────────────
void welcome_handler(TdClient& bot, const Message& m) {
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return;
    }
    auto toks = fields(args_of(m));
    if (toks.empty()) {
        std::string state = db::welcome_on(m.chat_id) ? "ON" : "OFF";
        reply(bot, m, styled("Welcome is currently <b>" + state +
                             "</b>.\n/welcome on — enable\n/welcome off — disable\n/welcome "
                             "preview — show it"));
        return;
    }
    auto sub = utils::to_lower(toks[0]);
    if (sub == "on") {
        if (!require_right(bot, m, "restrict members", false)) return;
        db::set_welcome_on(m.chat_id, true);
        reply(bot, m, styled("Welcome enabled."));
    } else if (sub == "off") {
        if (!require_right(bot, m, "restrict members", false)) return;
        db::set_welcome_on(m.chat_id, false);
        reply(bot, m, styled("Welcome disabled."));
    } else if (sub == "preview") {
        reply(bot, m, welcome_text(bot, m.chat_id, m.sender_id));
    } else {
        reply(bot, m, styled("Usage: /welcome on|off|preview"));
    }
}

// ── notes ──────────────────────────────────────────────────────────────────
std::string note_key_from_message(const Message& m) {
    auto t = utils::trim(m.text);
    if (!t.empty() && t[0] == '#') {
        auto tks = fields(t.substr(1));
        if (tks.empty()) return {};
        return utils::to_lower(tks[0]);
    }
    auto toks = fields(args_of(m));
    if (toks.empty()) return {};
    return utils::to_lower(toks[0]);
}

void save_note_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    auto toks = fields(args_of(m));
    if (toks.empty()) {
        reply(bot, m, styled("Usage: /save <name> <content>\nOr reply: /save <name>"));
        return;
    }
    std::string key = utils::to_lower(toks[0]);
    std::string content = utils::trim(args_after_first(m));
    if (content.empty() && m.reply_to != 0) {
        auto rep = bot.get_replied_message(m.chat_id, m.id);
        if (rep.is_object()) content = utils::trim(quoted_text(rep));
    }
    if (content.empty()) {
        reply(bot, m, styled("Provide content or reply to a message."));
        return;
    }
    db::set_note(m.chat_id, key, content);
    reply(bot, m, styled("Note <b>" + utils::escape_html(key) + "</b> saved."));
}

void get_note_handler(TdClient& bot, const Message& m) {
    if (m.is_private) return;
    std::string key = note_key_from_message(m);
    if (key.empty()) {
        reply(bot, m, styled("Usage: /get <name> or #name"));
        return;
    }
    std::string content = db::get_note(m.chat_id, key);
    if (content.empty()) {
        reply(bot, m, styled("No note named <b>" + utils::escape_html(key) + "</b>."));
        return;
    }
    reply(bot, m, styled("<b>" + utils::escape_html(key) + "</b>\n\n" + utils::escape_html(content)));
}

void notes_handler(TdClient& bot, const Message& m) {
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return;
    }
    auto names = db::list_notes(m.chat_id);
    if (names.empty()) {
        reply(bot, m, styled("No notes saved."));
        return;
    }
    std::string text = "<b>Notes (" + std::to_string(names.size()) + ")</b>";
    for (auto& n : names) text += "\n• #" + utils::escape_html(n);
    text += "\n\nUse #name or /get name";
    reply(bot, m, styled(text));
}

void clear_note_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    auto toks = fields(args_of(m));
    if (toks.empty()) {
        reply(bot, m, styled("Usage: /clearnote <name>"));
        return;
    }
    std::string key = utils::to_lower(toks[0]);
    if (!db::delete_note(m.chat_id, key)) {
        reply(bot, m, styled("No note named <b>" + utils::escape_html(key) + "</b>."));
        return;
    }
    reply(bot, m, styled("Note <b>" + utils::escape_html(key) + "</b> deleted."));
}

// ── filters ────────────────────────────────────────────────────────────────
void filter_save_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    auto toks = fields(args_of(m));
    if (toks.empty()) {
        reply(bot, m, styled("Usage: /filter <keyword> <reply>\nOr reply to a message: /filter <keyword>"));
        return;
    }
    std::string keyword = toks[0];
    std::string text = utils::trim(args_after_first(m));
    if (text.empty() && m.reply_to != 0) {
        auto rep = bot.get_replied_message(m.chat_id, m.id);
        if (rep.is_object()) text = utils::trim(quoted_text(rep));
    }
    if (text.empty()) {
        reply(bot, m, styled("Provide reply text or reply to a message."));
        return;
    }
    db::save_filter(m.chat_id, keyword, text);
    reply(bot, m, styled("Filter saved for <code>" + utils::escape_html(utils::to_lower(keyword)) + "</code>."));
}

void filters_list_handler(TdClient& bot, const Message& m) {
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return;
    }
    auto rows = db::list_filters(m.chat_id);
    if (rows.empty()) {
        reply(bot, m, styled("No filters set.\nAdd one with /filter <keyword> <reply>"));
        return;
    }
    std::string text = "<b>Filters (" + std::to_string(rows.size()) + ")</b>";
    for (auto& r : rows) text += "\n• <code>" + utils::escape_html(r.keyword) + "</code>";
    text += "\n\nDelete with /stopfilter <keyword>";
    reply(bot, m, styled(text));
}

void stop_filter_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    auto toks = fields(args_of(m));
    if (toks.empty()) {
        reply(bot, m, styled("Usage: /stopfilter <keyword>"));
        return;
    }
    if (!db::delete_filter(m.chat_id, toks[0])) {
        reply(bot, m, styled("No filter named <code>" + utils::escape_html(toks[0]) + "</code>."));
        return;
    }
    reply(bot, m, styled("Filter removed: <code>" + utils::escape_html(utils::to_lower(toks[0])) + "</code>"));
}

void clear_filters_handler(TdClient& bot, const Message& m) {
    if (!require_right(bot, m, "restrict members", false)) return;
    if (m.is_private) {
        reply(bot, m, styled("This command can only be used in a group."));
        return;
    }
    int n = db::clear_filters(m.chat_id);
    reply(bot, m, styled("Removed " + std::to_string(n) + " filter(s) from this chat."));
}

// handleGroupTextExtras parity: #hashtag notes + keyword filters on plain
// group texts. Runs inline from the raw-message hook (DB-only, no network).
void group_text_extras(TdClient& bot, const Message& m) {
    if (m.is_private) return;
    auto t = utils::trim(m.text);
    if (t.empty()) return;
    bool is_command = t[0] == '/';
    if (!is_command && t[0] == '#') {
        auto tks = fields(t.substr(1));
        if (!tks.empty()) {
            std::string key = utils::to_lower(tks[0]);
            std::string content = db::get_note(m.chat_id, key);
            if (!content.empty()) {
                bot.send_text_sync(m.chat_id,
                                   styled("<b>" + utils::escape_html(key) + "</b>\n\n" +
                                          utils::escape_html(content)),
                                   {}, m.id, 15000);
                return;
            }
        }
    }
    if (is_command) return;
    db::FilterRow f;
    if (db::match_filter(m.chat_id, t, &f)) {
        bot.send_text_sync(m.chat_id, styled(f.text), {}, 0, 15000);
    }
}

// maybeWelcomeMember parity: fires from updateChatMember when a regular member
// becomes "member" after not being inside.
void on_chat_member(TdClient& bot, const json& update) {
    int64_t chat_id = jint(update, "chat_id");
    auto new_member = jobj(update, "new_chat_member");
    auto old_member = jobj(update, "old_chat_member");
    auto mid = jobj(new_member, "member_id");
    if (jstr(mid, "@type") != "messageSenderUser") return;
    int64_t user_id = jint(mid, "user_id");
    if (user_id == 0 || user_id == bot.me_id()) return;
    // Never greet our own assistant.
    if (g_assistants) {
        if (auto* a = g_assistants->for_chat(chat_id); a && a->user_id == user_id) return;
    }
    if (!db::welcome_on(chat_id)) return;
    auto status_of = [](const json& member) -> std::string {
        return jstr(jobj(member, "status"), "@type");
    };
    std::string old_status = status_of(old_member);
    std::string new_status = status_of(new_member);
    bool was_inside = old_status == "chatMemberStatusMember" || old_status == "chatMemberStatusAdministrator" ||
                      old_status == "chatMemberStatusCreator";
    bool now_inside = new_status == "chatMemberStatusMember";
    if (was_inside || !now_inside) return;
    bot.send_text(chat_id, welcome_text(bot, chat_id, user_id));
}

}  // namespace

void init_moderation(tg::TdClient& bot) {
    bot.on_command("welcome", [&bot](const tg::Message& m) { welcome_handler(bot, m); });
    bot.on_command("save", [&bot](const tg::Message& m) { save_note_handler(bot, m); });
    bot.on_command("get", [&bot](const tg::Message& m) { get_note_handler(bot, m); });
    bot.on_command("notes", [&bot](const tg::Message& m) { notes_handler(bot, m); });
    bot.on_command("clearnote", [&bot](const tg::Message& m) { clear_note_handler(bot, m); });
    bot.on_command("filter", [&bot](const tg::Message& m) { filter_save_handler(bot, m); });
    bot.on_command("filters", [&bot](const tg::Message& m) { filters_list_handler(bot, m); });
    bot.on_command("stopfilter", [&bot](const tg::Message& m) { stop_filter_handler(bot, m); });
    bot.on_command("clearfilters", [&bot](const tg::Message& m) { clear_filters_handler(bot, m); });
    bot.on_message([&bot](const tg::Message& m) { group_text_extras(bot, m); });
    bot.on_chat_member([&bot](const json& u) { on_chat_member(bot, u); });
    ADVAIT_LOG_INFO("moderation", "welcome/notes/filters registered");
}

}  // namespace advait::modules
