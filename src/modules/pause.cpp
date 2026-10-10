#include "advait/modules/modules.hpp"
#include "advait/core/assistant.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/telegram/tdlib.hpp"

#include <cstdio>

namespace advait::modules {
namespace {

std::string mention_of(const tg::Message& m) {
    auto name = utils::trim(m.first_name + " " + m.last_name);
    return utils::html_mention(name.empty() ? "user" : name, m.sender_id);
}

RoomState* effective_room(const tg::Message& m, bool cplay, std::string* err) {
    int64_t chat_id = m.chat_id;
    int64_t orig = chat_id;
    if (cplay) {
        auto linked = db::linked_channel(chat_id);
        if (linked == 0) {
            if (err) *err = F(orig, "cplay_id_not_set");
            return nullptr;
        }
        chat_id = linked;
    }
    auto* r = room_for(chat_id);
    return r;
}

}  // namespace

void handle_pause(const tg::Message& m, bool cplay) {
    if (m.is_private || m.is_channel) {
        tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
        return;
    }
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    std::string room_err;
    auto* r = effective_room(m, cplay, &room_err);
    if (!r) {
        bot.reply_text(m, room_err.empty() ? F(chat_id, "room_no_active") : room_err);
        return;
    }
    if (!r->active()) {
        bot.reply_text(m, F(chat_id, "room_no_active"));
        return;
    }
    if (r->paused()) {
        bot.reply_text(m, F(chat_id, "pause_already"));
        return;
    }
    if (!r->pause()) {
        bot.reply_text(m, F(chat_id, "room_pause_failed", {{"error", "pause"}}));
        return;
    }
    auto t = r->track();
    auto title = t ? utils::escape_html(utils::short_title(t->title, 25)) : "";
    auto msg = F(chat_id, "pause_success",
                 {{"title", title},
                  {"position", utils::format_duration(r->position())},
                  {"duration", utils::format_duration(t ? t->duration : 0)},
                  {"user", mention_of(m)}});
    if (r->speed() != 1.0) {
        char sp[16];
        std::snprintf(sp, sizeof(sp), "%.2f", r->speed());
        msg += "\n" + F(chat_id, "speed_line", {{"speed", sp}});
    }
    bot.reply_text(m, msg);
}

void handle_resume(const tg::Message& m, bool cplay) {
    if (m.is_private || m.is_channel) {
        tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
        return;
    }
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    std::string room_err;
    auto* r = effective_room(m, cplay, &room_err);
    if (!r) {
        bot.reply_text(m, room_err.empty() ? F(chat_id, "room_no_active") : room_err);
        return;
    }
    if (!r->active()) {
        bot.reply_text(m, F(chat_id, "room_no_active"));
        return;
    }
    if (!r->paused()) {
        bot.reply_text(m, F(chat_id, "resume_already_playing"));
        return;
    }
    auto t = r->track();
    if (!r->resume()) {
        bot.reply_text(m, F(chat_id, "resume_failed", {{"error", "resume"}}));
        return;
    }
    auto title = t ? utils::escape_html(utils::short_title(t->title, 25)) : "";
    std::string speed_line;
    if (r->speed() != 1.0) {
        char sp[16];
        std::snprintf(sp, sizeof(sp), "%.2f", r->speed());
        speed_line = F(chat_id, "speed_line", {{"speed", sp}});
    }
    bot.reply_text(m, F(chat_id, "resume_success",
                        {{"title", title},
                         {"position", utils::format_duration(r->position())},
                         {"duration", utils::format_duration(t ? t->duration : 0)},
                         {"user", mention_of(m)},
                         {"speed_line", speed_line}}));
}

}  // namespace advait::modules
