#include "advait/modules/modules.hpp"
#include "advait/core/assistant.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/telegram/tdlib.hpp"

#include <sstream>

namespace advait::modules {
namespace {

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

void handle_queue(const tg::Message& m, bool cplay) {
    if (m.is_private || m.is_channel) {
        tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
        return;
    }
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    std::string room_err;
    auto* r = effective_room(m, cplay, &room_err);
    if (!r) {
        bot.reply_text(m, room_err.empty() ? F(chat_id, "queue_no_active") : room_err);
        return;
    }
    auto t = r->track();
    if (!r->active() || !t) {
        bot.reply_text(m, F(chat_id, "queue_no_active"));
        return;
    }

    std::ostringstream b;
    b << F(chat_id, "queue_header") << "\n\n";
    b << F(chat_id, "queue_now_playing") << "\n";
    b << "🎧 " << utils::escape_html(utils::short_title(t->title, 35))
      << " — " << t->requester << " [" << utils::format_duration(t->duration) << "]\n\n";

    auto queue = r->queue();
    auto q = static_cast<int>(queue.size());
    bool use_quote = q >= 3;
    if (q > 0) {
        b << F(chat_id, "queue_up_next");
        b << (use_quote ? "\n" : "\n\n");
        if (use_quote) b << "<blockquote>";
        for (int i = 0; i < q && i < 10; ++i) {
            auto& track = queue[static_cast<size_t>(i)];
            b << (i + 1) << ". 🎵 "
              << utils::escape_html(utils::short_title(track.title, 35)) << " — " << track.requester
              << " [" << utils::format_duration(track.duration) << "]\n";
        }
        if (use_quote) b << "</blockquote>";
        if (q > 10) {
            b << F(chat_id, "queue_more_line", {{"remaining", std::to_string(q - 10)}});
        }
    } else {
        b << F(chat_id, "queue_empty_tail");
    }
    bot.reply_text(m, b.str());
}

}  // namespace advait::modules
