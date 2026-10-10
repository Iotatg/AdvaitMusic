#include "advait/modules/modules.hpp"
#include "help_texts.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/utils.hpp"
#include "advait/telegram/tdlib.hpp"

#include <sstream>
#include <vector>

namespace advait::modules {
namespace {

std::vector<std::string> fields(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) out.push_back(tok);
    return out;
}

void show_help_for(const tg::Message& m, const std::string& cmd) {
    auto help_text = format_help_text(cmd);
    if (help_text.empty()) {
        auto trimmed = cmd;
        if (!trimmed.empty() && trimmed[0] == '/') trimmed.erase(0, 1);
        help_text = format_help_text(trimmed);
    }
    if (help_text.empty()) {
        tg::bot().reply_text(m,
                             "⚠️ <i>No help found for command <code>" + utils::escape_html(cmd) + "</code></i>");
        return;
    }
    tg::bot().reply_text(m, "📘 <b>Help for</b> <code>" + utils::escape_html(cmd) + "</code>:\n\n" + help_text);
}

}  // namespace

void handle_help(const tg::Message& m) {
    if (m.is_channel) return;
    auto& bot = tg::bot();
    auto args = fields(m.text);
    if (args.size() > 1 && args[1] != "pm_help") {
        auto cmd = args[1];
        if (cmd.empty() || cmd[0] != '/') cmd = "/" + cmd;
        show_help_for(m, cmd);
        return;
    }
    if (!m.is_private) {
        bot.reply_text(m, F(m.chat_id, "help_private_only"), group_help_keyboard(m.chat_id));
        return;
    }
    bot.reply_text(m, F(m.chat_id, "help_main"), help_keyboard(m.chat_id));
}

void handle_help_cb(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    bot.answer_callback(cb.query_id);
    bot.edit_text(cb.chat_id, cb.message_id, F(cb.chat_id, "help_main"), help_keyboard(cb.chat_id));
}

void handle_help_page_cb(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    bot.answer_callback(cb.query_id);
    if (cb.data.empty()) return;
    auto sep = cb.data.find(':');
    if (sep == std::string::npos) return;
    auto page = cb.data.substr(sep + 1);
    auto chat_id = cb.chat_id;
    std::string text;
    auto mk = back_keyboard(chat_id);
    if (page == "admin") text = F(chat_id, "help_admin");
    else if (page == "auth") text = F(chat_id, "help_auth");
    else if (page == "bcast") text = F(chat_id, "help_bcast");
    else if (page == "play") text = F(chat_id, "help_play");
    else if (page == "sudo") text = F(chat_id, "help_sudo");
    else if (page == "restrict") text = F(chat_id, "help_restrict");
    else if (page == "thumb") text = F(chat_id, "help_thumb");
    else if (page == "start") text = F(chat_id, "help_start");
    else if (page == "autoplay") text = F(chat_id, "help_autoplay");
    else if (page == "playlist") text = F(chat_id, "help_playlist");
    else if (page == "vclogs") text = F(chat_id, "help_vclogs");
    else if (page == "inline") text = F(chat_id, "help_inline");
    else if (page == "main") {
        text = F(chat_id, "help_main");
        mk = help_keyboard(chat_id);
    }
    if (text.empty()) return;
    bot.edit_text(cb.chat_id, cb.message_id, text, mk);
}

}  // namespace advait::modules
