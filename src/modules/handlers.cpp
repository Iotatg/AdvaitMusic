#include "advait/modules/modules.hpp"
#include "advait/modules/admin_lists.hpp"
#include "advait/modules/admin_system.hpp"
#include "advait/modules/common.hpp"
#include "advait/modules/playlists.hpp"
#include "advait/modules/vc_control.hpp"
#include "advait/core/assistant.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/config.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/modules/monitor.hpp"
#include "advait/platforms/platforms.hpp"
#include "advait/telegram/tdlib.hpp"

namespace advait::modules {
namespace {

void handle_close(const tg::CallbackQuery& cb) {
    tg::bot().answer_callback(cb.query_id);
    tg::bot().delete_message(cb.chat_id, cb.message_id);
}

void handle_cancel(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    if (!db::is_authorized(cb.chat_id, cb.user_id) && !db::is_sudo(cb.user_id)) {
        bot.answer_callback(cb.query_id, F(cb.chat_id, "only_admin_or_auth_cb"), true);
        return;
    }
    if (cancel_download(cb.chat_id)) {
        bot.answer_callback(cb.query_id, F(cb.chat_id, "download_cancelled"), true);
    } else {
        bot.answer_callback(cb.query_id, F(cb.chat_id, "no_download_to_cancel"), true);
    }
}

}  // namespace

namespace {

std::string cb_mention(const tg::CallbackQuery& cb) {
    return utils::html_mention("user", cb.user_id);
}

tg::Message cb_as_message(const tg::CallbackQuery& cb) {
    tg::Message m;
    m.chat_id = cb.chat_id;
    m.id = cb.message_id;
    return m;
}

// Go: checkAdminOrAuth in roomHandle (owner/sudo, chat auth users; admins-only
// mode changes the alert text). Must run before the press flood is charged.
bool room_cb_authorized(const tg::CallbackQuery& cb) {
    auto owner = Config::get().owner_id;
    if ((owner != 0 && cb.user_id == owner) || db::is_sudo(cb.user_id) ||
        db::is_authorized(cb.chat_id, cb.user_id))
        return true;
    auto key = db::play_mode_admins_only(cb.chat_id) ? "only_admin_cb" : "only_admin_or_auth_cb";
    tg::bot().answer_callback(cb.query_id, F(cb.chat_id, key), true);
    return false;
}

bool room_flood_check(const tg::CallbackQuery& cb) {
    using clock = std::chrono::steady_clock;
    static std::mutex mu;
    static std::unordered_map<std::string, clock::time_point> until;
    auto key = std::to_string(cb.user_id) + ":" + std::to_string(cb.chat_id);
    std::lock_guard<std::mutex> g(mu);
    auto now = clock::now();
    auto it = until.find(key);
    if (it != until.end() && it->second > now) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(it->second - now).count() + 1;
        tg::bot().answer_callback(cb.query_id, F(cb.chat_id, "flood_seconds", {{"duration", std::to_string(secs)}}), true);
        return true;
    }
    until[key] = now + std::chrono::seconds(3);
    return false;
}

void update_playback_message(const tg::CallbackQuery& cb, RoomState* r, const char* state) {
    auto t = r->track();
    if (!t) return;
    auto safe_title = utils::escape_html(utils::short_title(t->title, 25));
    auto user = cb_mention(cb);
    std::string text;
    if (std::string(state) == "paused") {
        text = F(cb.chat_id, "cb_pause_message",
                 {{"url", t->url},
                  {"title", safe_title},
                  {"position", utils::format_duration(r->position())},
                  {"duration", utils::format_duration(t->duration)},
                  {"user", user}});
    } else if (std::string(state) == "muted") {
        text = F(cb.chat_id, "cb_mute_message",
                 {{"url", t->url}, {"title", safe_title}, {"user", user}});
    } else {
        text = F(cb.chat_id, "cb_resume_message",
                 {{"url", t->url},
                  {"title", safe_title},
                  {"duration", utils::format_duration(t->duration)},
                  {"user", user}});
    }
    bool photo_card = cb.message_id == r->status_msg.load() && r->status_photo.load();
    card_edit(cb.chat_id, cb.message_id, text, play_markup(cb.chat_id, r, false), photo_card);
    }

}  // namespace

void handle_room_cb(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    auto chat_id = cb.chat_id;
    auto data = cb.data;
    auto p1 = data.find(':');
    auto p2 = data.find(':', p1 == std::string::npos ? p1 : p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) {
    bot.answer_callback(cb.query_id, F(chat_id, "invalid_request"), true);
    bot.delete_message(chat_id, cb.message_id);
    return;
    }
    auto room_id_str = data.substr(p1 + 1, p2 - p1 - 1);
    auto action = data.substr(p2 + 1);
    int64_t room_id = 0;
    try {
    room_id = std::stoll(room_id_str);
    } catch (...) {
    bot.answer_callback(cb.query_id, F(chat_id, "invalid_request"), true);
    bot.delete_message(chat_id, cb.message_id);
    return;
    }
    auto* r = room_for(room_id);
    if (!r || !r->active()) {
    bot.answer_callback(cb.query_id, F(chat_id, "room_not_active_cb"), true);
    card_edit(chat_id, cb.message_id, F(chat_id, "room_no_active"));
    return;
    }
    if (!room_cb_authorized(cb)) return;
    if (room_flood_check(cb)) return;

    if (action.rfind("seek", 0) == 0) {
    bool is_back = action.rfind("seekback_", 0) == 0;
    auto num_part = action.substr(is_back ? 9 : 5);
    int seconds = 0;
    try {
        {
            size_t consumed = 0;
            seconds = std::stoi(num_part, &consumed);
            if (consumed != num_part.size()) throw std::invalid_argument("trailing");
        }
    } catch (...) {
        bot.answer_callback(cb.query_id, F(chat_id, "invalid_request"), true);
        return;
    }
    auto t = r->track();
    if (!r->active() || !t) {
        bot.answer_callback(cb.query_id, F(chat_id, "room_not_active_cb"), true);
        return;
    }
    if (is_back) {
        if (r->position() <= seconds) r->seek(-r->position());
        else r->seek(-seconds);
        bot.answer_callback(cb.query_id, F(chat_id, "cb_seekback_success", {{"seconds", std::to_string(seconds)}}), true);
        bot.reply_text(cb_as_message(cb),
                       F(chat_id, "cb_seekback_edited",
                         {{"seconds", std::to_string(seconds)}, {"user", cb_mention(cb)}}));
    } else {
        if ((t->duration - r->position()) <= seconds) {
            bot.answer_callback(cb.query_id, F(chat_id, "cb_seek_near_end", {{"seconds", std::to_string(seconds)}}), true);
            return;
        }
        r->seek(seconds);
        bot.answer_callback(cb.query_id, F(chat_id, "cb_seek_success", {{"seconds", std::to_string(seconds)}}), true);
        bot.reply_text(cb_as_message(cb),
                       F(chat_id, "cb_seek_edited",
                         {{"seconds", std::to_string(seconds)}, {"user", cb_mention(cb)}}));
    }
    return;
    }
    if (action == "pause") {
    if (r->paused()) {
        bot.answer_callback(cb.query_id, F(chat_id, "room_already_paused"), true);
        return;
    }
    if (!r->pause()) {
        bot.answer_callback(cb.query_id, F(chat_id, "room_pause_failed", {{"error", "pause"}}), true);
        return;
    }
    if (r->muted()) r->unmute();
    bot.answer_callback(cb.query_id,
                        F(chat_id, "cb_pause_success", {{"position", utils::format_duration(r->position())}}), true);
    update_playback_message(cb, r, "paused");
    return;
    }
    if (action == "resume") {
    if (!r->paused()) {
        bot.answer_callback(cb.query_id, F(chat_id, "cb_already_playing"), true);
        return;
    }
    if (!r->resume()) {
        bot.answer_callback(cb.query_id, F(chat_id, "cb_resume_failed"), true);
        return;
    }
    bot.answer_callback(cb.query_id,
                        F(chat_id, "cb_resume_success", {{"position", utils::format_duration(r->position())}}), true);
    update_playback_message(cb, r, "playing");
    return;
    }
    if (action == "replay") {
    bot.reply_text(cb_as_message(cb), F(chat_id, "cb_replaying"));
    if (!r->replay()) {
        bot.answer_callback(cb.query_id, F(chat_id, "cb_replay_failed"), true);
        return;
    }
    send_now_playing(chat_id, r, *r->track());
    bot.answer_callback(cb.query_id, F(chat_id, "cb_replay_success"), true);
    card_edit(chat_id, cb.message_id, F(chat_id, "cb_replay_edited", {{"user", cb_mention(cb)}}), {},
              cb.message_id == r->status_msg.load() && r->status_photo.load());
    return;
    }
    if (action == "skip") {
    if (r->queue().empty()) {
        drop_room(r->id);
        card_edit(chat_id, cb.message_id, F(chat_id, "skip_stopped", {{"user", cb_mention(cb)}}));
        bot.answer_callback(cb.query_id, F(chat_id, "cb_skip_queue_empty"), true);
        return;
    }
    r->set_loop(0);
    auto t = r->next_track();
    if (!t) {
        drop_room(r->id);
        card_edit(chat_id, cb.message_id, F(chat_id, "skip_stopped", {{"user", cb_mention(cb)}}));
        bot.answer_callback(cb.query_id, F(chat_id, "cb_skip_queue_empty"), true);
        return;
    }
    bot.reply_text(cb_as_message(cb), F(chat_id, "stream_downloading_next"));
    std::string dl_err;
    auto path = platforms::download_track(*t, &dl_err);
    if (path.empty()) {
        bot.answer_callback(cb.query_id, F(chat_id, "cb_skip_download_failed"), true);
        drop_room(r->id);
        return;
    }
    if (!r->play(*t, path, true)) {
        bot.answer_callback(cb.query_id, F(chat_id, "cb_skip_play_failed"), true);
        drop_room(r->id);
        return;
    }
    bot.answer_callback(cb.query_id, F(chat_id, "cb_skip_success"), true);
    bot.delete_message(chat_id, cb.message_id);
    auto nid = send_now_playing(chat_id, r, *t);
    tg::Message ref;
    ref.chat_id = chat_id;
    ref.id = nid != 0 ? nid : cb.message_id;
    bot.reply_text(ref, F(chat_id, "cb_skip_edited", {{"user", cb_mention(cb)}}));
    return;
    }
    if (action == "stop") {
    drop_room(r->id);
    bot.answer_callback(cb.query_id, F(chat_id, "cb_stop_success"), true);
    card_edit(chat_id, cb.message_id, F(chat_id, "stopped", {{"user", cb_mention(cb)}}));
    return;
    }
    if (action == "mute") {
    if (r->muted()) {
        bot.answer_callback(cb.query_id, F(chat_id, "mute_already_muted"), true);
        return;
    }
    if (!r->mute()) {
        bot.answer_callback(cb.query_id, F(chat_id, "mute_failed", {{"error", "mute"}}), true);
        return;
    }
    bot.answer_callback(cb.query_id, F(chat_id, "cb_mute_success"), true);
    update_playback_message(cb, r, "muted");
    return;
    }
    if (action == "unmute") {
    if (!r->muted()) {
        bot.answer_callback(cb.query_id, F(chat_id, "unmute_already"), true);
        return;
    }
    if (!r->unmute()) {
        bot.answer_callback(cb.query_id, F(chat_id, "unmute_failed", {{"error", "unmute"}}), true);
        return;
    }
    bot.answer_callback(cb.query_id, F(chat_id, "cb_unmute_success"), true);
    update_playback_message(cb, r, "playing");
    return;
    }
    if (action == "autoplay_toggle") {
    bool enabled = !r->autoplay();
    r->set_autoplay(enabled);
    db::set_autoplay(r->chat_id, enabled);
    bot.answer_callback(cb.query_id, F(chat_id, enabled ? "autoplay_enabled" : "autoplay_disabled"));
    bot.edit_markup(chat_id, cb.message_id, play_markup(chat_id, r, false));
    return;
    }
    bot.answer_callback(cb.query_id, F(chat_id, "unknown_action"), true);
}

void init(tg::TdClient& bot, AssistantManager* assistants) {
    g_assistants = assistants;
    platforms::init_platforms();

    bot.on_command("start", handle_start);
    bot.on_command("help", handle_help);
    bot.on_command("ping", handle_ping);
    bot.on_command("play", [](const tg::Message& m) { handle_play(m, false, false, false); });
    bot.on_command("fplay", [](const tg::Message& m) { handle_play(m, true, false, false); });
    bot.on_command("playforce", [](const tg::Message& m) { handle_play(m, true, false, false); });
    bot.on_command("cplay", [](const tg::Message& m) { handle_play(m, false, true, false); });
    bot.on_command("cfplay", [](const tg::Message& m) { handle_play(m, true, true, false); });
    bot.on_command("fcplay", [](const tg::Message& m) { handle_play(m, true, true, false); });
    bot.on_command("cforceplay", [](const tg::Message& m) { handle_play(m, true, true, false); });
    bot.on_command("cplayforce", [](const tg::Message& m) { handle_play(m, true, true, false); });
    bot.on_command("vplay", [](const tg::Message& m) { handle_play(m, false, false, true); });
    bot.on_command("fvplay", [](const tg::Message& m) { handle_play(m, true, false, true); });
    bot.on_command("vfplay", [](const tg::Message& m) { handle_play(m, true, false, true); });
    bot.on_command("vplayforce", [](const tg::Message& m) { handle_play(m, true, false, true); });
    bot.on_command("vcplay", [](const tg::Message& m) { handle_play(m, false, true, true); });
    bot.on_command("cvplay", [](const tg::Message& m) { handle_play(m, false, true, true); });
    bot.on_command("fvcplay", [](const tg::Message& m) { handle_play(m, true, true, true); });
    bot.on_command("vcplayforce", [](const tg::Message& m) { handle_play(m, true, true, true); });
    bot.on_command("skip", [](const tg::Message& m) { handle_skip(m, false); });
    bot.on_command("cskip", [](const tg::Message& m) { handle_skip(m, true); });
    bot.on_command("pause", [](const tg::Message& m) { handle_pause(m, false); });
    bot.on_command("cpause", [](const tg::Message& m) { handle_pause(m, true); });
    bot.on_command("resume", [](const tg::Message& m) { handle_resume(m, false); });
    bot.on_command("cresume", [](const tg::Message& m) { handle_resume(m, true); });
    bot.on_command("queue", [](const tg::Message& m) { handle_queue(m, false); });
    bot.on_command("cqueue", [](const tg::Message& m) { handle_queue(m, true); });
    bot.on_command("stop", [](const tg::Message& m) { handle_stop(m, false); });
    bot.on_command("end", [](const tg::Message& m) { handle_stop(m, false); });
    bot.on_command("cstop", [](const tg::Message& m) { handle_stop(m, true); });
    bot.on_command("cend", [](const tg::Message& m) { handle_stop(m, true); });

    bot.on_callback("start", handle_start_cb);
    bot.on_callback("help_cb", handle_help_cb);
    bot.on_callback("help:", handle_help_page_cb);
    bot.on_callback("room:", handle_room_cb);
    bot.on_callback("close", handle_close);
    bot.on_callback("cancel", handle_cancel);

    init_vc_control(bot);
    init_admin_lists(bot);
    init_admin_system(bot);
    init_playlists(bot);

    start_room_monitor();
    wire_stream_end(assistants);

    ADVAIT_LOG_INFO("modules", "handlers registered");
}

}  // namespace advait::modules
