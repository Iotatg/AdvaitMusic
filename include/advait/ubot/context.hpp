#pragma once

#include "advait/mtproto/client.hpp"
#include "advait/ntgcalls/client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace advait::ubot {

// Assistant-side voice chat controller: keeps the WebRTC binding and the
// MTProto transport, replicating KanhaMusic's ubot join/connect sequence.
class Context {
public:
    explicit Context(std::shared_ptr<mtproto::Client> mt);
    ~Context();

    // Join (if needed) and start streaming desc into the chat's group call.
    bool play(int64_t chat_id, const ntg::MediaDescription& desc, std::string* err = nullptr);
    bool connected(int64_t chat_id) const;
    bool pause(int64_t chat_id);
    bool resume(int64_t chat_id);
    bool mute(int64_t chat_id);
    bool unmute(int64_t chat_id);
    bool stop(int64_t chat_id);
    void on_stream_end(ntg::StreamEndCallback cb);
    void on_call_discarded(std::function<void(int64_t)> cb);
    void close();

    ntg::Client* binding() { return binding_.get(); }
    std::shared_ptr<mtproto::Client> mt() const { return mt_; }

private:
    struct Mounted {
        bool joined{false};
        bool sending_video{false};
        mtproto::Client::GroupCallRef call;
    };

    bool join_locked(int64_t chat_id, const ntg::MediaDescription& desc, std::string* err);

    std::unique_ptr<ntg::Client> binding_;
    std::shared_ptr<mtproto::Client> mt_;
    mutable std::mutex mu_;
    std::unordered_map<int64_t, Mounted> calls_;
    std::vector<std::function<void(int64_t)>> discarded_;
};

}  // namespace advait::ubot
