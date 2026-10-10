#include "advait/modules/modules.hpp"
#include "advait/modules/common.hpp"
#include "advait/core/assistant.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/platforms/platforms.hpp"
#include "advait/telegram/tdlib.hpp"

namespace advait::modules {
namespace {

void send_now_playing(int64_t chat_id, RoomState* r, const Track& t) {
    auto& bot = tg::bot();
    auto text = now_playing_text(chat_id, t);
    auto mk = play_markup(chat_id, r, false);
    if (!t.artwork.empty() && !db::thumbnails_disabled(chat_id)) {
        bot.send_photo(chat_id, t.artwork, text, mk);
        return;
    }
    bot.send_text(chat_id, text, mk);
}

}  // namespace

void handle_skip(const tg::Message& m, bool cplay) {
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

    auto mention = mention_of(m);
    int skip_count = 1;
    auto args = m.text;
    auto sp = args.find(' ');
    if (sp != std::string::npos) {
        auto raw = utils::trim(args.substr(sp + 1));
        int parsed = 0;
        try {
            parsed = std::stoi(raw);
        } catch (...) {
            bot.reply_text(m, F(chat_id, "skip_invalid_number"));
            return;
        }
        int queued = static_cast<int>(r->queue().size());
        if (queued == 0) {
            bot.reply_text(m, F(chat_id, "skip_queue_empty_for_count"));
            return;
        }
        if (parsed < 1 || parsed > queued) {
            bot.reply_text(m, F(chat_id, "skip_count_exceeds_queue",
                                {{"requested", std::to_string(parsed)},
                                 {"available", std::to_string(queued)}}));
            return;
        }
        skip_count = parsed + 1;
    }

    if (r->queue().empty()) {
        drop_room(r->id);
        bot.reply_text(m, F(chat_id, "skip_stopped", {{"user", mention}}));
        return;
    }

    r->set_loop(0);
    for (int i = 1; i < skip_count; ++i) {
        if (r->queue().empty()) {
            drop_room(r->id);
            bot.reply_text(m, F(chat_id, "skip_stopped", {{"user", mention}}));
            return;
        }
        r->next_track();
    }
    if (r->queue().empty()) {
        drop_room(r->id);
        bot.reply_text(m, F(chat_id, "skip_stopped", {{"user", mention}}));
        return;
    }

    auto t = r->next_track();
    if (!t) {
        drop_room(r->id);
        bot.reply_text(m, F(chat_id, "skip_stopped", {{"user", mention}}));
        return;
    }

    LiveMsg lm;
    lm.send(F(chat_id, "stream_downloading_next"));
    std::string dl_err;
    auto path = download_with_status(lm, chat_id, *t, &dl_err);
    if (path.empty()) {
        lm.eor(F(chat_id, "stream_download_fail",
                 {{"error", dl_err.empty() ? "download" : dl_err}}));
        drop_room(r->id);
        return;
    }
    if (!r->play(*t, path, true)) {
        lm.eor(F(chat_id, "stream_play_fail"));
        drop_room(r->id);
        return;
    }
    lm.id = 0;
    show_track(lm, chat_id, r, *t, now_playing_text(chat_id, *t), false);
}

}  // namespace advait::modules
