# playlists.cpp porting notes

Go sources: kanha/modules/playlist.go, callback.go (emptyCBHandler), play.go
(autoplayCallbackHandler + autoplayMenu), handlers.go lines 103-109 / 210-211.

## Files
- include/advait/modules/playlists.hpp — declares `void init_playlists(tg::TdClient& bot);`
- src/modules/playlists.cpp — handlers + registration

## Registrations (all inside init_playlists)
- /createplaylist -> handle_create_playlist (Go createPlaylistHandler)
- /deleteplaylist -> handle_delete_playlist (Go deletePlaylistHandler)
- /addtoplaylist -> handle_add_to_playlist (Go addToPlaylistHandler)
- /removefromplaylist -> handle_remove_from_playlist (Go removeFromPlaylistHandler)
- /playlistinfo and /plistinfo -> handle_playlist_info (Go playlistInfoHandler, both aliases)
- /myplaylists -> handle_my_playlists (Go myPlaylistsHandler)
- on_callback("progress", handle_progress_cb) — Go emptyCBHandler (regex "progress"); answers with
  nothing (answer_callback, no toast) to dismiss the client spinner
- on_callback("autoplay:toggle", handle_autoplay_toggle_cb) — Go autoplayCallbackHandler

## Locale keys used (all verified present in locales/en.yml)
playlist_usage_create, playlist_limit_reached, playlist_create_fail, playlist_create_success,
playlist_usage_delete, playlist_not_found, playlist_not_owner (unreachable, see gaps),
playlist_delete_fail, playlist_delete_success, playlist_usage_add, playlist_invalid_query,
playlist_add_fail, playlist_add_success, playlist_usage_remove, playlist_invalid_song_number,
playlist_song_not_found, playlist_remove_fail, playlist_remove_success, playlist_usage_info,
playlist_songs_empty, playlist_song_line, playlist_info, playlist_none, playlist_list_line,
playlist_list, autoplay_menu, autoplay_btn, autoplay_enabled/disabled only via toast (not used as
toast; see below), enabled, disabled, CLOSE_BTN.

No locale key was missing; nothing was hardcoded for missing keys. The only inline English strings
are synthetic error strings fed into the existing {error} placeholders ("a playlist named '...'
already exists", "delete failed", "add failed", "remove failed") because the in-process db API
returns bool, not error text (Go injected err.Error()).

## Key adaptation: id-based -> name-based playlists
Go stores playlists under a generated id (tgpl_...) with a global lookup; the C++ db API is keyed
by (user_id, name). Consequences:
- {id} placeholders in playlist_create_success and playlist_list_line are filled with the playlist
  name (there is no generated id).
- Playlist "ownership" is implicit: lookups only search the caller's own playlists, so
  playlist_not_found is returned where Go would return playlist_not_owner (a global name->owner
  lookup does not exist). Key playlist_not_owner is registered in Go's flow but is unreachable in
  this port.
- playlistinfo of ANOTHER user's playlist is not possible (Go allowed it via global GetPlaylist).
  Only the caller's playlists are visible here.
- create: 10-playlist limit checked via playlists_of(user).size() >= 10 (Go UserPlaylistLimitReached);
  40-rune name trim implemented UTF-8-aware (truncate_runes) to mirror Go's []rune slicing.
- addtoplaylist: reuses platforms::resolve_query(query, false) (the same call play.cpp uses) and
  stores first result's title/url/duration into PlaylistEntry; requester = sender display name.
  Go also stored TrackID/Platform; PlaylistEntry has no such fields, so remove-identifier matching
  falls back to URL-only comparison (Go matched URL or TrackID).
- removefromplaylist: numeric arg validated as full string (std::stoi with pos check) mirroring
  strconv.Atoi; range-checked 1..size -> playlist_invalid_song_number; non-numeric falls back to URL
  match -> playlist_song_not_found.
- playlistinfo: Go has no pagination (single message with all songs); ported identically (list all).
  Owner name: C++ tg layer has no user lookup API, so "Unknown" is used, which is Go's own fallback
  value when c.GetUser fails.
- fetch-failure paths (playlist_fetch_fail on DB errors in create/myplaylists) are unreachable:
  the in-memory db API cannot fail.

## Callback adaptations / gaps
- "progress" callback: Go matches substring "progress" anywhere in data via regex; the C++
  on_callback matches prefix/equality only. Prefix registration covers button payloads that start
  with "progress"; data with "progress" embedded mid-string (not at start) would not fire.
- autoplay:toggle: room is resolved from cb.chat_id (Go uses u.ChatId; the room id is NOT embedded
  in the payload, data is exactly "autoplay:toggle"). Toggle logic mirrors Go: active room ->
  set_autoplay(!autoplay); otherwise flip db::autoplay(chat) and also push to the room if one
  exists. The menu is re-rendered via edit_text on cb.message_id using the same rows Go's
  autoplayMenu builds (autoplay_btn with state + CLOSE_BTN -> data "close").
  GAP: Go's toast text (autoplay_enabled/autoplay_disabled via u.Answer show_alert=false) is lost —
  answer_callback(query_id) carries no message field in this tdlib layer.
  GAP: Go gates with canUseAdminCommand (admins/auth users only, toasts only_admin_cb /
  only_admin_or_auth_cb which do exist in en.yml); the C++ tg layer exposes no chat-admin query and
  existing C++ callback handlers (handlers.cpp room cb, help, start) perform no such check either,
  so the permission gate is omitted.
- "autoplay:toggle" is registered as a prefix in C++, so any data starting with it (e.g.
  "autoplay:toggleX") would also fire; Go used exact equality. No such payload is produced by the
  codebase.

## Integration
- init_playlists must be called from advait::modules::init in src/modules/handlers.cpp
  (shared file, not editable by this agent — integration wiring is a gap for the orchestrator).
- Go also exposes playlistIDPrefix/isPlaylistQuery/playlistTracks used by play.go's
  "/play tgpl_xxx" flow and registers /autoplay command plus helpTexts; those belong to the play /
  help modules, not to this file.
