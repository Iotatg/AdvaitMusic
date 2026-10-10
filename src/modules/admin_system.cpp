#include "advait/modules/admin_system.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/config.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/room.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/modules/common.hpp"
#include "advait/ntgcalls/client.hpp"
#include "advait/telegram/tdlib.hpp"

#include <nlohmann/json.hpp>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace advait::modules {
namespace {

std::string fmt1(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

std::string get_command(const tg::Message& m) {
    auto fields = utils::split_fields(m.text);
    if (fields.empty()) return {};
    auto cmd = fields[0];
    auto at = cmd.find('@');
    if (at != std::string::npos) cmd = cmd.substr(0, at);
    return cmd;
}

std::optional<bool> strict_bool(const std::string& raw) {
    auto v = utils::to_lower(utils::trim(raw));
    if (v == "on" || v == "enable" || v == "enabled" || v == "true" || v == "1" || v == "yes" || v == "y")
        return true;
    if (v == "off" || v == "disable" || v == "disabled" || v == "false" || v == "0" || v == "no" || v == "n")
        return false;
    return std::nullopt;
}

bool require_owner(const tg::Message& m) {
    auto& cfg = Config::get();
    if (cfg.owner_id == 0 || m.sender_id != cfg.owner_id) {
        tg::bot().reply_text(m, F(m.chat_id, "only_owner"));
        return false;
    }
    return true;
}

bool require_owner_or_sudo(const tg::Message& m) {
    auto& cfg = Config::get();
    if ((cfg.owner_id != 0 && m.sender_id == cfg.owner_id) || db::is_sudo(m.sender_id)) return true;
    tg::bot().reply_text(m, F(m.chat_id, "only_sudo"));
    return false;
}

bool group_gate(const tg::Message& m) {
    auto& bot = tg::bot();
    if (m.is_private) {
        bot.reply_text(m, F(m.chat_id, "only_supergroup"));
        db::add_served_user(m.chat_id);
        return false;
    }
    if (m.is_channel) return false;
    db::add_served_chat(m.chat_id);
    return true;
}

// Go uses canUseAdminCommand (real chat-admin lookup); the TDLib client here
// exposes no admin check, so owner/sudo/authorized users pass (see NOTES).
bool privileged_gate(int64_t chat_id, int64_t user_id) {
    auto& cfg = Config::get();
    if (cfg.owner_id != 0 && user_id == cfg.owner_id) return true;
    if (db::is_sudo(user_id)) return true;
    return db::is_authorized(chat_id, user_id);
}

bool privileged_msg(const tg::Message& m) {
    if (privileged_gate(m.chat_id, m.sender_id)) return true;
    tg::bot().reply_text(m, F(m.chat_id, "only_admin_or_auth"));
    return false;
}

std::mutex g_flood_mu;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_flood;

int flood_left(const std::string& key) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_flood_mu);
    auto it = g_flood.find(key);
    if (it == g_flood.end() || it->second <= now) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(it->second - now).count());
}

void flood_set(const std::string& key, int seconds) {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::lock_guard<std::mutex> lk(g_flood_mu);
    g_flood[key] = until;
}

// No db API for these flags in the C++ build; process-local state (see NOTES).
std::mutex g_vc_mu;
std::mutex g_state_mu;
std::unordered_map<int64_t, bool> g_cleanmode;
std::unordered_map<int64_t, bool> g_cmddelete;
std::unordered_map<int64_t, int> g_clean_duration;
std::unordered_map<int64_t, std::string> g_adminmode;
std::unordered_map<int64_t, bool> g_vclogger;
std::unordered_set<int64_t> g_vclogger_running;
std::string g_maint_reason;
std::atomic<bool> g_logger_enabled{false};
std::atomic<bool> g_maint_cancel{false};

std::string maint_get_reason() {
    std::lock_guard<std::mutex> lk(g_state_mu);
    return g_maint_reason;
}

void maint_set_reason(const std::string& reason) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_maint_reason = reason;
}

bool state_flag(const std::unordered_map<int64_t, bool>& map_, int64_t chat_id) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    auto it = map_.find(chat_id);
    return it != map_.end() && it->second;
}

void state_flag_set(std::unordered_map<int64_t, bool>& map_, int64_t chat_id, bool v) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    map_[chat_id] = v;
}

std::string adminmode_get(int64_t chat_id) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    auto it = g_adminmode.find(chat_id);
    return it != g_adminmode.end() ? it->second : std::string("adminauth");
}

void adminmode_set(int64_t chat_id, const std::string& mode) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_adminmode[chat_id] = mode;
}

std::string adminmode_status_key(const std::string& mode) {
    if (mode == "admins") return "adminmode_status_admin";
    if (mode == "everyone") return "adminmode_status_everyone";
    return "adminmode_status_adminauth";
}

// ---------------------------------------------------------------- dev / eval

void handle_eval(const tg::Message& m) {
    if (!require_owner(m)) return;
    tg::bot().reply_text(m, "Eval is not available in this C++ build (Go snippet evaluation was not ported).");
}

void handle_shell(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner_or_sudo(m)) return;
    auto command = cmd_arg(m);
    if (command.empty()) {
        bot.reply_text(m, "No command provided");
        return;
    }
    auto merged = command + " 2>&1";
    std::string out;
    bool truncated = false;
    constexpr size_t kCap = 4000;
    FILE* pipe = popen(merged.c_str(), "r");
    if (!pipe) {
        bot.reply_text(m, "<code>Error:</code> <b>failed to spawn command</b>");
        return;
    }
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        if (out.size() >= kCap) {
            truncated = true;
            continue;
        }
        out.append(buf);
    }
    int status = pclose(pipe);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    auto text = utils::trim(out);
    if (text.empty()) {
        if (code != 0) {
            bot.reply_text(m,
                           "<code>Error:</code> <b>command exited with code " + std::to_string(code) + "</b>");
        } else {
            bot.reply_text(m, "<code>No Output</code>");
        }
        return;
    }
    if (text.size() > kCap) {
        text.resize(kCap);
        truncated = true;
    }
    bot.reply_text(m,
                   "<pre lang=\"bash\">" + utils::escape_html(text) + (truncated ? "\n... (output truncated)" : "") +
                       "</pre>");
}

void handle_json(const tg::Message& m) {
    auto& bot = tg::bot();
    if (m.reply_to != 0) {
        bot.reply_text(m, "<code>Error:</code> <b>could not fetch replied message</b>");
        return;
    }
    auto dump = m.raw.dump(2);
    if (dump.size() > 3800) {
        dump.resize(3800);
        dump += "\n... (json truncated)";
    }
    bot.reply_text(m, "<pre lang='json'>" + utils::escape_html(dump) + "</pre>");
}

void handle_logs(const tg::Message& m) {
    auto& bot = tg::bot();
    std::error_code ec;
    auto size = std::filesystem::file_size("logs.txt", ec);
    if (ec || size == 0) {
        bot.reply_text(m, F(m.chat_id, "logs_empty"));
        return;
    }
    bot.reply_text(m, "<code>Error:</code> <b>document sending is not available in this build</b>");
}

// -------------------------------------------------------------------- logger

void handle_logger(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner_or_sudo(m)) return;
    auto fields = utils::split_fields(m.text);
    auto chat_id = m.chat_id;
    bool current = g_logger_enabled.load();
    auto action = F(chat_id, current ? "enabled" : "disabled");
    if (fields.size() < 2) {
        bot.reply_text(m, F(chat_id, "logger_usage",
                            {{"cmd", get_command(m)},
                             {"status", F(chat_id, "logger_status", {{"action", action}})}}));
        return;
    }
    auto enable = strict_bool(fields[1]);
    if (!enable) {
        bot.reply_text(m, F(chat_id, "invalid_bool"));
        return;
    }
    action = F(chat_id, *enable ? "enabled" : "disabled");
    if (current == *enable) {
        bot.reply_text(m, F(chat_id, "logger_already", {{"action", action}}));
        return;
    }
    g_logger_enabled.store(*enable);
    ADVAIT_LOG_INFO("logger", "logger toggled");
    bot.reply_text(m, F(chat_id, "logger_updated", {{"action", action}}));
}

// -------------------------------------------------------------- maintenance

void notify_maintenance_start(std::string reason) {
    for (auto& entry : all_rooms()) {
        if (g_maint_cancel.load()) break;
        auto chat_id = entry.first;
        drop_room(chat_id);
        auto msg = F(chat_id, "maint_entering");
        if (!reason.empty()) msg += "\n" + F(chat_id, "maint_reason", {{"reason", utils::escape_html(reason)}});
        tg::bot().send_text(chat_id, msg);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void handle_maintenance(const tg::Message& m) {
    if (!require_owner(m)) return;
    auto& bot = tg::bot();
    auto fields = utils::split_fields(m.text);
    auto chat_id = m.chat_id;
    bool current = db::maintenance();

    if (fields.size() < 2) {
        auto reason = maint_get_reason();
        std::string status = F(chat_id, "disabled");
        if (current) {
            status = reason.empty()
                         ? F(chat_id, "enabled")
                         : F(chat_id, "enabled_with_reason", {{"reason", utils::escape_html(reason)}});
        }
        bot.reply_text(m, F(chat_id, "maint_usage", {{"cmd", get_command(m)}, {"status", status}}));
        return;
    }

    auto enable = strict_bool(fields[1]);
    if (!enable) {
        bot.reply_text(m, F(chat_id, "invalid_bool"));
        return;
    }
    std::string reason;
    for (size_t i = 2; i < fields.size(); ++i) {
        if (!reason.empty()) reason += " ";
        reason += fields[i];
    }

    if (current == *enable) {
        auto old = maint_get_reason();
        if (!*enable) {
            bot.reply_text(m, F(chat_id, "maint_already_disabled"));
        } else if (reason == old) {
            bot.reply_text(m, F(chat_id, "maint_already_reason_same"));
        } else if (reason.empty() && !old.empty()) {
            maint_set_reason("");
            bot.reply_text(m, F(chat_id, "maint_reason_removed"));
        } else if (!reason.empty()) {
            maint_set_reason(reason);
            bot.reply_text(m, F(chat_id, "maint_reason_updated", {{"reason", utils::escape_html(reason)}}));
        } else {
            bot.reply_text(m, F(chat_id, "maint_already_enabled"));
        }
        return;
    }

    db::set_maintenance(*enable);
    maint_set_reason(*enable ? reason : "");
    ADVAIT_LOG_INFO("maintenance",
                   "user " + std::to_string(m.sender_id) + " set maintenance: " + (*enable ? "true" : "false"));
    g_maint_cancel.store(!*enable);
    if (*enable) {
        std::thread([reason]() { notify_maintenance_start(reason); }).detach();
        if (reason.empty()) {
            bot.reply_text(m, F(chat_id, "maint_enabled"));
        } else {
            bot.reply_text(m, F(chat_id, "maint_enabled_reason", {{"reason", utils::escape_html(reason)}}));
        }
    } else {
        bot.reply_text(m, F(chat_id, "maint_disabled"));
    }
}

// ------------------------------------------------------------------- reload

void handle_reload_cmd(const tg::Message& m, bool cplay) {
    if (!group_gate(m)) return;
    if (cplay && !privileged_msg(m)) return;
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    LiveMsg lm;
    lm.reply(m, F(chat_id, "reload_start"));
    auto err = Locales::instance().load("locales");
    auto summary = err.empty() ? std::string("locales reloaded") : err;
    lm.eor(F(chat_id, "reload_done", {{"summary", utils::escape_html(summary)}}));
}

// ------------------------------------------------------------------ restart

RoomState* active_room(int64_t chat_id) {
    auto* r = room_for(chat_id);
    if (!r || !r->active() || !r->track()) return nullptr;
    return r;
}

void handle_restart(const tg::Message& m) {
    if (!require_owner(m)) return;
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    if (active_room(chat_id)) {
        bot.reply_text(m, F(chat_id, "restart_confirm_running"), restart_confirm_markup(chat_id));
        return;
    }
    bot.reply_text(m, F(chat_id, "restart"));
    ADVAIT_LOG_INFO("restart", "restart requested by " + std::to_string(m.sender_id) +
                                 "; process restart is not performed in this build");
}

void restart_cb(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    auto& cfg = Config::get();
    if (cfg.owner_id == 0 || cb.user_id != cfg.owner_id) {
        bot.answer_callback(cb.query_id);
        return;
    }
    auto chat_id = cb.chat_id;
    if (cb.data.rfind("restart:", 0) != 0) {
        bot.answer_callback(cb.query_id);
        return;
    }
    auto action = cb.data.substr(8);
    if (action == "bot") {
        bot.answer_callback(cb.query_id);
        bot.edit_text(chat_id, cb.message_id, F(chat_id, "restart"));
        ADVAIT_LOG_INFO("restart", "restart confirmed via callback by " + std::to_string(cb.user_id) +
                                     "; process restart is not performed in this build");
        return;
    }
    if (action == "replay") {
        auto* r = active_room(chat_id);
        if (!r) {
            bot.answer_callback(cb.query_id);
            return;
        }
        if (!r->replay()) {
            bot.answer_callback(cb.query_id);
            return;
        }
        bot.edit_text(chat_id, cb.message_id, F(chat_id, "restart_confirm_replay_done"));
        bot.answer_callback(cb.query_id);
    }
}

// -------------------------------------------------------------------- stats

namespace {

double read_cpu_percent() {
    auto sample = []() -> std::pair<long long, long long> {
        std::ifstream in("/proc/stat");
        std::string tag;
        long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        in >> tag >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        long long busy = user + nice + system + irq + softirq + steal;
        long long total = busy + idle + iowait;
        return {busy, total};
    };
    auto [b1, t1] = sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto [b2, t2] = sample();
    auto dt = t2 - t1;
    if (dt <= 0) return 0.0;
    return static_cast<double>(b2 - b1) * 100.0 / static_cast<double>(dt);
}

std::pair<double, double> read_mem_gib() {
    std::ifstream in("/proc/meminfo");
    std::string k, unit;
    long long total = 0, avail = 0;
    while (in >> k) {
        long long v = 0;
        in >> v >> unit;
        if (k == "MemTotal:") total = v;
        else if (k == "MemAvailable:") avail = v;
        if (total && avail) break;
    }
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    double tot = static_cast<double>(total) * 1024.0 / kGiB;
    double used = static_cast<double>(total - avail) * 1024.0 / kGiB;
    return {used, tot};
}

std::pair<double, double> read_disk_gib() {
    struct statvfs vfs {};
    if (statvfs("/", &vfs) != 0) return {0.0, 0.0};
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    double total = static_cast<double>(vfs.f_blocks) * static_cast<double>(vfs.f_frsize) / kGiB;
    double used = static_cast<double>(vfs.f_blocks - vfs.f_bavail) * static_cast<double>(vfs.f_frsize) / kGiB;
    return {used, total};
}

std::pair<long long, long long> read_proc_mem_mb() {
    long long rss_mb = 0, vsz_mb = 0;
    std::ifstream st("/proc/self/statm");
    long long size_pages = 0, resident_pages = 0;
    if (st >> size_pages >> resident_pages) {
        long long page = sysconf(_SC_PAGESIZE);
        rss_mb = resident_pages * page / 1024 / 1024;
        vsz_mb = size_pages * page / 1024 / 1024;
    }
    return {rss_mb, vsz_mb};
}

int read_thread_count() {
    std::ifstream in("/proc/self/status");
    std::string k;
    int threads = 0;
    while (std::getline(in, k)) {
        if (k.rfind("Threads:", 0) == 0) {
            std::istringstream ss(k.substr(8));
            ss >> threads;
            break;
        }
    }
    return threads;
}

}  // namespace

void handle_stats(const tg::Message& m) {
    if (!require_owner_or_sudo(m)) return;
    auto chat_id = m.chat_id;

#if defined(__x86_64__)
    const char* arch = "amd64";
#elif defined(__aarch64__)
    const char* arch = "arm64";
#else
    const char* arch = "unknown";
#endif

    auto uptime_min = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - Config::get().start_time).count() / 60000000.0;
    auto gc_count = 0LL;
    double gc_per_min = uptime_min > 0 ? static_cast<double>(gc_count) / uptime_min : 0.0;
    const char* gc_emoji = "🟢";
    if (gc_per_min > 20) gc_emoji = "🔴";
    else if (gc_per_min > 10) gc_emoji = "🟠";

    double cpu = read_cpu_percent();
    const char* cpu_emoji = "🟢";
    if (cpu > 70) cpu_emoji = "🔴";
    else if (cpu > 40) cpu_emoji = "🟡";

    auto [ram_used, ram_total] = read_mem_gib();
    double ram_percent = ram_total > 0 ? ram_used / ram_total * 100.0 : 0.0;
    const char* ram_emoji = "🟢";
    if (ram_percent > 80) ram_emoji = "🔴";
    else if (ram_percent > 50) ram_emoji = "🟡";

    auto [disk_used, disk_total] = read_disk_gib();
    auto [alloc_mb, sys_mb] = read_proc_mem_mb();
    auto chats = db::served_chats().size();
    auto users = db::served_users().size();

    char buf[512];
    auto f2 = [](char* dst, size_t n, double v) {
        std::snprintf(dst, n, "%.2f", v);
        return std::string(dst);
    };

    auto text = F(chat_id, "stats_overview", {
                                                 {"os", "linux"},
                                                 {"arch", arch},
                                                 {"cpus", std::to_string(sysconf(_SC_NPROCESSORS_ONLN))},
                                                 {"goroutines", std::to_string(read_thread_count())},
                                                 {"alloc", std::to_string(alloc_mb)},
                                                 {"sys", std::to_string(sys_mb)},
                                                 {"gc_count", std::to_string(gc_count)},
                                                 {"gc_emoji", gc_emoji},
                                                 {"gc_rate", [&] {
                                                      std::snprintf(buf, sizeof(buf), "%.1f", gc_per_min);
                                                      return std::string(buf);
                                                  }()},
                                                 {"cpu_emoji", cpu_emoji},
                                                 {"cpu", f2(buf, sizeof(buf), cpu)},
                                                 {"ram_emoji", ram_emoji},
                                                 {"ram_used_gib", f2(buf, sizeof(buf), ram_used)},
                                                 {"ram_total_gib", f2(buf, sizeof(buf), ram_total)},
                                                 {"storage_used_gib", f2(buf, sizeof(buf), disk_used)},
                                                 {"storage_total_gib", f2(buf, sizeof(buf), disk_total)},
                                                 {"db_storage", f2(buf, sizeof(buf), 0.0)},
                                                 {"db_data", f2(buf, sizeof(buf), 0.0)},
                                                 {"db_collections", "0"},
                                                 {"db_objects", "0"},
                                                 {"served_chats", std::to_string(chats)},
                                                 {"served_users", std::to_string(users)},
                                                 {"go_version", "c++23 gcc " __VERSION__},
                                                 {"gogram_api_layer", tg::bot().tdlib_version()},
                                                 {"ntgcalls_version", advait::ntg::version()},
                                             });
    tg::bot().reply_text(m, text);
}

// ----------------------------------------------------------------- settings

nlohmann::json settings_markup(int64_t chat_id) {
    auto admin_status = F(chat_id, adminmode_status_key(adminmode_get(chat_id)));
    bool playmode_admins = db::play_mode_admins_only(chat_id);
    bool cmd_delete = state_flag(g_cmddelete, chat_id);
    bool clean_mode = state_flag(g_cleanmode, chat_id);
    int duration = 15;
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        auto it = g_clean_duration.find(chat_id);
        if (it != g_clean_duration.end()) duration = it->second;
    }
    if (duration <= 0) duration = 15;

    std::vector<std::vector<nlohmann::json>> rows;
    rows.push_back({data_btn(F(chat_id, "settings_btn_adminmode"), "info:adminmode"),
                    data_btn(admin_status, "set:adminmode")});
    rows.push_back({data_btn(F(chat_id, "settings_btn_playmode"), "info:playmode"),
                    data_btn(F(chat_id, playmode_admins ? "playmode_status_admins" : "playmode_status_everyone"),
                             "set:playmode")});
    rows.push_back({data_btn(F(chat_id, "settings_btn_cmddelete"), "info:cmddelete"),
                    data_btn(F(chat_id, cmd_delete ? "enabled" : "disabled"), "set:cmddelete")});
    rows.push_back({data_btn(F(chat_id, "settings_btn_cleanmode"), "info:cleanmode"),
                    data_btn(F(chat_id, clean_mode ? "enabled" : "disabled"), "set:cleanmode")});
    rows.push_back({data_btn(F(chat_id, "settings_btn_cleanduration"), "info:cleanduration"),
                    data_btn(std::to_string(duration) + "m", "set:cleanduration")});
    bool thumb_on = !db::thumbnails_disabled(chat_id);
    rows.push_back({data_btn(F(chat_id, "settings_btn_nothumb"), "info:nothumb"),
                    data_btn(F(chat_id, thumb_on ? "enabled" : "disabled"), "set:nothumb")});
    rows.push_back(
        {data_btn(F(chat_id, "settings_btn_lang"), "info:lang"), data_btn(F(chat_id, "name"), "lang:select")});
    rows.push_back({data_btn(F(chat_id, "CLOSE_BTN"), "close")});
    return markup_rows(rows);
}

std::string settings_main_text(int64_t chat_id) {
    return F(chat_id, "settings_main", {{"title", "Chat"}, {"id", std::to_string(chat_id)}});
}

void handle_settings(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!group_gate(m)) return;
    if (!privileged_msg(m)) return;
    bot.reply_text(m, settings_main_text(m.chat_id), settings_markup(m.chat_id));
}

void settings_cb(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    auto chat_id = cb.chat_id;
    const auto& data = cb.data;
    auto p = data.find(':');
    if (p == std::string::npos) {
        bot.answer_callback(cb.query_id);
        return;
    }
    auto action = data.substr(p + 1);
    auto q = action.find(':');
    if (q != std::string::npos) action.resize(q);

    if (!privileged_gate(chat_id, cb.user_id)) {
        bot.answer_callback(cb.query_id);
        return;
    }
    if (data.rfind("info:", 0) == 0) {
        bot.answer_callback(cb.query_id);
        return;
    }
    if (action == "main") {
        bot.answer_callback(cb.query_id);
        bot.edit_text(chat_id, cb.message_id, settings_main_text(chat_id), settings_markup(chat_id));
        return;
    }
    if (action == "playmode") {
        db::set_play_mode_admins_only(chat_id, !db::play_mode_admins_only(chat_id));
    } else if (action == "adminmode") {
        auto mode = adminmode_get(chat_id);
        adminmode_set(chat_id, mode == "admins" ? "adminauth" : (mode == "adminauth" ? "everyone" : "admins"));
    } else if (action == "cmddelete") {
        state_flag_set(g_cmddelete, chat_id, !state_flag(g_cmddelete, chat_id));
    } else if (action == "cleanmode") {
        state_flag_set(g_cleanmode, chat_id, !state_flag(g_cleanmode, chat_id));
    } else if (action == "cleanduration") {
        static const int k_opts[4] = {15, 30, 60, 5};
        int cur = 15;
        {
            std::lock_guard<std::mutex> lk(g_state_mu);
            auto it = g_clean_duration.find(chat_id);
            if (it != g_clean_duration.end()) cur = it->second;
        }
        int next = k_opts[0];
        for (int i = 0; i < 4; ++i) {
            if (k_opts[i] == cur) {
                next = k_opts[(i + 1) % 4];
                break;
            }
        }
        std::lock_guard<std::mutex> lk(g_state_mu);
        g_clean_duration[chat_id] = next;
    } else if (action == "nothumb") {
        db::set_thumbnails_disabled(chat_id, !db::thumbnails_disabled(chat_id));
    }
    bot.answer_callback(cb.query_id);
    bot.edit_text(chat_id, cb.message_id, settings_main_text(chat_id), settings_markup(chat_id));
}

// ------------------------------------------------------------------ privacy

void handle_privacy(const tg::Message& m) {
    if (m.is_channel) return;
    auto support = utils::escape_html(Config::get().support_chat);
    auto text = std::string("<b>") + "\xf0\x9f\x9b\xa1" + R"PRIV( Privacy Policy &amp; Data Handling</b>

Your privacy is important to us. This bot is designed with privacy in mind.

<b>)PRIV" +
                    "\xf0\x9f\x93\x8a" +
                    R"PRIV( Data We Collect</b>
<blockquote>We only store essential data required for the bot to function:
• <b>User & Chat IDs:</b> To identify groups and manage settings.
• <b>Preferences:</b> Language settings and bot configurations.
• <b>Access Control:</b> Authorized user lists for your group.</blockquote>

<b>)PRIV" +
                    "\xf0\x9f\x93\xa9" +
                    R"PRIV( Message Privacy</b>
<blockquote>• The bot <b>only</b> reads messages that start with a command (e.g., <code>/play</code>) or interactions with its own buttons.
• It <b>does not</b> read, store, or monitor your private conversations or general group messages.</blockquote>

<b>)PRIV" +
                    "\xf0\x9f\x8c\x90" +
                    R"PRIV( Third-Party Services</b>
<blockquote>• We use external services like <b>YouTube</b> and <b>Spotify</b> solely to search for and stream the music you request.
• No personal data is shared with these services beyond the search query itself.</blockquote>

<b>)PRIV" +
                    "\xf0\x9f\xa4\x9d" +
                    R"PRIV( Data Sharing</b>
<blockquote>• We <b>never</b> sell, share, or trade your data with third parties.
• All data is used strictly to provide and improve the bot's music streaming features.</blockquote>

<b>)PRIV" +
                    "\xe2\x9c\xa8" +
                    R"PRIV( Our Commitment</b>
This bot is an <a href="https://github.com/Iotatg/AdvaitMusic">open-source project</a> dedicated to providing a high-quality streaming experience while respecting user privacy.

<i>If you have any questions, feel free to join our <a href=")PRIV" +
                    support + R"PRIV(">Support Chat</a>.</i>)PRIV";
    tg::bot().reply_text(m, text);
}

// ---------------------------------------------------------------------- bug

void handle_bug(const tg::Message& m) {
    if (m.is_channel) return;
    auto& bot = tg::bot();
    auto chat_id = m.chat_id;
    auto reason = cmd_arg(m);
    if (reason.empty() && m.reply_to == 0) {
        bot.reply_text(m, F(chat_id, "bug_usage", {{"cmd", get_command(m)}}));
        return;
    }
    auto key = "bug:" + std::to_string(chat_id) + ":" + std::to_string(m.sender_id);
    auto left = flood_left(key);
    if (left > 0) {
        bot.reply_text(m, F(chat_id, "flood_minutes", {{"duration", utils::format_duration(left)}}));
        return;
    }
    flood_set(key, 300);
    if (m.reply_to != 0)
        ADVAIT_LOG_INFO("bug", "replied-message forwarding is not available in this build; text report only");
    auto chat_name = F(chat_id, "bug_private_chat");
    auto report = utils::escape_html(reason);
    if (report.empty()) report = F(chat_id, "bug_reply_only");
    auto msg = F(chat_id, "bug_report_format", {{"user", mention_of(m)},
                                                {"user_id", std::to_string(m.sender_id)},
                                                {"chat", chat_name},
                                                {"chat_id", std::to_string(chat_id)},
                                                {"report", report}});
    auto& cfg = Config::get();
    std::vector<int64_t> targets;
    if (cfg.logger_id != 0) targets.push_back(cfg.logger_id);
    if (cfg.owner_id != 0 && cfg.owner_id != cfg.logger_id) targets.push_back(cfg.owner_id);
    for (auto target : targets) bot.send_text(target, msg);
    bot.reply_text(m, F(chat_id, "bug_thanks"));
}

// --------------------------------------------------------------------- repo

void handle_repo(const tg::Message& m) {
    if (m.is_channel) return;
    auto& bot = tg::bot();
    if (m.is_private) {
        db::add_served_user(m.chat_id);
    } else {
        db::add_served_chat(m.chat_id);
    }
    auto name = bot.me_first_name();
    if (name.empty()) name = "Advait Music";
    bot.reply_text(m, F(m.chat_id, "repo_text", {{"bot", utils::escape_html(name)}}),
                   repo_markup(m.chat_id, Config::get().dev_url));
}

// ----------------------------------------------------------------- vclogger

bool vc_enabled(int64_t chat_id) {
    std::lock_guard<std::mutex> lk(g_vc_mu);
    auto it = g_vclogger.find(chat_id);
    return it != g_vclogger.end() && it->second;
}

void vc_set_enabled(int64_t chat_id, bool v) {
    std::lock_guard<std::mutex> lk(g_vc_mu);
    g_vclogger[chat_id] = v;
}

bool vc_running(int64_t chat_id) {
    std::lock_guard<std::mutex> lk(g_vc_mu);
    return g_vclogger_running.count(chat_id) > 0;
}

void vc_set_running(int64_t chat_id, bool v) {
    std::lock_guard<std::mutex> lk(g_vc_mu);
    if (v) {
        g_vclogger_running.insert(chat_id);
    } else {
        g_vclogger_running.erase(chat_id);
    }
}

// Stylized texts below are byte-for-byte copies of the Go vclogger.go strings;
// start/stop only tracks state here because no userbot participant monitor
// exists in the C++ build (see NOTES).
const std::string k_vc_anon = "⚠️ ᴀɴᴏɴʏᴍᴏᴜs ᴀᴅᴍɪɴ\n\nᴜsᴇ: <code>/vclogger on</code> ᴏʀ <code>/vclogger off</code>";
const std::string k_vc_already = "ℹ️ <b>𝐕ᴄ 𝐋σɢɢєꝛ 𝐈s 𝐀ʟꝛєᴧᴅʏ 𝐀ᴄᴛɪᴠє 𝐈η 𝐓ʜɪs 𝐆ꝛσυᴘ.</b>";
const std::string k_vc_enabled_msg = "✅ <b>𝐕ᴄ 𝐋σɢɢєꝛ 𝐇ᴧs 𝐁єєη 𝐄ηᴧʙʟєᴅ!</b>";
const std::string k_vc_disabled_msg = "🚫 <b>𝐕ᴄ 𝐋σɢɢєꝛ 𝐇ᴧs 𝐁єєη 𝐃ɪsᴧʙʟєᴅ!</b>";
const std::string k_vc_denied = "❌ <b>𝐏єꝛϻɪssɪση 𝐃єηɪєᴅ!</b>\n\n👮 <b>𝐎ηʟʏ 𝐀ᴅϻɪη 𝐎ꝛ 𝐎ᴡηєꝛs 𝐂ᴧη 𝐔sє 𝐕ᴄ 𝐋σɢɢєꝛ.</b>";
const std::string k_vc_invalid = "❌ <b>𝐈ηᴠᴧʟɪᴅ 𝐀ꝛɢυϻєηᴛ</b>\n\n➜ <b>𝐔sᴧɢє :</b> <code>/vclogger [on / off]</code>";
const std::string k_vc_state_on = "𝐄ηᴧʙʟєᴅ ✅";
const std::string k_vc_state_off = "𝐃ɪsᴧʙʟєᴅ ❌";
const std::string k_vc_status_a = "🎙 <b>𝐕ᴄ 𝐋σɢɢєꝛ 𝐒ᴛᴧᴛυs</b>\n\n➜ <b>𝐂υꝛꝛєηᴛ 𝐒ᴛᴧᴛє :</b> ";
const std::string k_vc_status_b = "\n\n➜ <b>𝐔sᴧɢє :</b> <code>/vclogger [on / off]</code>";
const std::string k_vc_admin_only = "❌ <b>ᴀᴅᴍɪɴ ᴏɴʟʏ!</b>";
const std::string k_vc_on = "✅ 𝐄ηᴧʙʟєᴅ";
const std::string k_vc_off = "❌ 𝐃ɪsᴧʙʟєᴅ";
const std::string k_vc_running_on = "🟢 𝐑υηηɪηɢ";
const std::string k_vc_running_off = "🔴 𝐒ᴛσᴘᴘєᴅ";
const std::string k_vc_stats_a = "📊 <b>𝐕ᴄ 𝐋σɢɢєꝛ 𝐒ᴛᴧᴛs</b>\n\n➜ <b>𝐋σɢɢєꝛ  :</b> ";
const std::string k_vc_stats_b = "\n➜ <b>𝐌σηɪᴛσꝛ :</b> ";
const std::string k_vc_stats_c = "\n➜ <b>𝐔sєꝛ 𝐈η 𝐕𝐂 :</b> <code>";

void handle_vclogger(const tg::Message& m) {
    auto& bot = tg::bot();
    if (m.is_private) {
        bot.reply_text(m, F(m.chat_id, "only_supergroup"));
        db::add_served_user(m.chat_id);
        return;
    }
    if (m.is_channel) return;
    db::add_served_chat(m.chat_id);
    auto chat_id = m.chat_id;
    auto fields = utils::split_fields(cmd_arg(m));

    if (m.sender_id == 0) {
        if (fields.empty()) {
            bot.reply_text(m, k_vc_anon);
            return;
        }
        auto arg = utils::to_lower(fields[0]);
        if (arg == "on" || arg == "enable" || arg == "yes") {
            if (vc_running(chat_id)) {
                bot.reply_text(m, k_vc_already);
                return;
            }
            vc_set_enabled(chat_id, true);
            vc_set_running(chat_id, true);
            bot.reply_text(m, k_vc_enabled_msg);
            return;
        }
        if (arg == "off" || arg == "disable" || arg == "no") {
            vc_set_enabled(chat_id, false);
            vc_set_running(chat_id, false);
            bot.reply_text(m, k_vc_disabled_msg);
        }
        return;
    }

    if (!privileged_gate(chat_id, m.sender_id)) {
        bot.reply_text(m, k_vc_denied);
        return;
    }

    if (fields.empty()) {
        auto state = vc_enabled(chat_id) ? k_vc_state_on : k_vc_state_off;
        bot.reply_text(m, k_vc_status_a + state + k_vc_status_b);
        return;
    }

    auto arg = utils::to_lower(fields[0]);
    if (arg == "on" || arg == "enable" || arg == "yes") {
        if (vc_running(chat_id)) {
            bot.reply_text(m, k_vc_already);
            return;
        }
        vc_set_enabled(chat_id, true);
        vc_set_running(chat_id, true);
        bot.reply_text(m, k_vc_enabled_msg);
    } else if (arg == "off" || arg == "disable" || arg == "no") {
        vc_set_enabled(chat_id, false);
        vc_set_running(chat_id, false);
        bot.reply_text(m, k_vc_disabled_msg);
    } else {
        bot.reply_text(m, k_vc_invalid);
    }
}

void handle_vcstats(const tg::Message& m) {
    auto& bot = tg::bot();
    if (m.is_private) {
        bot.reply_text(m, F(m.chat_id, "only_supergroup"));
        db::add_served_user(m.chat_id);
        return;
    }
    if (m.is_channel) return;
    auto chat_id = m.chat_id;
    if (m.sender_id != 0 && !privileged_gate(chat_id, m.sender_id)) {
        bot.reply_text(m, k_vc_admin_only);
        return;
    }
    auto enabled_text = vc_enabled(chat_id) ? k_vc_on : k_vc_off;
    auto running_text = vc_running(chat_id) ? k_vc_running_on : k_vc_running_off;
    // Go shows userbot-observed participant count; closest signal available is
    // the assistant presence inside a live room (see NOTES).
    auto* r = room_for(chat_id);
    auto count = (r && r->active()) ? 1 : 0;
    bot.reply_text(m,
                   k_vc_stats_a + enabled_text + k_vc_stats_b + running_text + k_vc_stats_c +
                       std::to_string(count) + "</code>");
}

// ---------------------------------------------------------------- broadcast

std::atomic<bool> g_bcast_running{false};
std::atomic<bool> g_bcast_cancel{false};

std::string bcast_progress(int64_t chat_id, int done_chats, int total_chats, int done_users, int total_users,
                           const std::vector<int64_t>& failed_chats, double delay_sec, int64_t elapsed_sec,
                           bool cancelled) {
    auto line = [&](const char* key, int done, int total) {
        double pct = total > 0 ? double(done) / double(total) * 100.0 : 0.0;
        return F(chat_id, key, {{"done", std::to_string(done)},
                                {"total", std::to_string(total)},
                                {"progress", fmt1(pct)}});
    };
    std::string out;
    if (cancelled) out += F(chat_id, "broadcast_cancel_done") + "\n\n";
    out += line("broadcast_total_chats", done_chats, total_chats) + "\n";
    out += line("broadcast_total_users", done_users, total_users) + "\n\n";
    if (!failed_chats.empty()) {
        out +=
            F(chat_id, "broadcast_failed_chats", {{"count", std::to_string(failed_chats.size())}}) + "\n";
        out += "\n";
    }
    out += F(chat_id, "broadcast_delay", {{"delay", fmt1(delay_sec)}}) + "\n";
    out += F(chat_id, "broadcast_elapsed", {{"elapsed", utils::format_duration(int(elapsed_sec))}}) + "\n";
    int total_targets = total_chats + total_users;
    int sent = done_chats + done_users;
    int failed = static_cast<int>(failed_chats.size());
    double rate = total_targets > 0 ? double(sent - failed) / double(total_targets) * 100.0 : 0.0;
    out += "\n\n" + F(chat_id, "broadcast_success_rate", {{"rate", fmt1(rate)},
                                                          {"sent", std::to_string(sent - failed)},
                                                          {"total", std::to_string(total_targets)}});
    return out;
}

bool parse_bcast_command(const tg::Message& m, bool* no_chat, bool* no_user, int* limit, double* delay,
                         std::string* content, std::string* err) {
    auto text = utils::trim(m.text);
    auto cmd = get_command(m);
    if (!cmd.empty() && text.rfind(cmd, 0) == 0) text = utils::trim(text.substr(cmd.size()));
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        auto nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    auto words = lines.empty() ? std::vector<std::string>{} : utils::split_fields(lines[0]);
    std::vector<std::string> content_words;
    for (size_t i = 0; i < words.size(); ++i) {
        auto w = utils::to_lower(words[i]);
        auto nv = i + 1 < words.size() ? words[i + 1] : std::string();
        if (w == "-nochat" || w == "--nochat") {
            *no_chat = true;
        } else if (w == "-nouser" || w == "--nouser") {
            *no_user = true;
        } else if (w == "-copy" || w == "--copy" || w == "-pin" || w == "--pin" || w == "-pinloud" ||
                   w == "--pinloud") {
            // forwarding/pinning is not available in this build (see NOTES)
        } else if (w == "-limit" || w == "--limit") {
            try {
                int v = std::stoi(nv);
                if (v < 0) throw std::invalid_argument("negative");
                *limit = v;
            } catch (...) {
                *err = "invalid limit value: " + nv;
                return false;
            }
            ++i;
        } else if (w == "-delay" || w == "--delay") {
            try {
                double v = std::stod(nv);
                if (v < 0) throw std::invalid_argument("negative");
                *delay = v;
            } catch (...) {
                *err = "invalid delay value: " + nv;
                return false;
            }
            ++i;
        } else if (w == "-cancel" || w == "--cancel") {
            // handled by the caller
        } else {
            content_words.push_back(words[i]);
        }
    }
    std::string c;
    for (auto& w : content_words) {
        if (!c.empty()) c += " ";
        c += w;
    }
    if (lines.size() > 1) {
        if (!c.empty()) c += "\n";
        for (size_t i = 1; i < lines.size(); ++i) {
            if (i > 1) c += "\n";
            c += lines[i];
        }
    }
    *content = utils::trim(c);
    return true;
}

void handle_broadcast(const tg::Message& m) {
    auto& bot = tg::bot();
    if (!require_owner(m)) return;
    auto chat_id = m.chat_id;

    if (utils::to_lower(m.text).find("-cancel") != std::string::npos) {
        if (!g_bcast_running.load()) {
            bot.reply_text(m, F(chat_id, "broadcast_not_running"));
            return;
        }
        g_bcast_cancel.store(true);
        bot.reply_text(m, F(chat_id, "broadcast_cancel_success"));
        return;
    }

    if (g_bcast_running.load()) {
        bot.reply_text(m, F(chat_id, "broadcast_already_running"));
        return;
    }

    bool no_chat = false;
    bool no_user = false;
    int limit = 0;
    double delay = 0.7;
    std::string content;
    std::string err;
    if (!parse_bcast_command(m, &no_chat, &no_user, &limit, &delay, &content, &err)) {
        bot.reply_text(m, F(chat_id, "broadcast_parse_failed", {{"error", utils::escape_html(err)}}));
        return;
    }
    if (content.empty()) {
        bot.reply_text(m, F(chat_id, "broadcast_no_content", {{"cmd", get_command(m)}}));
        return;
    }

    std::vector<int64_t> chats;
    std::vector<int64_t> users;
    if (!no_chat) chats = db::served_chats();
    if (!no_user) users = db::served_users();
    if (limit > 0 && static_cast<int>(chats.size() + users.size()) > limit) {
        if (static_cast<int>(chats.size()) >= limit) {
            chats.resize(size_t(limit));
            users.clear();
        } else {
            users.resize(size_t(limit - static_cast<int>(chats.size())));
        }
    }
    if (chats.empty() && users.empty()) {
        bot.reply_text(m, F(chat_id, "broadcast_no_targets"));
        return;
    }

    bool expected = false;
    if (!g_bcast_running.compare_exchange_strong(expected, true)) {
        bot.reply_text(m, F(chat_id, "broadcast_already_running"));
        return;
    }
    g_bcast_cancel.store(false);
    bot.reply_text(m, F(chat_id, "broadcast_initializing"), broadcast_cancel_keyboard(chat_id));

    std::thread([chat_id, content, chats, users, delay]() {
        try {
            int done_chats = 0;
            int done_users = 0;
            std::vector<int64_t> failed;
            auto start = std::chrono::steady_clock::now();
            int sent = 0;
            auto send_to = [&](const std::vector<int64_t>& targets, int& done) {
                for (auto id : targets) {
                    if (g_bcast_cancel.load()) return;
                    if (tg::bot().send_text(id, content)) {
                        ++done;
                    } else {
                        failed.push_back(id);
                    }
                    ++sent;
                    double d = sent % 30 == 0 ? 7.5 : delay;
                    auto until = std::chrono::steady_clock::now() +
                                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(d));
                    while (std::chrono::steady_clock::now() < until) {
                        if (g_bcast_cancel.load()) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                }
            };
            send_to(chats, done_chats);
            if (!g_bcast_cancel.load()) send_to(users, done_users);
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                            start)
                               .count();
            auto text = bcast_progress(chat_id, done_chats, static_cast<int>(chats.size()), done_users,
                                       static_cast<int>(users.size()), failed, delay, elapsed,
                                       g_bcast_cancel.load());
            tg::bot().send_text(chat_id, text);
        } catch (const std::exception& e) {
            ADVAIT_LOG_ERROR("broadcast", std::string("broadcast worker failed: ") + e.what());
        }
        g_bcast_running.store(false);
    }).detach();
}

void bcast_cancel_cb(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    if (Config::get().owner_id == 0 || cb.user_id != Config::get().owner_id) {
        bot.answer_callback(cb.query_id);
        return;
    }
    if (!g_bcast_running.load()) {
        bot.answer_callback(cb.query_id);
        return;
    }
    g_bcast_cancel.store(true);
    bot.answer_callback(cb.query_id);
    bot.send_text(cb.chat_id, F(cb.chat_id, "broadcast_cancel_done"));
}

}  // namespace

void init_admin_system(tg::TdClient& bot) {
    bot.on_command("ev", handle_eval);
    bot.on_command("eval", handle_eval);
    bot.on_command("sh", handle_shell);
    bot.on_command("bash", handle_shell);
    bot.on_command("shell", handle_shell);
    bot.on_command("json", handle_json);
    bot.on_command("log", handle_logs);
    bot.on_command("logs", handle_logs);
    bot.on_command("maintenance", handle_maintenance);
    bot.on_command("maint", handle_maintenance);
    bot.on_command("reload", [](const tg::Message& m) { handle_reload_cmd(m, false); });
    bot.on_command("creload", [](const tg::Message& m) { handle_reload_cmd(m, true); });
    bot.on_command("restart", handle_restart);
    bot.on_command("stats", handle_stats);
    bot.on_command("settings", handle_settings);
    bot.on_command("privacy", handle_privacy);
    bot.on_command("bug", handle_bug);
    bot.on_command("logger", handle_logger);
    bot.on_command("vclogger", handle_vclogger);
    bot.on_command("vclog", handle_vclogger);
    bot.on_command("vcstatus", handle_vcstats);
    bot.on_command("vcstats", handle_vcstats);
    bot.on_command("repo", handle_repo);
    bot.on_command("source", handle_repo);
    bot.on_command("broadcast", handle_broadcast);
    bot.on_command("gcast", handle_broadcast);
    bot.on_command("bcast", handle_broadcast);

    bot.on_callback("restart:", restart_cb);
    bot.on_callback("bcast_cancel", bcast_cancel_cb);
    bot.on_callback("set", settings_cb);
    bot.on_callback("info:", settings_cb);

    ADVAIT_LOG_INFO("modules", "admin system handlers registered");
}

}  // namespace advait::modules
