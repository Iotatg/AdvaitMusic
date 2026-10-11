#pragma once

#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <set>
#include <unordered_map>
#include <vector>

namespace advait::tg {

struct Message {
    int64_t chat_id{0};
    int64_t id{0};
    int64_t sender_id{0};
    std::string text;
    std::string first_name;
    std::string last_name;
    bool is_private{false};
    bool is_channel{false};
    int64_t reply_to{0};
    nlohmann::json raw;
};

struct CallbackQuery {
    int64_t chat_id{0};
    int64_t message_id{0};
    int64_t user_id{0};
    int64_t query_id{0};
    std::string data;
    nlohmann::json raw;
};

using CommandHandler = std::function<void(const Message&)>;
using CallbackHandler = std::function<void(const CallbackQuery&)>;

class TdClient {
public:
    TdClient();
    ~TdClient();

    bool start_bot(int32_t api_id, const std::string& api_hash, const std::string& token, std::string* err);
    void idle();
    void close();

    void on_command(const std::string& cmd, CommandHandler h);
    void on_callback(const std::string& prefix, CallbackHandler h);

    bool send_text(int64_t chat_id, const std::string& text, const nlohmann::json& markup = {});
    int64_t send_text_sync(int64_t chat_id, const std::string& text, const nlohmann::json& markup = {},
                           int64_t reply_to = 0, int timeout_ms = 15000);
    std::string tdlib_version();
    bool wait_sent(int64_t chat_id, int64_t message_id, int timeout_ms = 5000);
    bool reply_text(const Message& m, const std::string& text, const nlohmann::json& markup = {});
    bool edit_text(int64_t chat_id, int64_t message_id, const std::string& text, const nlohmann::json& markup = {},
                   int64_t* final_id = nullptr, bool quiet = false);
    bool send_photo(int64_t chat_id, const std::string& photo, const std::string& caption, const nlohmann::json& markup = {});
    int64_t send_photo_sync(int64_t chat_id, const std::string& photo, bool local_path, const std::string& caption,
                            const nlohmann::json& markup = {}, int64_t reply_to = 0, int timeout_ms = 20000);
    bool edit_caption(int64_t chat_id, int64_t message_id, const std::string& caption_html,
                      const nlohmann::json& markup = {}, bool quiet = false);
    bool edit_markup(int64_t chat_id, int64_t message_id, const nlohmann::json& markup, bool quiet = false);
    bool answer_callback(int64_t query_id, const std::string& text = {}, bool alert = false);
    bool delete_message(int64_t chat_id, int64_t message_id);

    int64_t me_id() const { return me_id_; }
    std::string me_username() const { return me_username_; }
    std::string me_first_name() const { return me_first_name_; }
    bool ready() const { return authorized_; }

private:
    using TdCreate = int (*)();
    using TdSend = void (*)(int, const char*);
    using TdReceive = const char* (*)(double);
    using TdExecute = const char* (*)(const char*);

    void* handle_{nullptr};
    TdCreate create_{nullptr};
    TdSend send_{nullptr};
    TdReceive receive_{nullptr};
    TdExecute execute_{nullptr};
    int client_id_{0};
    std::thread loop_;
    std::atomic<bool> running_{false};
    std::atomic<bool> authorized_{false};
    int64_t extra_{1};
    int64_t me_id_{0};
    std::string me_username_;
    std::string me_first_name_;
    int32_t api_id_{0};
    std::string api_hash_;
    std::string token_;

    std::mutex mu_;
    std::unordered_map<std::string, CommandHandler> commands_;
    std::vector<std::pair<std::string, CallbackHandler>> callbacks_;

    bool load_tdjson();
    void send_query(const nlohmann::json& q);
    nlohmann::json send_query_sync(const nlohmann::json& q, int timeout_ms);
    nlohmann::json execute(const nlohmann::json& q);
    nlohmann::json bot_api(const std::string& method,
                           const std::vector<std::pair<std::string, std::string>>& fields,
                           const std::string& file_field = {}, const std::string& file_path = {},
                           int timeout_ms = 20000);
    void process(const nlohmann::json& update);
    void handle_authorization(const nlohmann::json& state);
    void handle_message(const nlohmann::json& msg);
    void handle_callback(const nlohmann::json& cb);

    std::mutex pend_mu_;
    std::unordered_map<std::string, std::promise<nlohmann::json>> pend_;

    std::mutex id_mu_;
    std::condition_variable id_cv_;
    std::unordered_map<std::string, int64_t> id_remap_;
    std::unordered_map<std::string, int64_t> text_remap_;
    std::deque<std::pair<int64_t, std::pair<int64_t, std::string>>> pending_sends_;
};

TdClient& bot();

}  // namespace advait::tg
