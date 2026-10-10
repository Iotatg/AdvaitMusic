# admin_system.cpp porting notes

Ported from Go modules: `dev.go` (logs/shell/json), `eval.go`, `maint.go`,
`reload.go`, `restart.go`, `stats.go`, `settings.go`, `privacy.go`, `bug.go`,
`logger.go`, `vclogger.go`, `broadcast.go`, `download.go` (repo/source).

## Registered commands (on_command)

`ev`, `eval`, `sh`, `bash`, `shell`, `json`, `log`, `logs`, `maintenance`,
`maint`, `reload`, `creload`, `restart`, `stats`, `settings`, `privacy`,
`bug`, `logger`, `vclogger`, `vclog`, `vcstatus`, `vcstats`, `repo`, `source`,
`broadcast`, `gcast`, `bcast`

## Registered callbacks (on_callback)

- prefix `restart:` — confirm menu (`restart:bot`, `restart:replay`)
- exact `set` — settings toggle callback
- prefix `info:` — handled by the same settings callback (Go shares it)
- exact `bcast_cancel` — broadcast cancel button

## Gaps vs Go

- **eval (`ev`/`eval`)**: Go evaluates Go snippets through a native VM. Not
  portable; replies with a fixed English note.
- **sh/bash/shell**: uses `popen("/bin/sh -c")`. Difference from Go:
  stdout and stderr are merged (`2>&1`) because popen gives one pipe; the
  exit code is reported in the error text when there is no output. Gate is
  owner-or-sudo as instructed (Go restricts to owner only).
- **log/logs**: file-missing/empty path replies `logs_empty`. There is no
  document-send API in this TdClient build, so when the log file exists the
  reply is the fixed text
  `<code>Error:</code> <b>document sending is not available in this build</b>`.
  Flags on the attached command are forwarded but their output is not
  captured separately (Go runs each flag as its own command).
- **json**: `-s/-m/-c` style flags from Go's json module were not ported; the
  command always dumps the current message JSON (`dump(2)`, capped at 3800
  chars). Replied-to message JSON fetch is unavailable (no message-fetch API),
  so replying yields Go's fetch-error wording instead.
- **maintenance reason**: `db::set_maintenance` takes no reason argument, so
  the reason is kept in a process-local map; it resets on restart.
- **reload/creload**: Go edits its progress message in place; `reply_text`
  returns no message id, so the port sends two messages
  (`reload_start`, then `reload_done` with the loader summary).
- **restart**: intentionally does **not** kill or re-exec the process (no
  supervisor exists here). It replies with Go's restart wording and logs the
  request; `restart:bot` edits the callback message and logs; `restart:replay`
  replays the active room via `Room::replay()` like Go.
- **stats**: MongoDB counters (users/chats/playlists/blacklists) and system
  RAM/disk numbers have no C++ data source — filled as `n/a`, and the locale
  template has no placeholders for uptime or active calls, so a two-line
  hardcoded English footer is appended
  (`<b>Uptime:</b> ...`, `<b>Active calls:</b> ...`).
- **settings**: no GetChat API, so menu title always uses the generic "Chat"
  fallback text. Missing DB setters are emulated with process-local state:
  adminmode (cycles admins -> adminauth -> everyone), cmddelete, cleanmode,
  cleanmode duration (cycles 15/30/60/5), and the logger switches. These maps
  are separate from the same-named maps inside `admin_lists.cpp` (that file
  could not be extended per the rules), so `/settings` and `/adminmode` can
  disagree until the integrator unifies them. Play-mode and thumbnail toggles
  do use the real DB setters. `lang:select` is left to admin_lists.cpp.
- **callback toasts**: `answer_callback` has no text parameter, so Go's
  alert-only texts (e.g. only-admin, settings-updated toasts) are omitted or
  logged instead of shown.
- **broadcast**: targets are served **chats only** (Go also fans out to users
  and channels with separate pacing). Forwarded-message broadcasts are not
  supported (no forward/quote API) — non-text broadcasts fail with
  `broadcast_no_content`. Live progress editing is not possible (no message
  id returned), so only the final progress summary is sent to the source
  chat. Cancellation works via the `bcast_cancel` inline button and a second
  broadcast invocation (Go behavior: owner only). Go's success-rate formula
  quirk is reproduced verbatim. Default delay 0.7s and the 30-message/7.5s
  pacing sleep are kept.
- **vclogger/vcstats**: no userbot participant monitor exists, so
  `start`/`stop`/`on`/`off` only flip in-memory state. `vcstats` participant
  count falls back to 1 when the assistant room is active, else 0 (Go reads
  live voice-chat members). All stylized Unicode response strings are
  byte-for-byte copies of the Go originals.
- **repo/source/bug/privacy**: public per Go. `repo`/`source` fall back to
  `Config::dev_url` with `buttons::repo_markup`; `bug` reuses `logger_id` then
  `owner_id` as targets with the Go 5-minute cooldown, and forwards are not
  attached (forward API absent) — forwarded bug reports are rejected the same
  way Go rejects them. `privacy` text is hardcoded (see below); the support
  link is taken from `Config::support_chat`.
- **gates**: Go's `filterAuthUsers`/`IsChatAdmin` need a chat-admin API that
  TdClient here does not expose. "admin/authorized" checks are approximated as
  owner || sudo user || authorized user.

## Hardcoded texts (no locale key exists in locales/en.yml)

- eval unavailable note (short English sentence)
- sh: `No command provided`, spawn-failure text, `<code>No Output</code>`
- json: fetch-error wording (copied from Go dev.go)
- logs: document-sending fallback line above
- maint usage/status: uses `maint_usage` with a locally built status string
- stats footer: `Uptime:` / `Active calls:` lines
- privacy policy body (long text copied from privacy.go, support link spliced
  from config)
- all `vclogger`/`vcstats` stylized strings (Go hardcodes them too)
