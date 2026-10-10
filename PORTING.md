# AdvaitMusic Go -> C++ module porting guide (for porting agents)

## Context
- C++ project root: /workspace/AdvaitMusic-cpp (build: cmake --build build -j4 ; your files join lib advait_modules).
- Go source of truth: /workspace/AdvaitMusic/kanha/modules/*.go
- Goal: port Go handlers faithfully: same commands, aliases, locale keys, behavior flow. Where Go used mongo, use the in-process db API below. Where Go needs capabilities we lack (gogram/TData voice-chat join, Go-code eval), implement the closest reasonable behavior and record gaps in your NOTES file.

## HARD RULES
- Only CREATE your assigned files: src/modules/NAME.cpp, include/advait/modules/NAME.hpp, and NAME_NOTES.md at repo root.
- Never edit shared files: CMakeLists.txt, src/modules/handlers.cpp, include/advait/modules/modules.hpp, database files, locales/*.yml, tdlib sources/headers, buttons, room, common.hpp, peers' files.
- Locale strings: use F(chat_id, "key", args) with keys that exist in locales/en.yml (grep to verify). Missing key: hardcode Go's exact English text inline and list it in NOTES.md.
- C++23, namespace advait::modules, style matches src/modules/skip.cpp (2-space indent, no comments unless explaining a non-obvious why).
- Robustness: validate args before std::stoll/stod via try/catch; guard nulls; never crash.

## Your exports
Header declares: void init_GROUP(tg::TdClient& bot);  // body registers with bot.on_command / bot.on_callback only.

## API reference (verified)
### telegram, include "advait/telegram/tdlib.hpp", namespace advait::tg
- struct Message: int64 chat_id, id, sender_id; std::string text (full message incl. /cmd args), first_name, last_name; bool is_private, is_channel; int64 reply_to; json raw.
- struct CallbackQuery: int64 chat_id, message_id, user_id, query_id; std::string data; json raw.
- bot.on_command("play", h): handles /play and /play@bot (case-insensitive).
- bot.on_callback("room:", h): fires when data equals prefix or starts with prefix; FIRST registered match wins.
- bool send_text(chat_id, html, markup={}); bool reply_text(msg, html, markup={});
- bool edit_text(chat_id, msg_id, html, markup={}); bool send_photo(chat_id, url, caption, markup={});
- bool answer_callback(query_id); bool delete_message(chat_id, msg_id);
- tg::bot() is the singleton. Outgoing text is HTML-parsed; escape user data with utils::escape_html.

### shared helpers, include "kanha/modules/common.hpp", namespace advait::modules
- std::string mention_of(const Message&)  -> html tg://user?id mention
- RoomState* effective_room(const Message&, bool cplay, std::string* err) -> room_for(chat) or nullptr; on cplay with no linked channel sets err to F(chat,"cplay_id_not_set")
- std::string cmd_arg(const Message&) -> trimmed arg text after first space, empty if none
- now_playing_text(chat_id, Track) / send_now_playing(chat_id, RoomState*, Track)

### room, include "advait/core/room.hpp", namespace advait
- RoomState: id, chat_id; play(Track,path,force), pause(), resume(), replay(), stop(), seek(int delta_sec), set_speed(double) returns false when out of bounds, mute(), unmute(); active(), destroyed(), paused(), muted(), autoplay(), set_autoplay(bool), position(), speed(), loop()/set_loop(int), track() optional, next_track() optional, queue() vector, clear_queue(), remove_at(size_t)->bool, move(from,to)->bool, shuffle(), file_path(), played_history().
- RoomState* room_for(int64 chat); RoomState* create_room(int64, Assistant*); bool drop_room(int64); std::unordered_map<int64,RoomState*> all_rooms();

### database, include "advait/database/database.hpp", namespace advait::db
- get_assistant(chat); init_assistant_indexes(n)
- thumbnails_disabled / set_thumbnails_disabled(chat,bool)
- play_mode_admins_only / set_play_mode_admins_only(chat,bool)
- autoplay / set_autoplay(chat,bool)
- linked_channel / set_linked_channel(chat, chan or 0 to clear)
- language / set_language(chat, "en")
- is_authorized(chat,user); add_auth_user; del_auth_user; auth_users(chat) -> vector int64
- is_sudo; add_sudo; del_sudo; sudo_list()
- is_blacklisted_user; add_blacklisted_user; del_blacklisted_user; blacklisted_users()
- is_blacklisted_chat; add_blacklisted_chat; del_blacklisted_chat; blacklisted_chats()
- maintenance(); set_maintenance(bool)
- add_served_user(id); add_served_chat(id); served_users(); served_chats()
- playlists (per user, in-memory): create_playlist(user,name)->false if exists; delete_playlist; playlist_exists; playlists_of(user) sorted; add_to_playlist(user,name, PlaylistEntry{title,url,duration,requester}); remove_from_playlist(user,name,index); playlist_entries(user,name)

### locales / config / utils / buttons
- include "advait/core/locales.hpp": std::string F(int64 chat_id, key, LocaleArg args={})  (LocaleArg = map<string,string>; placeholders {name}).
- include "advait/core/config.hpp": Config::get() fields: owner_id, owner_username, logger_id, dev_url, support_chat, support_channel, default_lang, duration_limit, queue_limit, max_auth_users, disable_colour, start_time, port.
- include "advait/core/utils.hpp": format_bytes/duration/time/uptime, get_progress_bar(played,duration), short_title, escape_html, html_mention(name,id), parse_bool/int/int64, trim, to_lower, base64_encode/decode, split_fields.
- include "advait/core/buttons.hpp": url_btn, data_btn, markup_rows(rows of row vectors), cancel_keyboard, broadcast_cancel_keyboard, supp_markup, stop_confirm_markup, play_markup(chat, room, queued), group_help_keyboard, start_markup, repo_markup(chat, dev_url), help_keyboard, back_keyboard, restart_confirm_markup(chat).
- Logger: include "advait/core/logger.hpp", ADVAIT_LOG_INFO("tag","msg") / ADVAIT_LOG_ERROR.

## Verification
You cannot run the bot. Verify by syntax-checking YOUR translation unit only:
  g++ -std=c++23 -fsyntax-only -I include -I build/_deps/nlohmann_json-src/include src/modules/NAME.cpp
(look under build/_deps for the exact nlohmann include dir; find it once with: ls build/_deps)
Fix all errors in your files before finishing. Do not try to link or build the whole project.

## Handoff
Finish with a summary: files created, commands/callbacks registered, keys hardcoded, gaps.
