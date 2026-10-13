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
// Fired for EVERY new message (commands and plain texts alike) — used by the
// sticker reply engine and group notes/filters. Handlers run inline in the
// receive loop; dispatch to a thread if work is slow.
using RawMessageHandler = std::function<void(const Message&)>;
// Fired for updateChatMember updates (raw TDLib update json).
using ChatMemberHandler = std::function<void(const nlohmann::json& update)>;

class TdClient {
public:
    TdClient();
    ~TdClient();

    bool start_bot(int32_t api_id, const std::string& api_hash, const std::string& token, std::string* err);
    void idle();
    void close();

    void on_command(const std::string& cmd, CommandHandler h);
    void on_callback(const std::string& prefix, CallbackHandler h);
    void on_message(RawMessageHandler h);
    void on_chat_member(ChatMemberHandler h);

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

    // ---- chat membership helpers (assistant auto-join, KanhaMusic parity) ----
    struct ChatMemberInfo {
        bool ok{false};       // query succeeded (bot is admin enough to read members)
        bool present{false};  // assistant is a chat member
        bool banned{false};   // kicked / restricted without membership
        std::string error;    // TDLib error message when !ok
    };
    ChatMemberInfo get_chat_member_state(int64_t chat_id, int64_t user_id);
    std::string chat_public_username(int64_t chat_id);
    bool create_chat_invite_link(int64_t chat_id, std::string* link, std::string* err);
    bool approve_join_request(int64_t chat_id, int64_t user_id, std::string* err);

    // ---- group-admin / moderation / sticker plumbing ----
    bool send_chat_action(int64_t chat_id, const std::string& action_type);
    // Sends a sticker by remote file id; returns the new message id (0 on failure).
    int64_t send_sticker_sync(int64_t chat_id, const std::string& remote_file_id, int64_t reply_to = 0,
                              int timeout_ms = 20000);
    // Raw getStickerSet response ({} on failure): stickers[].{id, emoji}.
    // TDLib >= 1.8.66: getStickerSet accepts only a numeric set_id.
    nlohmann::json get_sticker_set(int64_t set_id);
    // Raw getRepliedMessage response ({} on failure).
    nlohmann::json get_replied_message(int64_t chat_id, int64_t message_id);
    // Raw getUser response ({} on failure).
    nlohmann::json get_user(int64_t user_id);
    // Raw searchPublicChat response ({} on failure).
    nlohmann::json search_public_chat(const std::string& username);
    // Generic sync TDLib call for group-admin moderation (setChatMemberStatus,
    // pin/unpin, deleteMessages, getChatHistory...). {} on failure.
    nlohmann::json call_sync(const nlohmann::json& q, int timeout_ms = 15000);
    // Raw sync query: surfaces the full response (including "@type":"error")
    // so callers can read code/message for diagnostics.
    nlohmann::json send_query_sync_public(const nlohmann::json& q, int timeout_ms = 15000);

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
    std::vector<RawMessageHandler> message_hooks_;
    ChatMemberHandler chat_member_hook_;

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
