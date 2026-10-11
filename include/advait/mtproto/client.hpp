#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "advait/mtproto/tl.hpp"

namespace advait::mtproto {

inline int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
inline int64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Authorized session obtained from a userbot string session (pyrogram or gogram format).
struct Session {
    int dc_id{0};
    std::string ip;  // literal address, no port
    int port{443};
    std::string auth_key;  // 256 raw bytes
    int32_t api_id{0};
    int64_t user_id{0};
};

// Server-side updates that the assistant engine cares about (decoded from RPC + push updates).
struct UpdateEvent {
    std::string kind;  // "signaling", "connection_state", "participants", ...
    int64_t group_call_id{0};
    int64_t chat_id{0};
    std::string payload;  // kind-specific (base64 / json / raw bytes)
};

struct ClientConfig {
    std::string app_version{"AdvaitMusic"};
    std::string device_model{"Linux x86_64"};
    std::string system_version{"Linux"};
    std::string lang_code{"en"};
};

// Minimal MTProto user client: enough to authenticate with an imported auth key,
// stay initialized, and run group-call phone.* methods.
class Client {
public:
    Client(Session session, ClientConfig cfg);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // TCP connect + initConnection handshake. Idempotent.
    bool connect(std::string* err);
    void disconnect();
    bool connected() const { return fd_ >= 0 && initialized_; }

    // Serialized TL request -> serialized TL response object (rpc_result body).
    std::string call_rpc(const std::string& serialized_request, std::string* err, bool wrap_init = false,
                         bool match_pong = false);

    // Non-blocking: drains socket, dispatches pushed updates to the handler.
    void poll_updates();
    void set_update_handler(std::function<void(const UpdateEvent&)> h);

    // ---- group voice calls ----
    struct GroupCallRef {
        int64_t id{0};
        int64_t access_hash{0};
    };
    // Resolve the currently active group call of a superchannel (via channels.getFullChannel).
    bool get_group_call(int64_t channel_id, GroupCallRef* out, std::string* err);
    // Access hash of our own account (users.getUsers(inputUserSelf)), cached after first call.
    bool self_access_hash(int64_t* out, std::string* err);
    // Join an active group call; on success *server_params receives updateGroupCallConnection params JSON.
    bool join_group_call(const GroupCallRef&, const std::string& params_json, std::string* server_params,
                         std::string* err);

    const Session& session() const { return session_; }

    static Session parse_session_string(const std::string& encoded);
    static std::string dc_ip(int dc_id);

private:
    Session session_;
    ClientConfig cfg_;
    int fd_{-1};
    bool initialized_{false};
    int64_t session_id_{0};
    int64_t last_msg_id_{0};
    int64_t seq_no_{0};
    int64_t server_salt_{0};
    bool salt_retry_{false};
    bool expect_pong_{false};
    bool rpc_ready_{false};
    bool init_active_{false};
    int64_t self_ah_{0};
    std::mutex send_mu_;
    std::mutex updates_mu_;
    std::function<void(const UpdateEvent&)> update_handler_;
    std::string recv_buf_;
    bool obfuscated_{false};

    bool socket_connect(std::string* err);
    bool send_all(const std::string& data);
    bool recv_chunk(int timeout_ms);
    bool write_msg(const std::string& msg);
    bool read_msg(std::string* msg);
    std::string encrypt_body(const std::string& body, int64_t* msg_id, bool content_related);
    // Decrypts one encrypted_message; returns plaintext inner body (after salt/session/msg_id/seq/len header).
    bool decrypt_message(const std::string& blob, std::string* body, int64_t* msg_id_out);
    // Process one decrypted body; if it is rpc_result matching req_id, copies response into out.
    bool handle_plaintext(const std::string& body, int64_t req_id, std::string* out_result, bool* matched);
    bool init_connection(std::string* err);
    bool resolve_channel(int64_t chat_id, int64_t* channel_out, int64_t* ah, std::string* err);
    void dispatch_update(const std::string& obj);
    bool wait_socket_readable(int timeout_ms);
};

}  // namespace advait::mtproto
