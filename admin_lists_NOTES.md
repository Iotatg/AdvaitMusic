# admin_lists porting notes (Go -> C++)

Ported files: `src/modules/admin_lists.cpp`, `include/advait/modules/admin_lists.hpp`.
Go sources: `modules/sudoers.go`, `blacklist.go`, `auth.go`, `autoleave.go`, `cleanmode.go` (command handler from `settings.go`), `settings.go` (playmode/adminmode), `thumbnail.go` (nothumb), `cplay.go` (setcplay), `play.go` (autoplay command), `language.go`, gates from `filters.go`, args from `utils/extract_user.go`, `utils/extract_chat.go`, `utils/parser.go`.

## Command gates mirrored from Go (verified per handler, not blanket sudo)

- addsudo/delsudo and the whole block/unblock/blocked family: Go uses `checkOwner` (strict `SenderID == OwnerID`, denied even when OwnerID==0) -> mirrored with `sender_id == Config::get().owner_id`, denial key `only_owner`. Note: the task brief suggested `db::is_sudo`; Go is owner-only for these commands, and the port stays faithful to Go.
- autoleave: Go uses `checkSudo` = owner OR sudo -> `sender == owner_id || db::is_sudo(sender)`, denial key `only_sudo`.
- sudoers: no gate in Go (open to all), only a 30s per chat+sender flood guard -> replicated with a local steady-clock map (key format `sudoers:<chat><sender>` matches Go's `%d%d`).
- auth / addauth / delauth / authlist: Go handlers have NO permission check despite help text claiming admins only -> mirrored as-is (no gate).
- nothumb, playmode, channelplay/setcplay, lang, autoplay, cleanmode, adminmode: Go gates with `isSuperGroup` + `filterAuthUsers` (admins/auth-users per adminmode). `isSuperGroup` is replicated (private -> `only_supergroup` + `db::add_served_user`; channel -> silent drop; group -> `db::add_served_chat`). `filterAuthUsers` CANNOT be replicated (admin checks, see gaps).

## Callbacks registered

- `bot.on_callback("lang:", lang_cb)` — payloads identical to Go: `lang:select` (re-shows the picker with a `BACK_BTN` -> `set:main` back row) and `lang:<code>` (sets `db::set_language`). The "already this language" and success paths render via `edit_text` because `answer_callback` has no toast-text parameter (Go used `Answer` toasts: `lang_same`, `lang_success`, `lang_fail`, `⚠️ Invalid data.`).
- The autoplay menu still embeds an `autoplay:toggle` button, but the callback is NOT registered here (owned by the play/room module, per task instructions).

## Gaps

1. No chat-admin queries in the TDLib client: `filterAuthUsers` / `IsChatAdmin` / `canUseAdminCommand` checks are replaced by "any sender allowed" for nothumb, playmode, channelplay, cleanmode, adminmode, lang (command), lang:xx (callback), autoplay. Denial keys `only_admin`, `only_admin_or_auth`, `only_admin_cb` are consequently never emitted from this file.
2. No `GetMessage`/replied-message lookup: replying to a user with addsudo/delsudo/blockuser/unblockuser/auth/delauth replies `user_extract_fail` with "replied message is not available, pass a numeric user id" instead of resolving the reply sender. `addauth_user_is_admin` check dropped (also unreacheable without admin API).
3. No `SearchPublicChat`/username resolution: @username args for users/channels reply with the same `user_extract_fail` / `blacklist_invalid_chat_identifier` errors; only numeric ids work.
4. No `GetUser`: user-type (bot) checks dropped — `sudo_bot_user` and `addauth_bot_user` keys unreachable; user names/mentions unavailable, so `{user}` placeholders render as `<code>id</code>` (Go's own no-info fallback) and `del_auth_success` always uses Go's user==nil format `User (<code>id</code>)`; `authlist` entries always use `authlist_user_fail` formatting (index + id).
5. db API exposes no error codes, so Go DB-error replies with `{error}` collapse to the fixed text "storage error"; unreachable locale keys kept out: `sudo_check_fail`, `sudo_fetch_user_fail`, `authlist_fetch_fail`, `blacklist_fetch_chats_fail`, `blacklist_fetch_users_fail`, `autoleave_fetch_fail`, `autoleave_update_fail`, `nothumb_fetch_fail`, `nothumb_update_fail`, `lang_fail`.
6. No `EOR`/edit-after-reply: `/sudoers` and `/authlist` skip the "fetching..." placeholder message (Go replied then edited it via utils.EOR) and reply with the finished list directly; the 1s/entry throttle sleep in `handleGetSudoers` is also dropped. Go bot-command-scope updates in addsudo/delsudo (`SetCommands`/`DeleteCommands` for sudo menus) have no equivalent — dropped.
7. autoleave background loop (assistant dialog scan / LeaveChannel every 10 min, 50 chats/cycle, floodwait handling) is NOT ported: no multi-account dialog iteration in this client. The command only flips a process-local flag, so `autoleave` state is in-memory only (no `db::autoleave` API) and restarts reset it.
8. cleanmode persistence/scheduler missing: no `db::cleanmode` API and no UpdateChatReadOutbox hook in the client, so the flag is a per-chat in-memory map, auto-delete never happens, and the delay shown is fixed at 15m (no `GetChatSettings.CleanModeDurationMins`). `cleanmode_hint` mentions /settings which does not exist in this build yet.
9. adminmode persistence missing: no `db::admin_mode` API, so mode is a per-chat in-memory map; other modules' gating code cannot read it (it will not affect playback command rights yet).
10. channelplay: Go resolves @username (`SearchPublicChat`), the `linked` keyword (supergroup LinkedChatId), verifies bot admin+invite rights in the target and chat-owner rights (`canSetCPlayTarget`); none available. Port accepts only raw numeric negative (-100...) ids, `reset`/`none`/`del` (and Go's falsy bool words) clear via `db::set_linked_channel(chat, 0)` + `cplay_disabled`; `enable`/`true`-style args that aren't ids reply `cplay_invalid_target`; the `linked` keyword replies `cplay_resolve_peer_fail`.
11. autoplay command displays state room-first then db like Go; the toggle flow itself lives in the other module's `autoplay:toggle` callback (Go's `autoplayCallbackHandler` not ported here: its admin gate toasts `only_admin_cb`/`only_admin_or_auth_cb`, and its db+room toggle semantics remain that module's responsibility).
12. addauth limit check uses `Config::get().max_auth_users` like Go (`auth_limit_reached` with `{limit}`), checked BEFORE target extraction, same order as Go.

## Hardcoded strings (no locale key in Go or in locales/en.yml)

- `"Failed to block user: storage error"` / `"Failed to block chat: storage error"` — Go itself hardcoded `"Failed to block user: "+err` (Go err text replaced by fixed suffix since C++ db returns bool).
- `"User (<code>id</code>)"` — Go's inline fallback in delAuthHandler, used for every deletion here (gap 4).
- `"• None"` — Go's inline empty-list marker in handleBlacklisted (UTF-8 bullet), kept verbatim including the missing trailing newline in the users section.
- `"✔️ "` check-mark button prefix and numeric-`<code>` id formatting — Go inline literals.
- extract errors injected into `{error}`: "replied message is not available, pass a numeric user id", "no user identifier found", "username lookup is not supported, use a numeric user id", "storage error", "username lookup is not supported, use a numeric chat id" — English replacements for Go runtime error texts.
- Go's `⚠️ Invalid data.` toast for malformed `lang:` payloads is dropped (no toast text API).

## Locale keys used (all verified present in locales/en.yml)

only_owner, only_sudo, only_supergroup, auth_no_user, user_extract_fail, auth_limit_reached, cannot_authorize_user, already_authed, addauth_add_fail, addauth_success, addauth_success_with_count, del_auth_not_authorized, del_auth_remove_fail, del_auth_success, authlist_empty, authlist_header, authlist_user_fail, authlist_total, sudo_owner_self, sudo_bot_self, sudo_already, sudo_add_fail, sudo_add_success, sudo_owner_remove_block, sudo_not_exists, sudo_remove_fail, sudo_remove_success, sudo_list_header, sudo_list_owner, sudo_list_entry, sudo_list_no_extra, flood_seconds, blacklist_usage_blockchat, blacklist_usage_unblockchat, blacklist_invalid_chat_identifier, blacklist_block_user_success, blacklist_unblock_user_fail, blacklist_unblock_user_success, blacklist_block_chat_success, blacklist_unblock_chat_fail, blacklist_unblock_chat_success, blacklist_list_title, blacklist_list_chats, blacklist_list_users, autoleave_status, autoleave_already, autoleave_updated, invalid_bool, enabled, disabled, playmode_help, playmode_status_admins, playmode_status_everyone, playmode_success_admins, playmode_success_everyone, nothumb_status, nothumb_already, nothumb_updated, cleanmode_status, cleanmode_hint, adminmode_help, adminmode_invalid, adminmode_updated, adminmode_status_admin, adminmode_status_adminauth, adminmode_status_everyone, cplay_usage, cplay_resolve_peer_fail, cplay_invalid_target, cplay_disabled, cplay_enabled, autoplay_menu, autoplay_btn, CLOSE_BTN, lang_select, lang_same, lang_success, BACK_BTN, name (per-language button label).

Fidelity quirks kept on purpose: nothumb passes the RAW word "enabled"/"disabled" (un-localized) as `{action}` exactly like Go (autoleave/cleanmode/adminmode DO localize via `F()`); `playmode`/`adminmode`/`cleanmode` in Go silently `return err` on DB read errors — treated as default false/adminauth here.

## Verification

`g++ -std=c++23 -fsyntax-only -I include -I build/_deps/nlohmann_json-src/include src/modules/admin_lists.cpp` — passes with zero errors/warnings. Not added to CMakeLists (shared file, integrator's call).
