#include "advait/modules/modules.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/config.hpp"
#include "advait/core/locales.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/telegram/tdlib.hpp"

namespace advait::modules {

static std::string mention_of(const tg::Message& m) {
    auto name = utils::trim(m.first_name + " " + m.last_name);
    return utils::html_mention(name.empty() ? "user" : name, m.sender_id);
}

void handle_start(const tg::Message& m) {
    if (m.is_channel) return;
    auto& bot = tg::bot();
    if (!m.is_private) {
        db::add_served_chat(m.chat_id);
        bot.reply_text(m, F(m.chat_id, "start_group"));
        return;
    }
    db::add_served_user(m.chat_id);
    auto caption = F(m.chat_id, "start_private",
                     {{"user", mention_of(m)},
                      {"bot", utils::html_mention(bot.me_first_name(), bot.me_id())}});
    auto image = Config::get().start_image();
    if (!image.empty()) {
        bot.send_photo(m.chat_id, image, caption, start_markup(m.chat_id));
        return;
    }
    bot.send_text(m.chat_id, caption, start_markup(m.chat_id));
}

void handle_start_cb(const tg::CallbackQuery& cb) {
    auto& bot = tg::bot();
    bot.answer_callback(cb.query_id);
    auto cb_name = utils::trim(cb.first_name + " " + cb.last_name);
    auto caption = F(cb.chat_id, "start_private",
                     {{"user", utils::html_mention(cb_name.empty() ? "user" : cb_name, cb.user_id)},
                      {"bot", utils::html_mention(bot.me_first_name(), bot.me_id())}});
    bot.edit_text(cb.chat_id, cb.message_id, caption, start_markup(cb.chat_id));
}

}  // namespace advait::modules
