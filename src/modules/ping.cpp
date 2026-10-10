#include "advait/modules/modules.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/config.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/telegram/tdlib.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <sys/statvfs.h>
#include <unistd.h>
namespace advait::modules {

static std::string ram_info() {
    std::ifstream in("/proc/meminfo");
    std::string k;
    long mem_total = 0, mem_avail = 0, v = 0;
    std::string unit;
    while (in >> k >> v >> unit) {
        if (k == "MemTotal:") mem_total = v;
        if (k == "MemAvailable:") mem_avail = v;
        if (mem_total && mem_avail) break;
    }
    if (!mem_total) return "N/A";
    double used = static_cast<double>(mem_total - mem_avail) / 1024.0 / 1024.0;
    double total = static_cast<double>(mem_total) / 1024.0 / 1024.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f / %.2f GB", used, total);
    return buf;
}

static std::string disk_info() {
    struct statvfs st {};
    if (statvfs("/", &st) != 0) return "N/A";
    double total = static_cast<double>(st.f_blocks) * static_cast<double>(st.f_frsize) / 1024.0 / 1024.0 / 1024.0;
    double free = static_cast<double>(st.f_bavail) * static_cast<double>(st.f_frsize) / 1024.0 / 1024.0 / 1024.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f / %.2f GB", total - free, total);
    return buf;
}

static std::string cpu_info() {
    std::ifstream in("/proc/loadavg");
    double load = 0;
    if (!(in >> load)) return "N/A";
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 1;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f%%", (load / static_cast<double>(n)) * 100.0);
    return buf;
}

void handle_ping(const tg::Message& m) {
    if (m.is_channel) return;
    auto& bot = tg::bot();
    if (m.is_private) {
        bot.delete_message(m.chat_id, m.id);
        db::add_served_user(m.chat_id);
    } else {
        db::add_served_chat(m.chat_id);
    }

    auto start = std::chrono::steady_clock::now();
    auto reply_id = bot.send_text_sync(m.chat_id, F(m.chat_id, "ping_start"), {},
                                       m.is_private ? 0 : m.id);
    if (reply_id != 0) bot.wait_sent(m.chat_id, reply_id);
    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - Config::get().start_time).count();
    auto msg = F(m.chat_id, "ping_result",
                 {{"latency", std::to_string(latency)},
                  {"bot", utils::html_mention(bot.me_first_name(), bot.me_id())},
                  {"uptime", utils::format_uptime(uptime)},
                  {"ram_info", ram_info()},
                  {"cpu_usage", cpu_info()},
                  {"disk_usage", disk_info()}});
    if (reply_id != 0) {
        if (!bot.edit_text(m.chat_id, reply_id, msg, supp_markup(m.chat_id))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            bot.edit_text(m.chat_id, reply_id, msg, supp_markup(m.chat_id));
        }
    } else {
        bot.send_text(m.chat_id, msg, supp_markup(m.chat_id));
    }
}

}  // namespace advait::modules
