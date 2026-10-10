# Message-Edit Audit (Go -> C++ port)

Purpose: every place Go edits a Telegram message must have an equivalent
"same message, edited in place" behavior in the C++ port, and this file records
how the machinery works so future edit bugs are caught in minutes, not hours.

Related files:

- Go reference: `kanha/utils/edit_or_reply.go` (`utils.EOR`), `kanha/modules/*.go`
- C++ machinery: `include/advait/modules/common.hpp` (`LiveMsg`, `show_track`,
  `download_with_status`, `cancel_download`), `src/telegram/tdlib.cpp`
- Tests: `/tmp/opencode/pg/play_chain_test.py` (pyrogram harness, user @im_abz)

## 1. The TDLib edit problem (why edits used to "miss")

The C++ port talks to Telegram through TDLib (bot mode). Three TDLib quirks
caused every edit failure seen so far:

1. `sendMessage` responds with a TEMPORARY message id (e.g. `1048577`).
   `editMessageText` against the temp id fails with
   `400 Message not found` until the message is acknowledged by the server.
2. `updateMessageIds` never fires for bot-sent messages, and in bot mode
   `updateMessageSendSucceeded.old_message_ids` is `[]` — TDLib does not hand
   us the temp -> permanent id mapping.
3. TDLib stores the message text in PARSED form: HTML tags/entities are
   stripped (`<b>x</b>` becomes `x`). Comparing the raw HTML we sent with the
   text inside `updateMessageSendSucceeded` does not match.

### The solution implemented in `src/telegram/tdlib.cpp`

- `send_text_sync()` sends and waits for the response, then registers the pair
  `(chat, temp_id, parsed_plain_text)` in `pending_sends_` (a deque).
  Critically it registers the **parsed plain text** from
  `execute(parseTextEntities)`, never the raw HTML source (quirk 3).
- On `updateMessageSendSucceeded` we FIFO-match by `(chat_id, plain_text)`
  and record `id_remap_[chat:temp_id] = new_server_id`, waking any thread
  waiting in `edit_text()` via `id_cv_`.
- Race guard: the update can beat the sendMessage response. If the update
  arrives before the pending registration, it stores `text_remap_[chat+text]`
  instead; `send_text_sync()` consults that map and returns the server id
  directly (plus an identity `id_remap_` entry so `wait_sent()` works).
- `edit_text()` retries (10 s deadline): retryable = `Message not found`,
  `can't be edited`, `still SENDING`. It waits on `id_cv_` for the remap and
  retries with the mapped id. `MESSAGE_NOT_MODIFIED` counts as success.
  The successful id is returned through `final_id` so callers (LiveMsg) can
  upgrade their stored id to the server id.

### Debugging recipe (when an edit silently "sends a new message instead")

1. `grep "sendSucceeded\|send_text_sync chat=\|edit_text failed" <bot log>`
2. `sendSucceeded ... matched=n` -> text-mismatch: the text used to register
   differs from the parsed text (HTML tags, entity decoding) — fix the
   registration to use parsed plain text.
3. `old_ids=[]` and no sendSucceeded at all -> TDLib DB out of sync.
4. `edit_text failed: Message not found` with no retry -> `id_remap_` entry
   missing (pending overwritten by the 200-entry cap, or duplicate text where
   the wrong FIFO entry was consumed).
5. Verify live with `python3 /tmp/opencode/pg/play_chain_test.py`; every
   intermediate status must be ONE message with `EDITED` flag in the dump.

## 2. EOR pattern

Go's `utils.EOR(c, msg, text, markup)`: try EditText; on failure delete the
old message and ReplyText a fresh one, returning the new message.

C++ equivalent is `LiveMsg` (`include/advait/modules/common.hpp`):

- `lm.reply(m, text)`      - sync send (reply anchor), id captured (server id when possible)
- `lm.eor(text, markup)`  - edit in place; fallback delete+resend (Go EOR)
- `lm.send(text)`          - plain sync send at the same anchor
- `show_track(lm, ...)`    - Go `showTrackMessage`: text -> lm edit; artwork ->
  delete + `send_photo` (TDLib bot API has no editMessageMedia; Go's own
  fallback path is identical except Go CAN edit photo captions via TDLib.
  Known approximation.)

## 3. Call-site audit matrix

Legend: OK = implemented & verified live; OK* = implemented, not yet live-tested
(needs a group with active voice chat); approx = documented deviation;
missing = not ported (command absent in C++).

| Go site | Behavior | C++ site | Status |
|---|---|---|---|
| `play.go:218,307,311,318,340,364-387,418,425,430` searching/fail/dupe/skip texts | EOR on the searching message | `src/modules/play.cpp` (`lm.eor`) | OK (private chat, verified 2026-10-10) |
| `play.go:545` play_downloading_song (+cancel kb if >600 s) | EOR | `play.cpp` + `download_with_status` | OK* |
| `play.go:559,566` canceled / download failed | EOR | `play.cpp` (`dl_err=="canceled"`) | OK* (cancel live-tested only via button path being wired) |
| `download.go:218` download progress via UpdateFile every 2 s | edits `download_progress` text with markup kept | `common.hpp download_with_status` (curl xferinfo, 2 s throttle) | OK (progress edit seen live once; 0% because datacenter-IP blocks yt) |
| `play.go finalizePlayReply` + `helpers.go showTrackMessage` | now-playing edits the SAME message (photo variant edits media caption in Go) | `common.hpp show_track` | approx (photo variant: delete+resend instead of media edit) |
| `callback.go:46 cancelDownloadHandler` | alert `download_cancelled` / `no_download_to_cancel`, real context cancel | `handlers.cpp handle_cancel` + `common.hpp cancel_download` -> `iota.cpp curl_xferinfo` abort | OK* |
| `skip.go` stream_downloading_next / fail / play-fail edits (6 sites) | EOR on a status message sent first | `skip.cpp skip_next` LiveMsg chain | OK* |
| `skip.go handleAutoplaySkip` (+5 retries) same edits | EOR | `vc_control.cpp` auto-skip block (`download_with_status`) | OK* (no per-candidate retry edits yet — single download attempt) |
| `settings.go` settings card toggle edits (4) | EditText on the settings msg | `admin_system.cpp settings_cb` (`edit_text(cb.message_id)`) — callback ids are canonical, no remap needed | OK |
| `language.go` lang card edits (2) | EditText | `admin_system.cpp` lang cb (lines ~685-710) | OK |
| `start.go` / `help.go` CTA + help edits | EditText | `start.cpp` / `help.cpp` | OK* |
| `auth.go:258` authorized-list card after add/del | EditText | `admin_lists.cpp` (3 edit sites) | OK* |
| `sudoers.go` sudo list renders (3) | EditText | `admin_lists.cpp` sudo sections | OK* |
| `reload.go:108,131` reload_done edit | EOR | `admin_system.cpp handle_reload_cmd` (LiveMsg) | OK* |
| `restart.go` 4 status edits (no real exec) | EOR | `admin_system.cpp` confirm cb edits; actual exec NOT implemented in this build (logged) | approx |
| `callback.go` playback buttons (pause/resume/mute/volume/shuffle/loop/seek...) edit the clicked message | EditText on callback message | `vc_control.cpp` / `handlers.cpp` cb edits | OK (real msg id from callback payload — never affected by the sync-send id problem) |
| `broadcast.go:351,382,457` progress edits | EditText | `admin_system.cpp` broadcast | OK* |
| `monitor.go:77` status ticker EditText | periodic live edits | — | missing (monitor cmd not ported) |
| room status msg live progress ticker + auto-next now-playing edits (Go `room.go`/`playTrackWithRetry`) | ticker edits status msg every ~N s; on track end auto-plays next and edits card | — | missing (C++ room has no track-end poll; queue advances only via /skip /stop; no status ticker) |

## 4. Rules for new/changed flows

1. Never `send_text` again where Go edits: use `LiveMsg`.
2. Long-running work (download etc.): call `download_with_status(lm, ...)` so
   progress + `/cancel` wiring come for free; map `err=="canceled"` to
   `play_download_canceled` with the requester mention.
3. If you add a NEW way to send a message that later gets edited, use
   `send_text_sync` (or `LiveMsg`), never the fire-and-forget `send_text` —
   otherwise you will hold no id and the edit falls back to spamming messages.
4. Text used for TDLib correlation MUST be the parseTextEntities plain text —
   already centralized in `send_text_sync`; don't re-implement matching.
5. When porting a Go edit site, add it to the matrix above with its status.
