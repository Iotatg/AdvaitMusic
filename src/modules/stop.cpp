#include "advait/modules/modules.hpp"
#include "advait/core/assistant.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/telegram/tdlib.hpp"

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
    return room_for(chat_id);
}

}  // namespace

void handle_stop(const tg::Message& m, bool cplay) {
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

    bool is_paused = r->paused();
    bool is_muted = r->muted();
    if (is_paused || is_muted) {
        auto key = is_muted ? "stop_confirm_muted" : "stop_confirm_paused";
        bot.reply_text(m, F(chat_id, key), stop_confirm_markup(chat_id, r, is_paused));
        return;
    }

    drop_room(r->id);
    bot.reply_text(m, F(chat_id, "stopped", {{"user", mention_of(m)}}));
}

}  // namespace advait::modules
