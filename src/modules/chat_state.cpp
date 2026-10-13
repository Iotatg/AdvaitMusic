#include "advait/modules/chat_state.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "advait/core/assistant.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/telegram/tdlib.hpp"
#include "advait/ubot/context.hpp"

namespace advait::chat_state {
namespace {

// KanhaMusic isAdminError: these names mean the *bot* lacks admin rights rather
// than the assistant failing to join.
bool is_admin_error(const std::string& err) {
    static const char* kMarks[] = {"CHAT_ID_INVALID", "CHAT_ADMIN_REQUIRED", "CHANNEL_PRIVATE", "CHANNEL_INVALID"};
    for (const char* m : kMarks) {
        if (err.find(m) != std::string::npos) return true;
    }
    return false;
}

// "https://t.me/+AbCdEf" / "https://t.me/joinchat/AbCdEf" -> "AbCdEf"
std::string invite_hash_from_link(const std::string& link) {
    auto pos = link.find_last_of('/');
    std::string tail = pos == std::string::npos ? link : link.substr(pos + 1);
    if (!tail.empty() && tail[0] == '+') tail.erase(0, 1);
    return tail;
}

// "telegram_error:420:FLOOD_WAIT_31" -> 31
int flood_wait_seconds(const std::string& err) {
    auto pos = err.find("FLOOD_WAIT_");
    if (pos == std::string::npos) return 0;
    pos += 11;
    long v = 0;
    bool any = false;
    while (pos < err.size() && err[pos] >= '0' && err[pos] <= '9') {
        v = v * 10 + (err[pos] - '0');
        pos++;
        any = true;
    }
    return any ? static_cast<int>(v) : 0;
}

std::string flood_text(int64_t chat_id, const std::string& err) {
    int n = flood_wait_seconds(err);
    if (n <= 0) return {};
    if (n >= 60) {
        return F(chat_id, "flood_minutes", LocaleArg{{"duration", std::to_string((n + 59) / 60)}});
    }
    return F(chat_id, "flood_seconds", LocaleArg{{"duration", std::to_string(n)}});
}

}  // namespace

std::string ensure_voice_chat_ready(RoomState* r, int64_t notify_chat_id) {
    if (!r || !r->assistant || !r->assistant->ntg || !r->assistant->ntg->mt()) {
        return F(notify_chat_id, "err_assistant_get_failed");
    }
    Assistant* ass = r->assistant;
    auto mt = ass->ntg->mt();
    tg::TdClient& bot = tg::bot();

    // Session strings from some generators carry no embedded user id; resolve
    // it via users.getUsers(inputUserSelf) once and cache it on the assistant.
    if (ass->user_id == 0) {
        int64_t uid = 0;
        std::string uerr;
        if (!mt->self_user_id(&uid, &uerr)) {
            ADVAIT_LOG_ERROR("chat_state", "assistant self lookup failed: " + uerr);
            return F(notify_chat_id, "err_assistant_get_failed");
        }
        ass->user_id = uid;
        ADVAIT_LOG_INFO("chat_state", "assistant user id resolved: " + std::to_string(uid));
    }

    // Membership snapshot from the bot (needs admin in the chat, like KanhaMusic
    // FullChat + GetChatMember).
    ADVAIT_LOG_INFO("chat_state", "member-state lookup chat=" + std::to_string(r->id) +
                                      " user=" + std::to_string(ass->user_id));
    auto info = bot.get_chat_member_state(r->id, ass->user_id);
    ADVAIT_LOG_INFO("chat_state",
                    "member-state done: ok=" + std::to_string(info.ok) + " present=" +
                        std::to_string(info.present) + " banned=" + std::to_string(info.banned) +
                        " err=" + info.error);
    if (!info.ok) {
        if (is_admin_error(info.error)) return F(notify_chat_id, "err_admin_permission_required");
        return F(notify_chat_id, "err_fetch_failed", LocaleArg{{"error", utils::escape_html(info.error)}});
    }
    if (info.banned) {
        return F(notify_chat_id, "err_assistant_banned",
                 LocaleArg{{"user", utils::escape_html(ass->first_name)},
                           {"id", std::to_string(ass->user_id)}});
    }
    if (info.present) return {};

    // Not a member -> join. Prefer the public username, else a fresh invite link.
    std::string join_err;
    auto join_once = [&]() -> bool {
        std::string username = bot.chat_public_username(r->id);
        if (!username.empty()) return mt->join_channel_by_username(username, &join_err);
        std::string link, lerr;
        if (!bot.create_chat_invite_link(r->id, &link, &lerr)) {
            join_err = lerr;
            return false;
        }
        std::string hash = invite_hash_from_link(link);
        if (hash.empty()) {
            join_err = "bot returned an invite link without a hash: " + link;
            return false;
        }
        return mt->join_channel_by_invite(hash, &join_err);
    };

    if (!join_once()) {
        if (join_err.find("INVITE_REQUEST_SENT") != std::string::npos) {
            // Assistant sent a join request (join-by-request chat); approve it as
            // the bot, like KanhaMusic approveJoinRequest.
            if (!bot.approve_join_request(r->id, ass->user_id, nullptr)) {
                return F(notify_chat_id, "err_assistant_join_request_sent");
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            return {};
        }
        int fw = flood_wait_seconds(join_err);
        if (fw > 0) {
            // Short floods: sleep and retry once; long ones surface to the user.
            if (fw <= 15) {
                std::this_thread::sleep_for(std::chrono::seconds(fw + 1));
                join_err.clear();
                if (join_once()) return {};
            }
            std::string msg = flood_text(notify_chat_id, join_err);
            return msg.empty() ? F(notify_chat_id, "err_assistant_invite_failed",
                                   LocaleArg{{"error", utils::escape_html(join_err)}})
                               : msg;
        }
        if (is_admin_error(join_err)) return F(notify_chat_id, "err_admin_permission_required");
        return F(notify_chat_id, "err_assistant_invite_failed",
                 LocaleArg{{"error", utils::escape_html(join_err)}});
    }
    return {};
}

}  // namespace advait::chat_state
