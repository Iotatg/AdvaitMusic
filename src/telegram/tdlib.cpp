#include "advait/telegram/tdlib.hpp"
#include "advait/core/buttons.hpp"
#include "advait/core/config.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/utils.hpp"

#include <dlfcn.h>
#include <unistd.h>
#include <atomic>
#include <chrono>

namespace advait::tg {

namespace {
TdClient* g_bot = nullptr;
}

TdClient& bot() {
    static TdClient c;
    g_bot = &c;
    return c;
}

TdClient::TdClient() = default;
TdClient::~TdClient() { close(); }

bool TdClient::load_tdjson() {
    const char* paths[] = {
        "./libtdjson.so",
        "./lib/libtdjson.so",
        "./libtdjson.so.1.8.66",
        "../AdvaitMusic/libtdjson.so.1.8.66",
        "/workspace/AdvaitMusic/libtdjson.so.1.8.66",
        "libtdjson.so",
        nullptr,
    };
    const char* env = std::getenv("TDJSON_PATH");
    if (env) handle_ = dlopen(env, RTLD_NOW | RTLD_GLOBAL);
    for (int i = 0; !handle_ && paths[i]; ++i) handle_ = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
    if (!handle_) {
        ADVAIT_LOG_ERROR("TDLib", std::string("libtdjson not found: ") + dlerror());
        return false;
    }
    create_ = reinterpret_cast<TdCreate>(dlsym(handle_, "td_create_client_id"));
    send_ = reinterpret_cast<TdSend>(dlsym(handle_, "td_send"));
    receive_ = reinterpret_cast<TdReceive>(dlsym(handle_, "td_receive"));
    execute_ = reinterpret_cast<TdExecute>(dlsym(handle_, "td_execute"));
    if (!create_ || !send_ || !receive_) {
        ADVAIT_LOG_ERROR("TDLib", "missing tdjson symbols");
        return false;
    }
    ADVAIT_LOG_INFO("TDLib", "loaded libtdjson");
    return true;
}

void TdClient::send_query(const nlohmann::json& q) {
    if (!send_) return;
    auto s = q.dump();
    send_(client_id_, s.c_str());
}

nlohmann::json TdClient::execute(const nlohmann::json& q) {
    if (!execute_) return {};
    auto s = q.dump();
    auto* r = execute_(s.c_str());
    if (!r) return {};
    try {
        return nlohmann::json::parse(r);
    } catch (...) {
        return {};
    }
}

bool TdClient::start_bot(int32_t api_id, const std::string& api_hash, const std::string& token, std::string* err) {
    if (!load_tdjson()) {
        if (err) *err = "failed to load libtdjson";
        return false;
    }
    api_id_ = api_id;
    api_hash_ = api_hash;
    token_ = token;
    execute({{"@type", "setLogVerbosityLevel"}, {"new_verbosity_level", 1}});
    client_id_ = create_();
    send_query({{"@type", "getOption"}, {"key", "version"}});
    running_ = true;
    loop_ = std::thread([this] {
        while (running_) {
            if (!receive_) break;
            const char* r = receive_(1.0);
            if (!r) continue;
            try {
                auto j = nlohmann::json::parse(r);
                process(j);
            } catch (const std::exception& e) {
                ADVAIT_LOG_ERROR("TDLib", std::string("parse: ") + e.what());
            }
        }
    });
    int waits = 0;
    while (!authorized_ && waits < 120) {
        usleep(250000);
        ++waits;
    }
    if (!authorized_) {
        if (err) *err = "authorization timeout";
        return false;
    }
    return true;
}

void TdClient::idle() {
    while (running_) usleep(200000);
}

void TdClient::close() {
    running_ = false;
    if (loop_.joinable()) loop_.join();
    if (handle_) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

void TdClient::on_command(const std::string& cmd, CommandHandler h) {
    std::lock_guard<std::mutex> g(mu_);
    commands_[utils::to_lower(cmd)] = std::move(h);
}

void TdClient::on_callback(const std::string& prefix, CallbackHandler h) {
    std::lock_guard<std::mutex> g(mu_);
    callbacks_.emplace_back(prefix, std::move(h));
}

bool TdClient::send_text(int64_t chat_id, const std::string& text, const nlohmann::json& markup) {
    nlohmann::json content = {
        {"@type", "inputMessageText"},
        {"text", {{"@type", "formattedText"}, {"text", text}, {"entities", nlohmann::json::array()}}},
        {"parse_mode", {{"@type", "textParseModeHTML"}}},
    };
    if (!text.empty()) {
        auto parsed = execute({{"@type", "parseTextEntities"},
                               {"text", text},
                               {"parse_mode", {{"@type", "textParseModeHTML"}}}});
        if (parsed.contains("@type") && parsed["@type"] == "formattedText") content["text"] = parsed;
    }
    nlohmann::json q = {
        {"@type", "sendMessage"},
        {"chat_id", chat_id},
        {"input_message_content", content},
    };
    if (!markup.empty()) q["reply_markup"] = markup;
    send_query(q);
    return true;
}

nlohmann::json TdClient::send_query_sync(const nlohmann::json& q, int timeout_ms) {
    static std::atomic<long long> seq{1000};
    nlohmann::json rq = q;
    auto id = std::to_string(++seq);
    rq["@extra"] = id;
    auto pr = std::make_unique<std::promise<nlohmann::json>>();
    auto fut = pr->get_future();
    {
        std::lock_guard<std::mutex> g(pend_mu_);
        pend_.emplace(id, std::move(*pr));
    }
    send_query(rq);
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        std::lock_guard<std::mutex> g(pend_mu_);
        pend_.erase(id);
        return {{"@type", "error"}, {"@message", "timeout"}};
    }
    try {
        return fut.get();
    } catch (...) {
        return {{"@type", "error"}, {"@message", "promise failed"}};
    }
}

int64_t TdClient::send_text_sync(int64_t chat_id, const std::string& text, const nlohmann::json& markup,
                                 int64_t reply_to, int timeout_ms) {
    nlohmann::json content = {
        {"@type", "inputMessageText"},
        {"text", {{"@type", "formattedText"}, {"text", text}, {"entities", nlohmann::json::array()}}},
    };
    // Matching against updateMessageSendSucceeded requires the PARSED plain
    // text (TDLib strips HTML tags/entities there), not the raw source we send.
    std::string plain = text;
    if (!text.empty()) {
        auto parsed = execute({{"@type", "parseTextEntities"},
                               {"text", text},
                               {"parse_mode", {{"@type", "textParseModeHTML"}}}});
        if (parsed.contains("@type") && parsed["@type"] == "formattedText") {
            content["text"] = parsed;
            plain = parsed.value("text", text);
        }
    }
    nlohmann::json q = {
        {"@type", "sendMessage"},
        {"chat_id", chat_id},
        {"input_message_content", content},
    };
    if (reply_to != 0)
        q["reply_to"] = {{"@type", "inputMessageReplyToMessage"}, {"message_id", reply_to}};
    if (!markup.empty()) q["reply_markup"] = markup;
    auto resp = send_query_sync(q, timeout_ms);
    auto t = resp.value("@type", "");
    bool succeeded_arrived_first = false;
    int64_t by_text_id = 0;
    {
        std::lock_guard<std::mutex> g(id_mu_);
        // updateMessageSendSucceeded may beat the sendMessage response; in
        // that case the FIFO matcher could not consume our (not yet added)
        // pending entry, so consult the per-text remap instead.
        auto it = text_remap_.find(std::to_string(chat_id) + "\x1f" + plain);
        if (it != text_remap_.end()) {
            succeeded_arrived_first = true;
            by_text_id = it->second;
            text_remap_.erase(it);
        }
    }
    int64_t mid = 0;
    if (t == "messageId") mid = resp.value("value", 0LL);
    else if (t == "message") mid = resp.value("id", 0LL);
    else {
        ADVAIT_LOG_ERROR("TDLib", std::string("send_text_sync failed: ") + resp.dump().substr(0, 200));
        return 0;
    }
    int64_t ret = succeeded_arrived_first ? by_text_id : mid;
    if (succeeded_arrived_first) {
        std::lock_guard<std::mutex> g(id_mu_);
        id_remap_[std::to_string(chat_id) + ":" + std::to_string(ret)] = ret;  // identity: wait_sent shortcut
        id_cv_.notify_all();
    }
    ADVAIT_LOG_INFO("TDLib", "send_text_sync chat=" + std::to_string(chat_id) +
                                " resp_id=" + std::to_string(mid) + " use=" + std::to_string(ret) +
                                " text='" + text.substr(0, 20) + "'");
    if (!succeeded_arrived_first) {
        std::lock_guard<std::mutex> g(id_mu_);
        pending_sends_.push_back({chat_id, {mid, plain}});
        if (pending_sends_.size() > 200) pending_sends_.pop_front();
    }
    return ret;
}

bool TdClient::wait_sent(int64_t chat_id, int64_t message_id, int timeout_ms) {
    if (message_id == 0) return false;
    auto key = std::to_string(chat_id) + ":" + std::to_string(message_id);
    std::unique_lock<std::mutex> lk(id_mu_);
    return id_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return id_remap_.count(key) > 0; });
}

std::string TdClient::tdlib_version() {    static std::string cached;
    if (cached.empty()) {
        auto v = execute({{"@type", "getVersion"}});
        cached = v.value("tdlib_version", "unknown");
    }
    return cached;
}

bool TdClient::reply_text(const Message& m, const std::string& text, const nlohmann::json& markup) {
    nlohmann::json content = {{"@type", "inputMessageText"},
                              {"text", {{"@type", "formattedText"}, {"text", text}, {"entities", nlohmann::json::array()}}}};
    auto parsed = execute({{"@type", "parseTextEntities"},
                           {"text", text},
                           {"parse_mode", {{"@type", "textParseModeHTML"}}}});
    if (parsed.contains("@type") && parsed["@type"] == "formattedText") content["text"] = parsed;
    nlohmann::json q = {
        {"@type", "sendMessage"},
        {"chat_id", m.chat_id},
        {"reply_to", {{"@type", "inputMessageReplyToMessage"}, {"message_id", m.id}}},
        {"input_message_content", content},
    };
    if (!markup.empty()) q["reply_markup"] = markup;
    send_query(q);
    return true;
}

bool TdClient::edit_text(int64_t chat_id, int64_t message_id, const std::string& text, const nlohmann::json& markup,
                         int64_t* final_id) {
    auto parsed = execute({{"@type", "parseTextEntities"},
                           {"text", text},
                           {"parse_mode", {{"@type", "textParseModeHTML"}}}});
    nlohmann::json content = {{"@type", "inputMessageText"}, {"text", parsed.is_object() ? parsed : nlohmann::json{{"@type", "formattedText"}, {"text", text}}}};
    auto attempt = [&](int64_t mid) -> nlohmann::json {
        nlohmann::json q = {
            {"@type", "editMessageText"},
            {"chat_id", chat_id},
            {"message_id", mid},
            {"input_message_content", content},
        };
        if (!markup.empty()) q["reply_markup"] = markup;
        return send_query_sync(q, 10000);
    };
    auto key = std::to_string(chat_id) + ":" + std::to_string(message_id);
    int64_t mid = message_id;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
        auto resp = attempt(mid);
        if (resp.value("@type", "") != "error") {
            if (final_id) {
                auto rid = resp.value("message", nlohmann::json::object()).value("id", 0LL);
                *final_id = rid != 0 ? rid : mid;
            }
            return true;
        }
        auto msg = resp.value("message", std::string());
        if (msg.find("MESSAGE_NOT_MODIFIED") != std::string::npos) {
            if (final_id) *final_id = mid;
            return true;
        }
        bool retriable = msg.find("not found") != std::string::npos ||
                         msg.find("can't be edited") != std::string::npos ||
                         msg.find("SENDING") != std::string::npos;
        if (!retriable || std::chrono::steady_clock::now() > deadline) {
            ADVAIT_LOG_ERROR("TDLib", std::string("edit_text failed: ") + resp.dump().substr(0, 200));
            return false;
        }
        // The send may still be finalizing (local id not yet remapped to the
        // server id). Wait for updateMessageIds and retry with the new id.
        int64_t mapped = 0;
        {
            std::unique_lock<std::mutex> lk(id_mu_);
            if (id_cv_.wait_until(lk, deadline, [&] {
                    auto it = id_remap_.find(key);
                    if (it == id_remap_.end()) return false;
                    mapped = it->second;
                    return true;
                }) &&
                mapped != 0 && mapped != mid) {
                mid = mapped;
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

bool TdClient::send_photo(int64_t chat_id, const std::string& photo, const std::string& caption, const nlohmann::json& markup) {
    auto parsed = execute({{"@type", "parseTextEntities"},
                           {"text", caption},
                           {"parse_mode", {{"@type", "textParseModeHTML"}}}});
    nlohmann::json q = {
        {"@type", "sendMessage"},
        {"chat_id", chat_id},
        {"input_message_content",
         {{"@type", "inputMessagePhoto"},
          {"photo", {{"@type", "inputFileRemote"}, {"id", photo}}},
          {"caption", parsed.is_object() ? parsed : nlohmann::json{{"@type", "formattedText"}, {"text", caption}}}}},
    };
    if (!markup.empty()) q["reply_markup"] = markup;
    send_query(q);
    return true;
}

bool TdClient::answer_callback(int64_t query_id, const std::string& text, bool alert) {
    send_query({{"@type", "answerCallbackQuery"},
                {"callback_query_id", query_id},
                {"text", text},
                {"show_alert", alert},
                {"cache_time", 0}});
    return true;
}

bool TdClient::delete_message(int64_t chat_id, int64_t message_id) {
    send_query({{"@type", "deleteMessages"}, {"chat_id", chat_id}, {"message_ids", nlohmann::json::array({message_id})}, {"revoke", true}});
    return true;
}

void TdClient::handle_authorization(const nlohmann::json& state) {
    auto t = state.value("@type", "");
    if (t == "authorizationStateWaitTdlibParameters") {
        send_query({
            {"@type", "setTdlibParameters"},
            {"database_directory", "database"},
            {"use_message_database", true},
            {"use_secret_chats", false},
            {"api_id", api_id_},
            {"api_hash", api_hash_},
            {"system_language_code", "en"},
            {"device_model", "AdvaitMusic-cpp"},
            {"application_version", "1.0.0"},
        });
    } else if (t == "authorizationStateWaitPhoneNumber") {
        send_query({{"@type", "checkAuthenticationBotToken"}, {"token", token_}});
    } else if (t == "authorizationStateReady") {
        authorized_ = true;
        send_query({{"@type", "getMe"}});
        ADVAIT_LOG_INFO("TDLib", "authorizationStateReady");
    } else if (t == "authorizationStateClosed") {
        running_ = false;
    }
}

void TdClient::handle_message(const nlohmann::json& msg) {
    Message m;
    m.chat_id = msg.value("chat_id", 0LL);
    m.id = msg.value("id", 0LL);
    m.raw = msg;
    if (msg.contains("sender_id")) {
        auto s = msg["sender_id"];
        if (s.value("@type", "") == "messageSenderUser") m.sender_id = s.value("user_id", 0LL);
    }
    m.is_private = m.chat_id > 0;
    if (msg.contains("reply_to") && msg["reply_to"].is_object())
        m.reply_to = msg["reply_to"].value("message_id", 0LL);
    auto content = msg.value("content", nlohmann::json::object());
    if (content.value("@type", "") == "messageText") {
        m.text = content.value("text", nlohmann::json::object()).value("text", "");
    } else if (content.contains("caption")) {
        m.text = content["caption"].value("text", "");
    }
    if (m.text.empty() || m.text[0] != '/') return;
    auto space = m.text.find(' ');
    auto cmd = m.text.substr(1, (space == std::string::npos ? m.text.size() : space) - 1);
    auto at = cmd.find('@');
    if (at != std::string::npos) cmd = cmd.substr(0, at);
    cmd = utils::to_lower(cmd);
    CommandHandler h;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = commands_.find(cmd);
        if (it == commands_.end()) return;
        h = it->second;
    }
    auto msg_copy = m;
    std::thread([h, msg_copy]() {
        try {
            h(msg_copy);
        } catch (const std::exception& e) {
            ADVAIT_LOG_ERROR("dispatcher", std::string("handler: ") + e.what());
        }
    }).detach();
}

void TdClient::handle_callback(const nlohmann::json& cb) {
    CallbackQuery q;
    {
        auto idv = cb.value("id", std::string("0"));
        try {
            q.query_id = std::stoll(idv);
        } catch (...) {
            q.query_id = 0;
        }
    }
    q.chat_id = cb.value("chat_id", 0LL);
    q.message_id = cb.value("message_id", 0LL);
    if (cb.contains("sender_user_id")) q.user_id = cb.value("sender_user_id", 0LL);
    auto payload = cb.value("payload", nlohmann::json::object());
    if (payload.contains("data")) {
        auto d = payload["data"];
        if (d.is_string())
            q.data = utils::base64_decode(d.get<std::string>());
        else if (d.is_array()) {
            std::string s;
            for (auto& b : d) s.push_back(static_cast<char>(b.get<int>()));
            q.data = s;
        }
    }
    q.raw = cb;
    CallbackHandler h;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& [pfx, fn] : callbacks_) {
            if (q.data == pfx || q.data.rfind(pfx, 0) == 0) {
                h = fn;
                break;
            }
        }
    }
    if (!h) return;
    std::thread([h, q]() {
        try {
            h(q);
        } catch (const std::exception& e) {
            ADVAIT_LOG_ERROR("dispatcher", std::string("callback: ") + e.what());
        }
    }).detach();
}

void TdClient::process(const nlohmann::json& update) {
    if (update.contains("@extra") && update["@extra"].is_string()) {
        auto id = update["@extra"].get<std::string>();
        std::lock_guard<std::mutex> g(pend_mu_);
        auto it = pend_.find(id);
        if (it != pend_.end()) {
            it->second.set_value(update);
            pend_.erase(it);
            return;
        }
    }
    auto t = update.value("@type", "");
    if (t == "updateMessageSendSucceeded") {
        const auto& msg = update.value("message", nlohmann::json::object());
        auto chat_id = msg.value("chat_id", 0LL);
        auto new_id = msg.value("id", 0LL);
        std::string text;
        if (msg.contains("content") && msg["content"].value("@type", "") == "messageText" &&
            msg["content"].contains("text") && msg["content"]["text"].contains("text") &&
            msg["content"]["text"]["text"].is_string())
            text = msg["content"]["text"]["text"].get<std::string>();
        std::lock_guard<std::mutex> g(id_mu_);
        bool matched = false;
        if (update.contains("old_message_ids") && update["old_message_ids"].is_array()) {
            for (auto& old : update["old_message_ids"]) {
                auto key = std::to_string(chat_id) + ":" + std::to_string(old.get<int64_t>());
                id_remap_[key] = new_id;
            }
        }
        if (!text.empty()) {
            for (auto it = pending_sends_.begin(); it != pending_sends_.end(); ++it) {
                if (it->first == chat_id && it->second.second == text) {
                    auto key = std::to_string(chat_id) + ":" + std::to_string(it->second.first);
                    id_remap_[key] = new_id;
                    pending_sends_.erase(it);
                    matched = true;
                    break;
                }
            }
        }
        if (!text.empty() && !matched) {
            // send_text_sync may still be between "request sent" and "response
            // handled"; remember by text so it can resolve its own message id.
            text_remap_[std::to_string(chat_id) + "\x1f" + text] = new_id;
            if (text_remap_.size() > 400) text_remap_.clear();
        }
        ADVAIT_LOG_INFO("TDLib", "sendSucceeded new_id=" + std::to_string(new_id) +
                                     " text='" + text.substr(0, 24) + "' matched=" + (matched ? "y" : "n") +
                                     " pending=" + std::to_string(pending_sends_.size()) +
                                     " old_ids=" + update.value("old_message_ids", nlohmann::json::array()).dump());
        if (id_remap_.size() > 800) id_remap_.clear();
        id_cv_.notify_all();
        return;
    }
    if (t == "updateMessageIds") {
        auto chat_id = update.value("chat_id", 0LL);
        ADVAIT_LOG_INFO("TDLib", "updateMessageIds chat=" + std::to_string(chat_id) + " " + update.dump().substr(0, 150));
        std::lock_guard<std::mutex> g(id_mu_);
        if (update.contains("message_ids") && update["message_ids"].is_array()) {
            for (auto& p : update["message_ids"]) {
                auto key = std::to_string(chat_id) + ":" + std::to_string(p.value("old_message_id", 0LL));
                id_remap_[key] = p.value("new_message_id", 0LL);
            }
            if (id_remap_.size() > 500) id_remap_.clear();
            id_cv_.notify_all();
        }
        return;
    }
    if (t == "updateAuthorizationState") {
        handle_authorization(update.value("authorization_state", nlohmann::json::object()));
        return;
    }
    if (t == "user" && update.value("id", 0LL) != 0) {
        me_id_ = update.value("id", 0LL);
        me_first_name_ = update.value("first_name", "");
        if (update.contains("usernames") && update["usernames"].contains("active_usernames") &&
            update["usernames"]["active_usernames"].is_array() &&
            !update["usernames"]["active_usernames"].empty()) {
            me_username_ = update["usernames"]["active_usernames"][0].get<std::string>();
            set_bot_username(me_username_);
            ADVAIT_LOG_INFO("TDLib", "Bot started as @" + me_username_);
        }
        return;
    }
    if (t == "updateNewMessage") {
        handle_message(update.value("message", nlohmann::json::object()));
        return;
    }
    if (t == "updateNewCallbackQuery") {
        handle_callback(update);
        return;
    }
}

}  // namespace advait::tg
