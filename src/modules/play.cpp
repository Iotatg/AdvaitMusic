#include "advait/modules/modules.hpp"
#include "advait/modules/chat_state.hpp"
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

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Flood-wait seconds as reported by Telegram, or 0 when not a flood wait.
int flood_wait_seconds(const std::string& err) {
    auto p = err.find("FLOOD_WAIT_");
    if (p == std::string::npos) return 0;
    int wait = 0;
    try {
        wait = std::stoi(err.substr(p + 11));
    } catch (...) {
        return 0;
    }
    return wait > 0 ? wait : 0;
}

std::string flood_wait_text(int64_t chat_id, int wait) {
    if (wait >= 60) {
        int minutes = wait / 60;
        return F(chat_id, "flood_minutes", {{"duration", std::to_string(minutes)}});
    }
    return F(chat_id, "flood_seconds", {{"duration", std::to_string(wait)}});
}

enum class PlayErrorAction { Retry, Stop };

// Mirrors the Go binding's handlePlayAttemptError: classify the failure so the
// user gets the specific message instead of a raw error dump.
PlayErrorAction handle_play_attempt_error(const std::string& err, int attempt, int64_t chat_id,
                                         const std::string& raw, LiveMsg& lm) {
    if (int wait = flood_wait_seconds(err); wait > 0) {
        ADVAIT_LOG_WARN("play", "FloodWait detected (" + std::to_string(wait) +
                                   "s). Retrying... (attempt " + std::to_string(attempt) + ")");
        lm.eor(flood_wait_text(chat_id, wait));
        std::this_thread::sleep_for(std::chrono::seconds(wait));
        return PlayErrorAction::Retry;
    }
    if (contains(err, "connection timeout") || contains(err, "no response from ntgcalls") ||
        contains(err, "timed out")) {
        ADVAIT_LOG_ERROR("play", "Voice connection timeout. Stopping call session...");
        lm.eor(F(chat_id, "err_connection_timeout"));
        drop_room(chat_id);
        return PlayErrorAction::Stop;
    }
    if (contains(err, "Streaming is not supported when using RTMP")) {
        ADVAIT_LOG_ERROR("play", "RTMP/live-stream voice chat detected, cannot play. Cleaning up...");
        drop_room(chat_id);
        lm.eor(F(chat_id, "rtmp_play_unsupported"));
        return PlayErrorAction::Stop;
    }
    if (contains(err, "group call") && contains(err, "is closed")) {
        ADVAIT_LOG_ERROR("play", "No active voice chat: " + err);
        lm.eor(F(chat_id, "err_no_active_voicechat"));
        return PlayErrorAction::Stop;
    }
    if (contains(err, "GROUPCALL_INVALID")) {
        ADVAIT_LOG_ERROR("play", "GROUPCALL_INVALID err occurred. Returning...");
        drop_room(chat_id);
        lm.eor(F(chat_id, "play_unable"));
        return PlayErrorAction::Stop;
    }
    if (contains(err, "INTERDC_X_CALL_ERROR")) {
        ADVAIT_LOG_WARN("play", "INTERDC_X_CALL_ERROR occurred. Retrying... (attempt " +
                                   std::to_string(attempt) + ")");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return PlayErrorAction::Retry;
    }
    (void)raw;
    return PlayErrorAction::Retry;
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
    ADVAIT_LOG_INFO("play", "resolve done: " + std::to_string(tracks.size()) + " track(s)");
    if (tracks.empty()) {
        lm.eor(F(chat_id, "no_song_found"));
        return;
    }

    // KanhaMusic fetchTracksAndCheckStatus order: after tracks resolve, make
    // sure the assistant is actually a chat member (auto-join / ban / admin
    // errors surface here) before anything is queued.
    auto guard_err = chat_state::ensure_voice_chat_ready(r, chat_id);
    ADVAIT_LOG_INFO("play", std::string("guard done: err=") + (guard_err.empty() ? "none" : guard_err));
    if (!guard_err.empty()) {
        lm.eor(guard_err);
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
        ADVAIT_LOG_INFO("play", "download start: " + first.title);
        std::string dl_err;
        path = download_with_status(lm, m.chat_id, first, &dl_err, dl_markup);
        ADVAIT_LOG_INFO("play", "download done: path=" + std::to_string(path.size()) + " err=" + dl_err);
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
    std::string play_err;
    for (int attempt = 1; attempt <= 3 && !played; ++attempt) {
        if (r->destroyed()) {
            ADVAIT_LOG_INFO("play", "Room destroyed during retry, aborting");
            return;
        }
        play_err.clear();
        played = r->play(first, was_downloaded ? path : std::string{}, force, &play_err);
        if (played) {
            if (attempt > 1) {
                ADVAIT_LOG_INFO("play", "Successfully played after retry attempt " +
                                           std::to_string(attempt));
            }
            break;
        }
        if (handle_play_attempt_error(play_err, attempt, chat_id, play_err, lm) ==
            PlayErrorAction::Stop) {
            return;
        }
    }
    if (!played) {
        // Nothing matched or retries ran out: report the underlying reason.
        std::string why = play_err.empty() ? "playback failed" : play_err;
        ADVAIT_LOG_ERROR("play", "❌ Failed to play after 3 attempts. Error: " + why);
        lm.eor(F(chat_id, "play_failed", {{"error", why}}));
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

    // `first` was already queued by the retry loop above when something was
    // playing; queueing it again here would add the same song twice.
    if (kept.size() == 1) {
        auto text = F(chat_id, "play_added_to_queue_single",
                      {{"index", std::to_string(r->queue().size())},
                       {"url", first.url},
                       {"title", utils::escape_html(utils::short_title(first.title, 25))},
                       {"duration", utils::format_duration(first.duration)},
                       {"by", mention}});
        // Queued reply only: edit the /play message, never the now-playing card.
        show_track_queued(lm, chat_id, r, text);
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
