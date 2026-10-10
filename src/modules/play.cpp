#include "advait/modules/modules.hpp"
#include "advait/modules/common.hpp"
#include "advait/core/assistant.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/config.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/platforms/platforms.hpp"
#include "advait/telegram/tdlib.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace advait::modules {
namespace {

RoomState* play_room(const tg::Message& m, bool cplay, std::string* err) {
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
    Assistant* ass = g_assistants ? g_assistants->for_chat(chat_id) : nullptr;
    auto* r = create_room(chat_id, ass);
    if (cplay && r) r->chat_id = orig;
    return r;
}

std::string command_of(const tg::Message& m) {
    auto t = m.text;
    auto sp = t.find(' ');
    return sp == std::string::npos ? t : t.substr(0, sp);
}

std::string extract_query(const std::string& text) {
    auto sp = text.find(' ');
    if (sp == std::string::npos) return {};
    return utils::trim(text.substr(sp + 1));
}

bool track_in_queue(RoomState* r, const Track& t) {
    for (auto& q : r->queue()) {
        if (q.url == t.url || q.id == t.id) return true;
    }
    return false;
}

std::string skipped_tracks_text(int64_t chat_id, const std::vector<std::string>& skipped) {
    auto& cfg = Config::get();
    auto out = F(chat_id, "play_multiple_tracks_too_long_header",
                 {{"count", std::to_string(skipped.size())},
                  {"limit_mins", std::to_string(cfg.duration_limit / 60)}}) +
             "\n";
    for (size_t i = 0; i < skipped.size(); ++i) {
        if (i < 5) {
            out += F(chat_id, "play_multiple_tracks_too_long_item", {{"title", skipped[i]}}) + "\n";
            continue;
        }
        out += F(chat_id, "play_multiple_tracks_too_long_more",
                 {{"remaining", std::to_string(skipped.size() - i)}}) +
               "\n";
        break;
    }
    return out;
}

std::string multi_added_text(int64_t chat_id, int count, const std::string& mention,
                             int available_slots, int track_count) {
    auto& cfg = Config::get();
    auto out = F(chat_id, "play_added_multiple_header",
                 {{"count", std::to_string(count)}, {"user", mention}}) +
               "\n\n";
    if (cfg.queue_limit > 0 && available_slots <= track_count) {
        out += F(chat_id, "play_queue_limit_hint") + "\n";
    }
    out += F(chat_id, "play_queue_view_hint");
    return out;
}

}  // namespace

void handle_play(const tg::Message& m, bool force, bool cplay, bool video) {
    if (m.is_channel) return;
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;

    if (db::play_mode_admins_only(chat_id) && !db::is_authorized(chat_id, m.sender_id) &&
        !db::is_sudo(m.sender_id)) {
        bot.reply_text(m, F(chat_id, "playmode_restricted"));
        return;
    }

    std::string room_err;
    auto* r = play_room(m, cplay, &room_err);
    if (!r) {
        bot.reply_text(m, room_err.empty() ? F(chat_id, "play_unable") : room_err);
        return;
    }

    if (Config::get().queue_limit > 0 && static_cast<int>(r->queue().size()) >= Config::get().queue_limit) {
        bot.reply_text(m, F(chat_id, "queue_limit_reached",
                            {{"limit", std::to_string(Config::get().queue_limit)}}));
        return;
    }

    auto query = extract_query(m.text);
    if (query.empty() && m.reply_to == 0) {
        bot.reply_text(m, F(chat_id, "no_song_query", {{"cmd", command_of(m)}}));
        return;
    }

    LiveMsg lm;
    lm.reply(m, query.empty()
                    ? F(chat_id, "searching")
                    : F(chat_id, "searching_query", {{"query", utils::escape_html(query)}}));

    auto tracks = platforms::resolve_query(query, video);
    if (tracks.empty()) {
        lm.eor(F(chat_id, "no_song_found"));
        return;
    }

    if (tracks.size() == 1 && !force && track_in_queue(r, tracks[0])) {
        lm.eor(F(chat_id, "play_already_in_queue",
                 {{"title", utils::escape_html(utils::short_title(tracks[0].title, 35))}}));
        return;
    }

    bool is_active = r->active();
    if (Config::get().queue_limit == 0 && is_active && !force) {
        lm.eor(F(chat_id, "play_already_playing"));
        return;
    }

    auto& cfg = Config::get();
    std::vector<Track> kept;
    std::vector<std::string> skipped;
    for (auto t : tracks) {
        if (cfg.duration_limit > 0 && t.duration > cfg.duration_limit) {
            skipped.push_back(utils::escape_html(utils::short_title(t.title, 35)));
            continue;
        }
        kept.push_back(std::move(t));
    }
    if (!skipped.empty()) {
        if (tracks.size() == 1) {
            lm.eor(F(chat_id, "play_single_track_too_long",
                     {{"limit_mins", utils::format_duration(cfg.duration_limit)},
                      {"title", skipped[0]}}));
            return;
        }
        lm.eor(skipped_tracks_text(chat_id, skipped));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (kept.empty()) {
        lm.eor(F(chat_id, "play_all_tracks_skipped"));
        return;
    }

    int available_slots = static_cast<int>(kept.size());
    if (cfg.queue_limit >= 0) {
        available_slots = cfg.queue_limit - static_cast<int>(r->queue().size());
        if (available_slots < 0) available_slots = 0;
    }
    if (available_slots == 0 && !is_active) available_slots = 1;
    if (available_slots < static_cast<int>(kept.size())) {
        kept.resize(static_cast<size_t>(available_slots));
        ADVAIT_LOG_WARN("play", "Queue full - adding only " + std::to_string(available_slots) +
                                   " tracks out of requested.");
    }

    auto mention = mention_of(m);
    auto first = kept[0];
    first.requester = mention;
    first.video = video;

    std::string path;
    bool was_downloaded = false;
    if (!is_active || force) {
        std::string title = utils::escape_html(utils::short_title(first.title, 25));
        nlohmann::json dl_markup = {};
        if (first.duration > 600) dl_markup = cancel_keyboard(chat_id);
        lm.eor(F(chat_id, "play_downloading_song", {{"title", title}}), dl_markup);
        std::string dl_err;
        path = download_with_status(lm, m.chat_id, first, &dl_err, dl_markup);
        if (dl_err == "canceled") {
            lm.eor(F(chat_id, "play_download_canceled", {{"user", mention}}));
            return;
        }
        if (path.empty()) {
            ADVAIT_LOG_ERROR("play", "Download failed: " + dl_err);
            lm.eor(F(chat_id, "play_download_failed"));
            return;
        }
        was_downloaded = true;
        ADVAIT_LOG_INFO("play", "Downloaded track to " + path);
    }

    bool played = false;
    for (int attempt = 1; attempt <= 3 && !played; ++attempt) {
        played = r->play(first, was_downloaded ? path : std::string{}, force);
        if (!played && attempt < 3) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!played) {
        lm.eor(F(chat_id, "play_failed", {{"error", "native play"}}));
        return;
    }

    if (!is_active || force) {
        show_track(lm, m.chat_id, r, first, now_playing_text(m.chat_id, first), false);
        if (kept.size() > 1) {
            for (size_t i = 1; i < kept.size(); ++i) {
                kept[i].requester = mention;
                kept[i].video = video;
                r->play(kept[i], {}, false);
            }
            bot.send_text_sync(m.chat_id,
                               multi_added_text(m.chat_id, static_cast<int>(kept.size()) - 1, mention,
                                                available_slots, static_cast<int>(kept.size())),
                               {}, lm.id);
        }
        return;
    }

    r->play(first, {}, false);
    if (kept.size() == 1) {
        auto text = F(chat_id, "play_added_to_queue_single",
                      {{"index", std::to_string(r->queue().size())},
                       {"url", first.url},
                       {"title", utils::escape_html(utils::short_title(first.title, 25))},
                       {"duration", utils::format_duration(first.duration)},
                       {"by", mention}});
        show_track(lm, chat_id, r, first, text, true);
        return;
    }
    for (size_t i = 1; i < kept.size(); ++i) {
        kept[i].requester = mention;
        kept[i].video = video;
        r->play(kept[i], {}, false);
    }
    lm.eor(multi_added_text(chat_id, static_cast<int>(kept.size()), mention, available_slots,
                            static_cast<int>(kept.size())));
}

}  // namespace advait::modules
