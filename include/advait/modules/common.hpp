#pragma once

#include "advait/core/buttons.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/models.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/platforms/platforms.hpp"
#include "advait/telegram/tdlib.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace advait::modules {

inline std::mutex& dl_mu() {
    static std::mutex m;
    return m;
}

// Active downloads by chat, so the /cancel button can abort them
// (Go: modules/download.go downloadManager).
inline std::unordered_map<int64_t, std::atomic<bool>*>& dl_map() {
    static std::unordered_map<int64_t, std::atomic<bool>*> m;
    return m;
}

inline bool cancel_download(int64_t chat_id) {
    std::lock_guard<std::mutex> g(dl_mu());
    auto it = dl_map().find(chat_id);
    if (it == dl_map().end()) return false;
    it->second->store(true);
    return true;
}

inline std::string mention_of(const tg::Message& m) {
    auto name = utils::trim(m.first_name + " " + m.last_name);
    return utils::html_mention(name.empty() ? "user" : name, m.sender_id);
}

// LiveMsg mirrors Go's utils.EOR: one status message that is created once and
// then edited in place. If an edit fails (message gone, temp-id race, etc.)
// the message is deleted and a fresh reply is sent, matching the semantics of
// advait/utils/edit_or_reply.go.
struct LiveMsg {
    int64_t chat_id = 0;
    int64_t id = 0;
    int64_t reply_to = 0;

    void reply(const tg::Message& m, const std::string& text, const nlohmann::json& markup = {}) {
        chat_id = m.chat_id;
        reply_to = m.id;
        id = tg::bot().send_text_sync(chat_id, text, markup, reply_to);
        if (id == 0) tg::bot().reply_text(m, text, markup);
    }

    void send(const std::string& text, const nlohmann::json& markup = {}) {
        id = tg::bot().send_text_sync(chat_id, text, markup, reply_to);
        if (id == 0) tg::bot().send_text(chat_id, text, markup);
    }

    void eor(const std::string& text, const nlohmann::json& markup = {}) {
        if (id != 0) {
            int64_t fid = 0;
            if (tg::bot().edit_text(chat_id, id, text, markup, &fid)) {
                if (fid != 0) id = fid;
                return;
            }
            tg::bot().delete_message(chat_id, id);
        }
        send(text, markup);
    }

    void remove() {
        if (id != 0) tg::bot().delete_message(chat_id, id);
        id = 0;
    }
};

inline RoomState* effective_room(const tg::Message& m, bool cplay, std::string* err) {
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
    if (!r) {
        if (err && !err->empty()) return nullptr;
    }
    if (cplay && r) r->chat_id = orig;
    return r;
}

inline std::string cmd_arg(const tg::Message& m) {
    auto sp = m.text.find(' ');
    if (sp == std::string::npos) return {};
    return utils::trim(m.text.substr(sp + 1));
}

inline std::string now_playing_text(int64_t chat_id, const Track& t) {
    return F(chat_id, "stream_now_playing",
             {{"url", t.url},
              {"title", utils::escape_html(utils::short_title(t.title, 25))},
              {"duration", utils::format_duration(t.duration)},
              {"by", t.requester}});
}

inline void send_now_playing(int64_t chat_id, RoomState* r, const Track& t) {
    auto& bot = tg::bot();
    auto text = now_playing_text(chat_id, t);
    auto mk = play_markup(chat_id, r, false);
    if (!t.artwork.empty() && !db::thumbnails_disabled(chat_id)) {
        bot.send_photo(chat_id, t.artwork, text, mk);
        return;
    }
    bot.send_text(chat_id, text, mk);
}

// Mirrors Go helpers.go showTrackMessage: edit the live message into the
// track card; artwork cards fall back to delete + send_photo (TDLib has no
// bot-side editMessageMedia).
inline void show_track(LiveMsg& lm, int64_t chat_id, RoomState* r, const Track& t,
                       const std::string& text, bool queued) {
    auto& bot = tg::bot();
    auto mk = play_markup(chat_id, r, queued);
    if (!t.artwork.empty() && !db::thumbnails_disabled(chat_id)) {
        lm.remove();
        bot.send_photo(chat_id, t.artwork, text, mk);
        return;
    }
    lm.eor(text, mk);
}

// Downloads the track while editing `lm` into a live progress message.
// Returns the file path, "" on failure/cancel (*err == "canceled" for cancels).
inline std::string download_with_status(LiveMsg& lm, int64_t chat_id, const Track& t, std::string* err,
                                        const nlohmann::json& markup = {}) {
    platforms::DownloadHooks hooks;
    {
        std::lock_guard<std::mutex> g(dl_mu());
        dl_map()[chat_id] = &hooks.cancel;
    }
    using clock = std::chrono::steady_clock;
    auto last_edit = clock::now() - std::chrono::seconds(10);
    hooks.progress = [&](double pct, double speed, long eta, double elapsed) {
        auto now = clock::now();
        bool done = pct >= 99.9;
        if (!done && now - last_edit < std::chrono::seconds(2)) return;
        last_edit = now;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f", pct);
        lm.eor(F(chat_id, "download_progress",
                 {{"percentage", std::string(buf)},
                  {"speed", utils::format_bytes(static_cast<int64_t>(speed)) + "/s"},
                  {"eta", utils::format_duration(static_cast<int>(eta))},
                  {"elapsed", utils::format_time(static_cast<int>(elapsed))}}),
               markup);
    };
    auto path = platforms::download_track(t, err, &hooks);
    {
        std::lock_guard<std::mutex> g(dl_mu());
        auto it = dl_map().find(chat_id);
        if (it != dl_map().end() && it->second == &hooks.cancel) dl_map().erase(it);
    }
    return path;
}

}  // namespace advait::modules
