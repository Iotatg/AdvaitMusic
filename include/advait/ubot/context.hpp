#pragma once

#include "advait/ntgcalls/client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace advait::ubot {

class Context {
public:
    Context();
    ~Context();

    bool play(int64_t chat_id, const ntg::MediaDescription& desc);
    bool pause(int64_t chat_id);
    bool resume(int64_t chat_id);
    bool mute(int64_t chat_id);
    bool unmute(int64_t chat_id);
    bool stop(int64_t chat_id);
    void on_stream_end(ntg::StreamEndCallback cb);
    void on_call_discarded(std::function<void(int64_t)> cb);
    void close();

    ntg::Client* binding() { return binding_.get(); }

private:
    std::unique_ptr<ntg::Client> binding_;
    std::mutex mu_;
    std::vector<std::function<void(int64_t)>> discarded_;
};

}  // namespace advait::ubot
