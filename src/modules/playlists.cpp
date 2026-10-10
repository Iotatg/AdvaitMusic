#include "advait/modules/playlists.hpp"

#include "advait/core/buttons.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/modules/common.hpp"
#include "advait/platforms/platforms.hpp"
#include "advait/telegram/tdlib.hpp"

#include <string>
#include <vector>

namespace advait::modules {
namespace {

constexpr size_t kMaxPlaylistNameLen = 40;
constexpr size_t kMaxPlaylistsPerUser = 10;

std::string command_of(const tg::Message& m) {
    auto t = m.text;
    auto sp = t.find(' ');
    return sp == std::string::npos ? t : t.substr(0, sp);
}

std::string truncate_runes(const std::string& s, size_t max_runes) {
    size_t runes = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (runes == max_runes) return s.substr(0, i);
            ++runes;
        }
    }
    return s;
}

std::string requester_name(const tg::Message& m) {
    auto name = utils::trim(m.first_name + " " + m.last_name);
    return name.empty() ? std::to_string(m.sender_id) : name;
}

bool split_two(const std::string& args, std::string& first, std::string& second) {
    auto sp = args.find(' ');
    if (sp == std::string::npos) return false;
    first = args.substr(0, sp);
    second = utils::trim(args.substr(sp + 1));
    return !first.empty() && !second.empty();
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\n";
        out += lines[i];
    }
    return out;
}

void handle_create_playlist(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    auto name = cmd_arg(m);
    if (name.empty()) {
        bot.reply_text(m, F(chat_id, "playlist_usage_create", {{"cmd", command_of(m)}}));
        return;
    }
    if (db::playlists_of(m.sender_id).size() >= kMaxPlaylistsPerUser) {
        bot.reply_text(m, F(chat_id, "playlist_limit_reached"));
        return;
    }
    name = truncate_runes(name, kMaxPlaylistNameLen);
    if (!db::create_playlist(m.sender_id, name)) {
        bot.reply_text(m, F(chat_id, "playlist_create_fail",
                                {{"error", "a playlist named '" + name + "' already exists"}}));
        return;
    }
    bot.reply_text(m, F(chat_id, "playlist_create_success",
                            {{"name", utils::escape_html(name)}, {"id", utils::escape_html(name)}}));
}

void handle_delete_playlist(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    auto name = cmd_arg(m);
    if (name.empty()) {
        bot.reply_text(m, F(chat_id, "playlist_usage_delete", {{"cmd", command_of(m)}}));
        return;
    }
    if (!db::playlist_exists(m.sender_id, name)) {
        bot.reply_text(m, F(chat_id, "playlist_not_found"));
        return;
    }
    if (!db::delete_playlist(m.sender_id, name)) {
        bot.reply_text(m, F(chat_id, "playlist_delete_fail", {{"error", "delete failed"}}));
        return;
    }
    bot.reply_text(m, F(chat_id, "playlist_delete_success", {{"name", utils::escape_html(name)}}));
}

void handle_add_to_playlist(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    std::string name, query;
    if (!split_two(cmd_arg(m), name, query)) {
        bot.reply_text(m, F(chat_id, "playlist_usage_add", {{"cmd", command_of(m)}}));
        return;
    }
    if (!db::playlist_exists(m.sender_id, name)) {
        bot.reply_text(m, F(chat_id, "playlist_not_found"));
        return;
    }
    auto tracks = platforms::resolve_query(query, false);
    if (tracks.empty()) {
        bot.reply_text(m, F(chat_id, "playlist_invalid_query"));
        return;
    }
    db::PlaylistEntry entry;
    entry.title = tracks[0].title;
    entry.url = tracks[0].url;
    entry.duration = tracks[0].duration;
    entry.requester = requester_name(m);
    if (!db::add_to_playlist(m.sender_id, name, entry)) {
        bot.reply_text(m, F(chat_id, "playlist_add_fail", {{"error", "add failed"}}));
        return;
    }
    bot.reply_text(m, F(chat_id, "playlist_add_success",
                            {{"song", utils::escape_html(entry.title)},
                             {"playlist", utils::escape_html(name)}}));
}

void handle_remove_from_playlist(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    std::string name, ident;
    if (!split_two(cmd_arg(m), name, ident)) {
        bot.reply_text(m, F(chat_id, "playlist_usage_remove", {{"cmd", command_of(m)}}));
        return;
    }
    if (!db::playlist_exists(m.sender_id, name)) {
        bot.reply_text(m, F(chat_id, "playlist_not_found"));
        return;
    }
    auto entries = db::playlist_entries(m.sender_id, name);
    size_t index = 0;
    bool found = false;
    try {
        size_t pos = 0;
        int parsed = std::stoi(ident, &pos);
        if (!ident.empty() && pos == ident.size()) {
            if (parsed < 1 || static_cast<size_t>(parsed) > entries.size()) {
                bot.reply_text(m, F(chat_id, "playlist_invalid_song_number"));
                return;
            }
            index = static_cast<size_t>(parsed - 1);
            found = true;
        }
    } catch (...) {
    }
    if (!found) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].url == ident) {
                index = i;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        bot.reply_text(m, F(chat_id, "playlist_song_not_found"));
        return;
    }
    if (!db::remove_from_playlist(m.sender_id, name, index)) {
        bot.reply_text(m, F(chat_id, "playlist_remove_fail", {{"error", "remove failed"}}));
        return;
    }
    bot.reply_text(m, F(chat_id, "playlist_remove_success", {{"name", utils::escape_html(name)}}));
}

void handle_playlist_info(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    auto name = cmd_arg(m);
    if (name.empty()) {
        bot.reply_text(m, F(chat_id, "playlist_usage_info", {{"cmd", command_of(m)}}));
        return;
    }
    if (!db::playlist_exists(m.sender_id, name)) {
        bot.reply_text(m, F(chat_id, "playlist_not_found"));
        return;
    }
    auto entries = db::playlist_entries(m.sender_id, name);
    std::string songs = F(chat_id, "playlist_songs_empty");
    if (!entries.empty()) {
        std::vector<std::string> lines;
        lines.reserve(entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            lines.push_back(F(chat_id, "playlist_song_line",
                              {{"index", std::to_string(i + 1)},
                               {"name", utils::escape_html(entries[i].title)},
                               {"url", entries[i].url}}));
        }
        songs = join_lines(lines);
    }
    bot.reply_text(m, F(chat_id, "playlist_info",
                            {{"name", utils::escape_html(name)},
                             {"owner", "Unknown"},
                             {"count", std::to_string(entries.size())},
                             {"songs", songs}}));
}

void handle_my_playlists(const tg::Message& m) {
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    auto names = db::playlists_of(m.sender_id);
    if (names.empty()) {
        bot.reply_text(m, F(chat_id, "playlist_none"));
        return;
    }
    std::vector<std::string> lines;
    lines.reserve(names.size());
    for (const auto& n : names) {
        lines.push_back(F(chat_id, "playlist_list_line",
                          {{"name", utils::escape_html(n)}, {"id", utils::escape_html(n)}}));
    }
    bot.reply_text(m, F(chat_id, "playlist_list", {{"playlists", join_lines(lines)}}));
}

void handle_progress_cb(const tg::CallbackQuery& cb) {
    tg::bot().answer_callback(cb.query_id);
}

std::string autoplay_menu_text(int64_t chat_id, bool enabled) {
    return F(chat_id, "autoplay_menu", {{"state", F(chat_id, enabled ? "enabled" : "disabled")}});
}

nlohmann::json autoplay_menu_markup(int64_t chat_id, bool enabled) {
    auto state = F(chat_id, enabled ? "enabled" : "disabled");
    return markup_rows({{data_btn(F(chat_id, "autoplay_btn", {{"state", state}}), "autoplay:toggle")},
                        {data_btn(F(chat_id, "CLOSE_BTN"), "close")}});
}

void handle_autoplay_toggle_cb(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    auto chat_id = cb.chat_id;
    bool enabled = false;
    auto* r = room_for(chat_id);
    if (r && r->active()) {
        enabled = !r->autoplay();
        r->set_autoplay(enabled);
    } else {
        enabled = !db::autoplay(chat_id);
        db::set_autoplay(chat_id, enabled);
        if (r) r->set_autoplay(enabled);
    }
    bot.answer_callback(cb.query_id);
    bot.edit_text(cb.chat_id, cb.message_id, autoplay_menu_text(chat_id, enabled),
                  autoplay_menu_markup(chat_id, enabled));
}

}  // namespace

void init_playlists(tg::TdClient& bot) {
    bot.on_command("createplaylist", handle_create_playlist);
    bot.on_command("deleteplaylist", handle_delete_playlist);
    bot.on_command("addtoplaylist", handle_add_to_playlist);
    bot.on_command("removefromplaylist", handle_remove_from_playlist);
    bot.on_command("playlistinfo", handle_playlist_info);
    bot.on_command("plistinfo", handle_playlist_info);
    bot.on_command("myplaylists", handle_my_playlists);
    bot.on_callback("progress", handle_progress_cb);
    bot.on_callback("autoplay:toggle", handle_autoplay_toggle_cb);
}

}  // namespace advait::modules
