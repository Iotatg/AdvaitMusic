#include "advait/modules/monitor.hpp"

#include "advait/core/logger.hpp"
#include "advait/modules/common.hpp"
#include "advait/ubot/context.hpp"

#include <chrono>
#include <thread>

namespace advait::modules {
namespace {

using namespace std::chrono_literals;

// Go: roomHandle/updatePlaybackMessage status edit for the plain-card slot.
void set_status(RoomState* r, const std::string& text) {
    auto& bot = tg::bot();
    auto mid = r->status_msg.load();
    if (mid != 0) {
        if (r->status_photo.load()) {
            if (bot.edit_caption(r->chat_id, mid, text, {}, true)) return;
        } else if (bot.edit_text(r->chat_id, mid, text, {}, nullptr, true)) {
            return;
        }
        bot.delete_message(r->chat_id, mid);
    }
    auto id = bot.send_text_sync(r->chat_id, text);
    if (id != 0) {
        r->status_msg = id;
        r->status_photo = false;
    }
}

std::atomic<bool> g_running{false};

}  // namespace

void advance_room(int64_t chat_id) {
    auto* r = room_for(chat_id);
    if (!r || r->destroyed()) return;
    try {
        // Loop mode repeats the current track (Go: loopCount decrement).
        if (r->loop() > 0) {
            r->set_loop(r->loop() - 1);
            if (r->replay()) {
                int64_t mid = r->status_msg.load();
                auto text = F(chat_id, "cb_replaying");
                if (mid == 0) {
                    auto id = tg::bot().send_text_sync(chat_id, text);
                    r->status_msg = id;
                    r->status_photo = false;
                } else if (r->status_photo.load()) {
                    tg::bot().edit_caption(chat_id, mid, text, {}, true);
                } else {
                    tg::bot().edit_text(chat_id, mid, text, {}, nullptr, true);
                }
            }
            return;
        }

        std::optional<Track> next;
        auto cand = r->next_track();
        if (cand) {
            next = cand;
        } else if (r->autoplay()) {
            auto last = r->track();
            if (last) {
                std::vector<Track> pool;
                for (auto& c : platforms::autoplay_tracks(*last, 5, r->played_history())) {
                    if (c.id.empty() && c.url.empty()) continue;
                    pool.push_back(c);
                }
                if (!pool.empty()) {
                    next = platforms::golden_zone_pick(pool);
                    next->mark_autoplay(F(chat_id, "autoplay_requester"), r->engine_token());
                }
            }
        }

        if (!next) {
            set_status(r, F(chat_id, "stream_queue_finished"));
            drop_room(chat_id);
            return;
        }

        set_status(r, F(chat_id, "stream_downloading_next"));
        std::string dl_err;
        auto path = platforms::download_track(*next, &dl_err);
        if (path.empty() && next->is_autoplay) {
            // Re-pick pass for autoplay on download failure, mirroring Go's
            // retry tiers (8 candidates this time, tried in relevance order).
            auto last = r->track();
            if (last) {
                for (auto& c : platforms::autoplay_tracks(*last, 8, r->played_history())) {
                    if (c.id.empty() && c.url.empty()) continue;
                    dl_err.clear();
                    path = platforms::download_track(c, &dl_err);
                    if (!path.empty()) {
                        c.mark_autoplay(F(chat_id, "autoplay_requester"), r->engine_token());
                        next = c;
                        break;
                    }
                }
            }
        }
        if (path.empty()) {
            set_status(r, F(chat_id, "stream_download_fail", {{"error", dl_err.empty() ? "download" : dl_err}}));
            drop_room(chat_id);
            return;
        }
        if (r->destroyed()) return;
        if (!r->play(*next, path, true)) {
            set_status(r, F(chat_id, "stream_play_fail"));
            drop_room(chat_id);
            return;
        }
        send_now_playing(chat_id, r, *next);
    } catch (const std::exception& e) {
        ADVAIT_LOG_ERROR("monitor", std::string("advance_room failed: ") + e.what());
    }
}

void start_room_monitor() {
    if (g_running.exchange(true)) return;
    std::thread([] {
        while (true) {
            std::this_thread::sleep_for(4s);
            try {
                for (auto& [id, r] : all_rooms()) {
                    if (!r || !r->active() || r->paused() || r->destroyed()) continue;
                    auto t = r->track();
                    if (!t) continue;
                    int64_t mid = r->status_msg.load();
                    if (mid == 0) continue;
                    int pos = r->position();
                    if (t->duration > 0 && pos >= t->duration) continue;
                    auto chat_id = r->chat_id;
                    auto text = now_playing_text(chat_id, *t);
                    auto mk = play_markup(chat_id, r, false);
                    if (!tg::bot().wait_sent(chat_id, mid, 200)) {
                        // Message id may still be remapping after a photo send;
                        // the edit APIs resolve remaps internally.
                    }
                    if (r->status_photo.load()) {
                        tg::bot().edit_caption(chat_id, mid, text, mk, true);
                    } else {
                        tg::bot().edit_text(chat_id, mid, text, mk, nullptr, true);
                    }
                }
            } catch (const std::exception& e) {
                ADVAIT_LOG_ERROR("monitor", std::string("room monitor: ") + e.what());
            }
        }
    }).detach();
    ADVAIT_LOG_INFO("monitor", "room monitor started (4s interval)");
}

void wire_stream_end(AssistantManager* assistants) {
    if (!assistants) return;
    for (int i = 0; i < assistants->count(); ++i) {
        auto* a = assistants->get(i);
        if (!a || !a->ntg || !a->ntg->binding()) continue;
        a->ntg->binding()->on_stream_end([](int64_t chat_id) {
            // ntg fires this on its own thread; move the follow-up work off it.
            std::thread([chat_id] { advance_room(chat_id); }).detach();
        });
    }
    ADVAIT_LOG_INFO("monitor", "stream-end handlers wired for assistants");
}

}  // namespace advait::modules
