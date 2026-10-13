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

// Edits a player-card message without knowing whether it is a photo or a plain
// text card. Caption edits go over HTTP (fast, works on photo cards, works on
// raw ids TDLib never saw); plain-text cards fall back to TDLib edit_text.
inline bool card_edit(int64_t chat_id, int64_t message_id, const std::string& text,
                      const nlohmann::json& markup = {}, bool expect_photo = false) {
    auto& bot = tg::bot();
    if (expect_photo) {
        if (bot.edit_caption(chat_id, message_id, text, markup, true)) return true;
        return bot.edit_text(chat_id, message_id, text, markup, nullptr, true);
    }
    if (bot.edit_caption(chat_id, message_id, text, markup, true)) return true;
    return bot.edit_text(chat_id, message_id, text, markup, nullptr, true);
}

inline void publish_card(RoomState* r, int64_t chat_id, const Track& t, const std::string& text,
                         const nlohmann::json& mk, int64_t* out_id, bool* out_photo) {
    auto& bot = tg::bot();
    int64_t id = 0;
    bool photo = false;
    if (!t.artwork.empty() && !db::thumbnails_disabled(chat_id)) {
        auto local = platforms::artwork_local_path(t.artwork);
        if (!local.empty()) {
            id = bot.send_photo_sync(chat_id, local, true, text, mk);
            photo = id != 0;
        }
    }
    if (id == 0) id = bot.send_text_sync(chat_id, text, mk);
    if (out_id) *out_id = id;
    if (out_photo) *out_photo = photo && id != 0;
}

inline int64_t send_now_playing(int64_t chat_id, RoomState* r, const Track& t) {
    int64_t old = r ? r->status_msg.load() : 0;
    auto text = now_playing_text(chat_id, t);
    auto mk = play_markup(chat_id, r, false);
    int64_t id = 0;
    bool photo = false;
    publish_card(r, chat_id, t, text, mk, &id, &photo);
    if (r) {
        r->status_msg = id;
        r->status_photo = photo;
        if (old != 0 && old != id) tg::bot().delete_message(chat_id, old);
    }
    return id;
}

// Mirrors Go helpers.go showTrackMessage: transform the live status message
// into the track card (photo card first, text fallback), and take ownership
// of the card slot on the room.
inline void show_track(LiveMsg& lm, int64_t chat_id, RoomState* r, const Track& t,
                       const std::string& text, bool queued) {
    auto& bot = tg::bot();
    int64_t old = r ? r->status_msg.load() : 0;
    auto mk = play_markup(chat_id, r, queued);
    int64_t id = 0;
    bool photo = false;
    publish_card(r, chat_id, t, text, mk, &id, &photo);
    if (id == 0) {
        lm.eor(text, mk);
        if (r) {
            r->status_msg = lm.id;
            r->status_photo = false;
        }
        return;
    }
    lm.remove();
    if (r) {
        r->status_msg = id;
        r->status_photo = photo;
        if (old != 0 && old != id) bot.delete_message(chat_id, old);
    }
}

// Mirrors Go helpers.go finalizePlayReply's single-queue branch: a queued reply
// edits the live /play message in place and leaves the room's now-playing card
// alone. Using show_track() here would publish a new card and delete the
// now-playing one, so the song actually playing would disappear from the chat.
inline void show_track_queued(LiveMsg& lm, int64_t chat_id, RoomState* r,
                              const std::string& text) {
    lm.eor(text, play_markup(chat_id, r, true));
}

// Downloads the track. KanhaMusic parity: iota downloads show a single static
// message (the caller edits it into lm before calling us) with no progress
// updates — the remote API never sends Content-Length, so a live percentage
// just sits at 0.0%. The cancel flag stays wired so /cancel still aborts.
// Returns the file path, "" on failure/cancel (*err == "canceled" for cancels).
inline std::string download_with_status(LiveMsg& lm, int64_t chat_id, const Track& t, std::string* err,
                                        const nlohmann::json& markup = {}) {
    (void)lm;
    (void)markup;
    platforms::DownloadHooks hooks;
    {
        std::lock_guard<std::mutex> g(dl_mu());
        dl_map()[chat_id] = &hooks.cancel;
    }
    auto path = platforms::download_track(t, err, &hooks);
    {
        std::lock_guard<std::mutex> g(dl_mu());
        auto it = dl_map().find(chat_id);
        if (it != dl_map().end() && it->second == &hooks.cancel) dl_map().erase(it);
    }
    return path;
}

}  // namespace advait::modules
