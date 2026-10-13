#include "advait/modules/vc_control.hpp"

#include "advait/core/config.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/modules/common.hpp"
#include "advait/platforms/platforms.hpp"
#include "advait/telegram/tdlib.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace advait::modules {
namespace {

std::string command_of(const tg::Message& m) {
  auto fields = utils::split_fields(m.text);
  if (fields.empty()) return {};
  auto cmd = fields[0];
  auto at = cmd.find('@');
  if (at != std::string::npos) cmd = cmd.substr(0, at);
  return cmd;
}

std::string fmt_speed(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

bool parse_int_strict(const std::string& s, int* out) {
  int v = 0;
  auto res = std::from_chars(s.data(), s.data() + s.size(), v);
  if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) return false;
  *out = v;
  return true;
}

bool parse_double_strict(const std::string& s, double* out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end != s.c_str() + s.size()) return false;
  if (!std::isfinite(v)) return false;
  *out = v;
  return true;
}

bool is_owner_or_sudo(int64_t user_id) {
  auto owner = Config::get().owner_id;
  if (owner != 0 && user_id == owner) return true;
  return db::is_sudo(user_id);
}

// Go's filterAuthUsers also honours real chat-admin rights; tdlib.hpp here
// exposes no chat-admin query, so owner/sudo and auth users are the checks.
bool can_use_admin_command(const tg::Message& m) {
  if (is_owner_or_sudo(m.sender_id)) return true;
  return db::is_authorized(m.chat_id, m.sender_id);
}

bool filter_auth_users(const tg::Message& m) {
  if (can_use_admin_command(m)) return true;
  auto key = db::play_mode_admins_only(m.chat_id) ? "only_admin" : "only_admin_or_auth";
  tg::bot().reply_text(m, F(m.chat_id, key));
  return false;
}

std::mutex shuffle_mu_;
std::unordered_map<int64_t, bool> shuffle_flags_;

bool shuffle_flag(int64_t chat_id) {
  std::lock_guard<std::mutex> lk(shuffle_mu_);
  auto it = shuffle_flags_.find(chat_id);
  return it != shuffle_flags_.end() && it->second;
}

void set_shuffle_flag(int64_t chat_id, bool v) {
  std::lock_guard<std::mutex> lk(shuffle_mu_);
  shuffle_flags_[chat_id] = v;
}

RoomState* resolve_room(const tg::Message& m, bool cplay, std::string* err) {
  std::string room_err;
  auto* r = effective_room(m, cplay, &room_err);
  if (!r) {
    if (err) *err = room_err.empty() ? F(m.chat_id, "room_no_active") : room_err;
    return nullptr;
  }
  return r;
}

}  // namespace

void handle_position(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (cplay && !filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  auto t = r->track();
  if (!r->active() || !t || t->id.empty()) {
    bot.reply_text(m, F(chat_id, "room_no_active"));
    return;
  }
  bot.reply_text(m, F(chat_id, "position_now",
                      {{"title", utils::escape_html(utils::short_title(t->title, 25))},
                       {"position", utils::format_duration(r->position())},
                       {"duration", utils::format_duration(t->duration)},
                       {"speed", fmt_speed(r->speed())}}));
}

void handle_replay(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  if (!r->active()) {
    bot.reply_text(m, F(chat_id, "room_no_active"));
    return;
  }
  auto t = r->track();
  auto title = t ? utils::escape_html(utils::short_title(t->title, 25)) : "";
  auto duration = utils::format_duration(t ? t->duration : 0);
  if (!r->replay()) {
    bot.reply_text(m, F(chat_id, "replay_failed", {{"error", "replay"}}));
    return;
  }
  bot.reply_text(m, F(chat_id, "replay_success",
                      {{"title", title},
                       {"duration", duration},
                       {"speed", fmt_speed(r->speed())}}));
}

void handle_loop(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  auto fields = utils::split_fields(m.text);
  auto current = r->loop();

  if (!r->active()) {
    bot.reply_text(m, F(chat_id, "room_no_active"));
    return;
  }

  if (fields.size() < 2) {
    std::string count_line;
    if (current > 0) {
      count_line = "\n" + F(chat_id, "loop_current", {{"count", std::to_string(current)}});
    }
    bot.reply_text(m,
                   F(chat_id, "loop_usage", {{"cmd", command_of(m)}, {"count_line", count_line}}));
    return;
  }

  int new_loop = 0;
  if (!parse_int_strict(fields[1], &new_loop) || new_loop < 0 || new_loop > 10) {
    bot.reply_text(m, F(chat_id, "loop_invalid"));
    return;
  }

  if (new_loop == current) {
    bot.reply_text(m, F(chat_id, "loop_already_set", {{"count", std::to_string(current)}}));
    return;
  }

  r->set_loop(new_loop);
  if (new_loop == 0) {
    bot.reply_text(m, F(chat_id, "loop_disabled", {{"user", mention_of(m)}}));
  } else {
    bot.reply_text(m, F(chat_id, "loop_set",
                        {{"count", std::to_string(new_loop)}, {"user", mention_of(m)}}));
  }
}

void handle_speed(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  auto t = r->track();
  if (!r->active() || !t) {
    bot.reply_text(m, F(chat_id, "room_no_active"));
    return;
  }

  auto fields = utils::split_fields(m.text);
  auto cmd = command_of(m);

  if (fields.size() < 2) {
    if (r->speed() != 1.0) {
      bot.reply_text(m, F(chat_id, "speed_current",
                          {{"speed", fmt_speed(r->speed())},
                           {"title", utils::escape_html(utils::short_title(t->title, 25))},
                           {"cmd", cmd}}));
    } else {
      bot.reply_text(m, F(chat_id, "speed_usage", {{"cmd", cmd}}));
    }
    return;
  }

  auto raw = utils::to_lower(utils::trim(fields[1]));
  if (!raw.empty() && raw.back() == 'x') raw.pop_back();
  static const std::string kTimes = "\xC3\x97";
  if (raw.size() >= kTimes.size() &&
      raw.compare(raw.size() - kTimes.size(), kTimes.size(), kTimes) == 0) {
    raw.erase(raw.size() - kTimes.size());
  }

  double new_speed = 1.0;
  if (raw == "normal" || raw == "reset" || raw == "default") {
    new_speed = 1.0;
  } else if (!parse_double_strict(raw, &new_speed)) {
    bot.reply_text(m, F(chat_id, "speed_invalid_value", {{"cmd", cmd}}));
    return;
  } else if (new_speed < 0.50 || new_speed > 4.0) {
    bot.reply_text(m, F(chat_id, "speed_invalid_range"));
    return;
  }

  if (new_speed == r->speed()) {
    bot.reply_text(m, F(chat_id, "speed_already_set",
                        {{"speed", fmt_speed(new_speed)},
                         {"title", utils::escape_html(utils::short_title(t->title, 25))}}));
    return;
  }

  if (!r->set_speed(new_speed)) {
    bot.reply_text(m, F(chat_id, "speed_failed",
                        {{"speed", fmt_speed(new_speed)}, {"error", "speed"}}));
    return;
  }

  if (new_speed == 1.0) {
    bot.reply_text(m, F(chat_id, "speed_reset_success", {{"user", mention_of(m)}}));
  } else {
    bot.reply_text(m,
                   F(chat_id, "speed_set",
                     {{"speed", fmt_speed(new_speed)}, {"user", mention_of(m)}}));
  }
}

void handle_shuffle(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  auto arg = utils::to_lower(cmd_arg(m));

  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  if (!r->active()) {
    bot.reply_text(m, F(chat_id, "room_no_active"));
    return;
  }

  if (arg.empty()) {
    auto state = F(chat_id, shuffle_flag(chat_id) ? "enabled" : "disabled");
    auto cmd = command_of(m) + (shuffle_flag(chat_id) ? " off" : " on");
    bot.reply_text(m,
                   F(chat_id, "shuffle_current_state", {{"state", state}, {"cmd", cmd}}));
    return;
  }

  bool new_state = false;
  if (arg == "on" || arg == "enable" || arg == "true" || arg == "1") {
    new_state = true;
  } else if (arg == "off" || arg == "disable" || arg == "false" || arg == "0") {
    new_state = false;
  }

  // RoomState only exposes an immediate shuffle() action, so a per-chat flag
  // mirrors Go's get/set shuffle state while enabling reorders the queue.
  if (new_state && !shuffle_flag(chat_id)) r->shuffle();
  set_shuffle_flag(chat_id, new_state);

  bot.reply_text(m, F(chat_id, "shuffle_updated",
                      {{"state", F(chat_id, new_state ? "enabled" : "disabled")},
                       {"user", mention_of(m)}}));
}

void handle_seek(const tg::Message& m, bool cplay, bool is_back) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  auto t = r->track();
  if (!r->active() || !t) {
    bot.reply_text(m, F(chat_id, "seek_no_active"));
    return;
  }

  auto fields = utils::split_fields(m.text);
  auto cmd = command_of(m);
  if (fields.size() < 2) {
    bot.reply_text(m, F(chat_id, "seek_usage", {{"cmd", cmd}}));
    return;
  }

  int seconds = 0;
  if (!parse_int_strict(fields[1], &seconds)) {
    bot.reply_text(m, F(chat_id, "seek_invalid_seconds", {{"cmd", cmd}}));
    return;
  }

  std::string direction;
  std::string emoji;
  bool ok = false;
  if (is_back) {
    if (r->position() - seconds <= 10) {
      bot.reply_text(m,
                     F(chat_id, "seek_too_close_start", {{"seconds", std::to_string(seconds)}}));
      return;
    }
    ok = r->seek(-seconds);
    direction = "backward";
    emoji = "\xE2\x8F\xAA";
  } else {
    if (t->duration - (r->position() + seconds) <= 10) {
      bot.reply_text(m, F(chat_id, "seek_too_close_end", {{"seconds", std::to_string(seconds)}}));
      return;
    }
    ok = r->seek(seconds);
    direction = "forward";
    emoji = "\xE2\x8F\xA9";
  }

  if (!ok) {
    bot.reply_text(m, F(chat_id, "seek_failed",
                        {{"direction", direction},
                         {"seconds", std::to_string(seconds)},
                         {"error", "seek"}}));
    return;
  }
  bot.reply_text(m, F(chat_id, "seek_success",
                      {{"emoji", emoji},
                       {"direction", direction},
                       {"position", utils::format_duration(r->position())},
                       {"duration", utils::format_duration(t->duration)}}));
}

void handle_jump(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  auto t = r->track();
  if (!r->active() || !t) {
    bot.reply_text(m, F(chat_id, "jump_no_active"));
    return;
  }

  auto fields = utils::split_fields(m.text);
  auto cmd = command_of(m);
  if (fields.size() < 2) {
    bot.reply_text(m, F(chat_id, "jump_usage", {{"cmd", cmd}}));
    return;
  }

  int seconds = 0;
  if (!parse_int_strict(fields[1], &seconds) || seconds < 0) {
    bot.reply_text(m, F(chat_id, "jump_invalid_position", {{"cmd", cmd}}));
    return;
  }

  if (t->duration - seconds <= 10) {
    bot.reply_text(m,
                   F(chat_id, "jump_too_close_end",
                     {{"position", utils::format_duration(seconds)}}));
    return;
  }

  if (!r->seek(seconds - r->position())) {
    bot.reply_text(m, F(chat_id, "jump_failed",
                        {{"position", utils::format_duration(seconds)}, {"error", "seek"}}));
    return;
  }
  bot.reply_text(m, F(chat_id, "jump_success",
                      {{"position", utils::format_duration(seconds)},
                       {"duration", utils::format_duration(t->duration)}}));
}

void handle_remove(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  auto t = r->track();
  if (!r->active() || !t) {
    bot.reply_text(m, F(chat_id, "queue_no_active"));
    return;
  }
  auto total = static_cast<int>(r->queue().size());
  if (total == 0) {
    bot.reply_text(m, F(chat_id, "queue_empty"));
    return;
  }

  auto fields = utils::split_fields(m.text);
  if (fields.size() < 2) {
    bot.reply_text(m, F(chat_id, "remove_usage", {{"cmd", command_of(m)}}));
    return;
  }

  int index = 0;
  if (!parse_int_strict(fields[1], &index)) {
    bot.reply_text(m, F(chat_id, "remove_invalid_index"));
    return;
  }
  if (index <= 0) {
    bot.reply_text(m, F(chat_id, "remove_index_too_small"));
    return;
  }
  if (index > total) {
    bot.reply_text(m, F(chat_id, "remove_index_too_big", {{"total", std::to_string(total)}}));
    return;
  }

  r->remove_at(static_cast<size_t>(index - 1));
  bot.reply_text(m, F(chat_id, "remove_success",
                      {{"index", std::to_string(index)}, {"user", mention_of(m)}}));
}

void handle_clear(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  auto t = r->track();
  if (!r->active() || !t) {
    bot.reply_text(m, F(chat_id, "clear_no_active"));
    return;
  }
  if (r->queue().empty()) {
    bot.reply_text(m, F(chat_id, "queue_empty"));
    return;
  }

  r->clear_queue();
  bot.reply_text(m, F(chat_id, "clear_success", {{"user", mention_of(m)}}));
}

void handle_move(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  if (!r->active() || !r->track()) {
    bot.reply_text(m, F(chat_id, "queue_no_active"));
    return;
  }
  auto queue_len = static_cast<int>(r->queue().size());
  if (queue_len == 0) {
    bot.reply_text(m, F(chat_id, "queue_empty"));
    return;
  }

  auto fields = utils::split_fields(m.text);
  auto cmd = command_of(m);
  if (fields.size() < 3) {
    bot.reply_text(m, F(chat_id, "move_usage", {{"cmd", cmd}}));
    return;
  }

  int from = 0;
  int to = 0;
  if (!parse_int_strict(fields[1], &from) || !parse_int_strict(fields[2], &to)) {
    bot.reply_text(m, F(chat_id, "move_invalid_numbers", {{"cmd", cmd}}));
    return;
  }
  if (from <= 0 || to <= 0) {
    bot.reply_text(m, F(chat_id, "move_invalid_indexes_min"));
    return;
  }
  if (from > queue_len || to > queue_len) {
    bot.reply_text(m,
                   F(chat_id, "move_invalid_indexes_max",
                     {{"queue_len", std::to_string(queue_len)}}));
    return;
  }

  r->move(static_cast<size_t>(from - 1), static_cast<size_t>(to - 1));
  bot.reply_text(m, F(chat_id, "move_success",
                      {{"from", std::to_string(from)},
                       {"to", std::to_string(to)},
                       {"user", mention_of(m)}}));
}

void handle_mute(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;
  if (!cmd_arg(m).empty()) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  if (!r->active()) {
    bot.reply_text(m, F(chat_id, "room_no_active"));
    return;
  }
  if (r->muted()) {
    bot.reply_text(m, F(chat_id, "mute_already_muted"));
    return;
  }

  auto t = r->track();
  auto title = t ? utils::escape_html(utils::short_title(t->title, 25)) : "";
  if (!r->mute()) {
    bot.reply_text(m, F(chat_id, "mute_failed", {{"error", "mute"}}));
    return;
  }
  bot.reply_text(m, F(chat_id, "mute_success", {{"title", title}, {"user", mention_of(m)}}));
}

void handle_unmute(const tg::Message& m, bool cplay) {
  if (!cmd_arg(m).empty()) return;
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  if (!r->active()) {
    bot.reply_text(m, F(chat_id, "room_no_active"));
    return;
  }
  if (!r->muted()) {
    bot.reply_text(m, F(chat_id, "unmute_already"));
    return;
  }

  auto t = r->track();
  auto title = t ? utils::escape_html(utils::short_title(t->title, 25)) : "";
  if (!r->unmute()) {
    bot.reply_text(m, F(chat_id, "unmute_failed", {{"error", "unmute"}}));
    return;
  }

  std::string speed_opt;
  if (r->speed() != 1.0) {
    speed_opt = F(chat_id, "speed_line", {{"speed", fmt_speed(r->speed())}});
  }
  bot.reply_text(m, F(chat_id, "unmute_success",
                      {{"title", title},
                       {"user", mention_of(m)},
                       {"speed_line", speed_opt}}));
}

void handle_autoplay_skip(const tg::Message& m, bool cplay) {
  if (m.is_private || m.is_channel) {
    tg::bot().reply_text(m, F(m.chat_id, "only_supergroup"));
    return;
  }
  if (!filter_auth_users(m)) return;

  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  std::string room_err;
  auto* r = resolve_room(m, cplay, &room_err);
  if (!r) {
    bot.reply_text(m, room_err);
    return;
  }
  if (!r->active()) {
    bot.reply_text(m, F(chat_id, "room_no_active"));
    return;
  }
  if (!r->autoplay()) {
    bot.reply_text(m, F(chat_id, "askip_disabled"));
    return;
  }
  auto last = r->track();
  if (!last) {
    bot.reply_text(m, F(chat_id, "room_no_active"));
    return;
  }

  LiveMsg lm;
  lm.send(F(chat_id, "stream_downloading_next"));

  Track next{};
  bool found = false;
  {
    std::vector<Track> pool;
    for (auto& cand : platforms::autoplay_tracks(*last, 5, r->played_history())) {
      if (cand.id.empty() && cand.url.empty()) continue;
      pool.push_back(cand);
    }
    if (!pool.empty()) {
      next = platforms::golden_zone_pick(pool);
      found = true;
    }
  }
  if (!found) {
    lm.eor(F(chat_id, "askip_no_recommendations"));
    return;
  }

  auto requester = F(chat_id, "autoplay_requester");
  if (requester.empty() || requester[0] == '[') {
    requester = "\xF0\x9F\x8E\xB5 \xE1\xB4\x80\xE1\xB4\x9C\xE1\xB4\x9B\xE1\xB4\x8F\xE1\xB4\x98\xCA\x9F\xE1\xB4\x80\xCA\x8F";
  }
  next.mark_autoplay(requester, r->engine_token());

  r->set_loop(0);
  std::string dl_err;
  auto path = download_with_status(lm, chat_id, next, &dl_err);
  if (path.empty()) {
    lm.eor(F(chat_id, "stream_download_fail",
             {{"error", dl_err.empty() ? "download" : dl_err}}));
    drop_room(r->id);
    return;
  }
  if (!r->play(next, path, true)) {
    lm.eor(F(chat_id, "stream_play_fail"));
    drop_room(r->id);
    return;
  }
  lm.id = 0;
  show_track(lm, chat_id, r, next, now_playing_text(chat_id, next), false);
}

namespace {

std::string active_status_cell(const RoomState* r) {
  if (r->muted()) return "<b>\xF0\x9F\x94\x87 Muted</b>";
  if (r->paused()) return "<b>\xE2\x8F\xB8 Paused</b>";
  auto t = r->track();
  if (t) {
    if (t->video) return "<b>\xE2\x96\xB6\xEF\xB8\x8F Playing (Video)</b>";
    return "<b>\xE2\x96\xB6\xEF\xB8\x8F Playing (Audio)</b>";
  }
  return "<b>\xE2\x8F\xB9 Idle</b>";
}

std::string active_track_cell(const RoomState* r) {
  auto t = r->track();
  if (!t) return "<i>\xF0\x9F\x94\x87 No song playing.</i>";
  auto title = utils::escape_html(utils::short_title(t->title, 25));
  auto duration = utils::format_duration(t->duration);
  return title + " <i>(" + duration + ")</i>";
}

std::string active_queue_cell(const RoomState* r) {
  auto n = r->queue().size();
  if (n == 0) return "<i>Empty</i>";
  return "<b>" + std::to_string(n) + "</b>";
}

}  // namespace

void handle_active(const tg::Message& m) {
  auto& bot = tg::bot();
  auto chat_id = m.chat_id;
  if (!is_owner_or_sudo(m.sender_id)) {
    bot.reply_text(m, F(chat_id, "only_sudo"));
    return;
  }

  std::vector<int64_t> ids;
  auto rooms = all_rooms();
  for (auto& [id, r] : rooms) {
    if (!r || r->destroyed() || !r->active()) continue;
    ids.push_back(id);
  }

  if (ids.empty()) {
    bot.reply_text(m,
                   "<b>\xF0\x9F\x8E\xB5 Active Voice Chats</b>\n\n"
                   "No active chat sessions found.");
    return;
  }

  std::sort(ids.begin(), ids.end());

  std::string msg = "<b>\xF0\x9F\x8E\xB5 Active Voice Chats</b>\n\n";
  msg += "There are currently <b>" + std::to_string(ids.size()) +
         "</b> active voice/video chat(s) running.\n\n";
  msg += "<b>\xF0\x9F\x93\x8A Active Chats</b>\n";
  for (size_t i = 0; i < ids.size(); ++i) {
    auto it = rooms.find(ids[i]);
    if (it == rooms.end() || !it->second) continue;
    const auto* r = it->second;
    msg += std::to_string(i + 1) + ". <code>" + std::to_string(ids[i]) + "</code> \xE2\x80\x94 " +
           active_status_cell(r) + " \xE2\x80\x94 " + active_track_cell(r) + " \xE2\x80\x94 " +
           active_queue_cell(r) + "\n";
  }
  msg += "Total active: " + std::to_string(ids.size());
  bot.reply_text(m, msg);
}

void init_vc_control(tg::TdClient& bot) {
  bot.on_command("position", [](const tg::Message& m) { handle_position(m, false); });
  bot.on_command("pos", [](const tg::Message& m) { handle_position(m, false); });
  bot.on_command("cposition", [](const tg::Message& m) { handle_position(m, true); });
  bot.on_command("cpos", [](const tg::Message& m) { handle_position(m, true); });
  bot.on_command("replay", [](const tg::Message& m) { handle_replay(m, false); });
  bot.on_command("creplay", [](const tg::Message& m) { handle_replay(m, true); });
  bot.on_command("loop", [](const tg::Message& m) { handle_loop(m, false); });
  bot.on_command("setloop", [](const tg::Message& m) { handle_loop(m, false); });
  bot.on_command("cloop", [](const tg::Message& m) { handle_loop(m, true); });
  bot.on_command("csetloop", [](const tg::Message& m) { handle_loop(m, true); });
  bot.on_command("speed", [](const tg::Message& m) { handle_speed(m, false); });
  bot.on_command("setspeed", [](const tg::Message& m) { handle_speed(m, false); });
  bot.on_command("speedup", [](const tg::Message& m) { handle_speed(m, false); });
  bot.on_command("cspeed", [](const tg::Message& m) { handle_speed(m, true); });
  bot.on_command("csetspeed", [](const tg::Message& m) { handle_speed(m, true); });
  bot.on_command("cspeedup", [](const tg::Message& m) { handle_speed(m, true); });
  bot.on_command("shuffle", [](const tg::Message& m) { handle_shuffle(m, false); });
  bot.on_command("cshuffle", [](const tg::Message& m) { handle_shuffle(m, true); });
  bot.on_command("seek", [](const tg::Message& m) { handle_seek(m, false, false); });
  bot.on_command("cseek", [](const tg::Message& m) { handle_seek(m, true, false); });
  bot.on_command("seekback", [](const tg::Message& m) { handle_seek(m, false, true); });
  bot.on_command("cseekback", [](const tg::Message& m) { handle_seek(m, true, true); });
  bot.on_command("jump", [](const tg::Message& m) { handle_jump(m, false); });
  bot.on_command("cjump", [](const tg::Message& m) { handle_jump(m, true); });
  bot.on_command("remove", [](const tg::Message& m) { handle_remove(m, false); });
  bot.on_command("cremove", [](const tg::Message& m) { handle_remove(m, true); });
  bot.on_command("clear", [](const tg::Message& m) { handle_clear(m, false); });
  bot.on_command("cclear", [](const tg::Message& m) { handle_clear(m, true); });
  bot.on_command("move", [](const tg::Message& m) { handle_move(m, false); });
  bot.on_command("cmove", [](const tg::Message& m) { handle_move(m, true); });
  bot.on_command("mute", [](const tg::Message& m) { handle_mute(m, false); });
  bot.on_command("cmute", [](const tg::Message& m) { handle_mute(m, true); });
  bot.on_command("unmute", [](const tg::Message& m) { handle_unmute(m, false); });
  bot.on_command("cunmute", [](const tg::Message& m) { handle_unmute(m, true); });
  bot.on_command("askip", [](const tg::Message& m) { handle_autoplay_skip(m, false); });
  bot.on_command("caskip", [](const tg::Message& m) { handle_autoplay_skip(m, true); });
  bot.on_command("ac", [](const tg::Message& m) { handle_active(m); });
  bot.on_command("active", [](const tg::Message& m) { handle_active(m); });
  bot.on_command("activevc", [](const tg::Message& m) { handle_active(m); });
  bot.on_command("activevoice", [](const tg::Message& m) { handle_active(m); });
}

}  // namespace advait::modules
