# vc_control porting notes (position/replay/loop/speed/shuffle/seek/jump/remove/clear/move/mute/unmute/askip/active)

Files: `include/advait/modules/vc_control.hpp`, `src/modules/vc_control.cpp`.
Registered in `init_vc_control()` — init must be called from the (off-limits) `src/modules/handlers.cpp`
and `vc_control.cpp` added to the `advait_modules` source list in `CMakeLists.txt` by the build maintainer.

## Locale keys

Every locale key used by the Go sources exists in `locales/en.yml` and is used via `F()`.
No locale keys were hardcoded. Hardcoded English (Go also hardcodes it, not locale-driven):
the `/active` report text ("Active Voice Chats", "There are currently N active voice/video
chat(s) running.", "No active chat sessions found.", "Total active: N", status cells
"Muted/Paused/Playing (Video)/Playing (Audio)/Idle", "No song playing.", "Empty") and the
literal "forward"/"backward" passed to the seek locale `{direction}` placeholder.

## Gaps / approximations vs the Go implementation

1. **filterAuthUsers (chat-admin rights)**: Go's `canUseAdminCommand` also consults real
   Telegram chat-admin status and a 3-value admin mode (everyone / admins / admins+auth).
   tdlib.hpp here exposes no chat-admin query and the db API only has the boolean
   `play_mode_admins_only`. Ported as: owner or sudo or `db::is_authorized` allowed;
   otherwise replies per Go's key selection (`only_admin` when admins-only mode is set,
   else `only_admin_or_auth`). Members who are raw chat admins (not auth users) may be
   wrongly rejected; "everyone" mode does not exist so no one is over-permitted.
   This matches the precedent set by play.cpp/pause.cpp.
2. **isSuperGroup side effects**: Go calls `database.AddServedUser/AddServedChat` inside
   isSuperGroup. Skipped, matching the existing C++ ports (pause.cpp etc.).
3. **effective_room lifecycle**: Go's `getEffectiveRoom` creates a room if missing
   (it then fails the `active()` gate with `room_no_active`). common.hpp's `effective_room`
   returns nullptr instead; the handlers reply `room_no_active` in that case, so the user
   sees the same Go message (same fallback pattern as pause.cpp).
4. **shuffle state**: Go's engine has `Shuffle()` getter + `SetShuffle()`; C++ `RoomState`
   only has an immediate `shuffle()` action and a private flag with no public get/set.
   Workaround: a per-chat flag map (mutex-guarded) inside vc_control.cpp mirrors visible
   state; enabling reorders the queue via `shuffle()`. On "off" the engine's internal
   `shuffle_` flag stays set (affects `next_track()` ordering) until room recreation.
   Go also calls `r.Parse()` (engine re-sync) for position/shuffle; no C++ equivalent — skipped.
5. **Go error objects**: Go's `SetSpeed/Seek/Replay/Mute/Unmute` return rich error strings;
   C++ returns bool. `{error}` placeholders are filled with a short tag ("replay", "speed",
   "seek", "mute", "unmute", "download"/actual download error) as pause.cpp already does.
6. **mute/unmute voice-chat join**: Go's `r.Mute()/Unmute()` drive the TData voice-chat
   mute state. Here the C++ `RoomState::mute()/unmute()` flags are used; the exact
   voice-chat join/mute RPC path does not exist in this port.
7. **askip**: Go's `handleAutoplaySkip` uses `PickAutoplayTrack` (radio-mix / smart search /
   golden-zone matching with retry loop up to 5 candidates, 60s download timeouts) and logs
   to the logger channel. Ported approximation: `platforms::autoplay_tracks(last, 5,
   played_history())`, first usable candidate, single download attempt; failure path
   `stream_download_fail` + `drop_room` as in skip.cpp. No logger post (`sendAutoplayLogs`).
   Autoplay requester marking follows Go including the "🎵 ᴀᴜᴛᴏᴘʟᴀʏ" fallback literal.
8. **/active rich message**: Go sends an `InputRichMessage` HTML `<details>`+`<table>`.
   tdlib-only `send_text` supports none of that; the report is rendered as a plain HTML
   numbered list (`<b>/<code>/<a>/<i>` only) with Go's exact wording, statuses and sorting
   (chat ids ascending, destroyed/inactive rooms skipped). `<summary>`/table chrome replaced
   by a bold "📊 Active Chats" line. Go used `SendRichMessage` to the chat (not a reply);
   here it is `reply_text`. The Go mongo-backed parts did not exist (Go only iterates
   `core.AllRooms()`), and this port iterates `all_rooms()` — equivalent.
9. **Position track check**: Go treats a track with empty `ID` as inactive; ported with
   `t->id.empty()` (Track optional handles the nil case).
10. **Strict number parsing**: Go `strconv.Atoi/ParseFloat` reject trailing junk; replicated
    with `std::from_chars` (full-consumption) and `strtod` + endptr, per PORTING robustness
    rules. Go accepts "inf"/"nan" in ParseFloat; the port rejects non-finite values
    (safer, and out of the valid 0.50-4.0 range anyway).
11. **Blacklist/maintenance wrapper**: Go registers every handler through
    `WithBlacklistMessage` (handled globally in the C++ core by whoever owns the dispatch
    layer); not re-implemented in this module.
12. **Loop `r.Loop()` before active check**: Go reads the current loop count before the
    active gate; ordering preserved but has no observable effect.
