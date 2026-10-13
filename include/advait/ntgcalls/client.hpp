#pragma once

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace advait::ntg {

struct AudioDescription {
    int media_source{0};
    std::string input;
    uint32_t sample_rate{96000};
    uint8_t channel_count{2};
};

struct VideoDescription {
    int media_source{0};
    std::string input;
    int16_t width{1280};
    int16_t height{720};
    uint8_t fps{30};
};

struct MediaDescription {
    AudioDescription* microphone{nullptr};
    VideoDescription* camera{nullptr};
};

using StreamEndCallback = std::function<void(int64_t chat_id)>;

// connected is true only when the engine reported NTG_STATE_CONNECTED.
// state is one of connecting/connected/timeout/failed/closed.
using ConnectionChangeCallback = std::function<void(int64_t chat_id, bool connected, const std::string& state)>;

struct ConnectionInfo {
    bool settled{false};
    bool connected{false};
    std::string state;
};

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool create(int64_t chat_id, std::string* sdp);
    bool connect(int64_t chat_id, const std::string& params);
    bool set_stream_sources(int64_t chat_id, const MediaDescription& desc);
    bool pause(int64_t chat_id);
    bool resume(int64_t chat_id);
    bool mute(int64_t chat_id);
    bool unmute(int64_t chat_id);
    bool stop(int64_t chat_id);
    int64_t time(int64_t chat_id);
    void on_stream_end(StreamEndCallback cb);
    void on_connection_change(ConnectionChangeCallback cb);
    // Waits until the engine reports a terminal connection state for chat_id.
    // Returns false only when a failure/timeout was actually reported; if the
    // engine never emits any event the wait just times out and returns true.
    bool wait_connected(int64_t chat_id, int timeout_ms = 20000);
    std::string connection_state(int64_t chat_id);
    void close();
    uintptr_t ptr() const { return ptr_; }

private:
    uintptr_t ptr_{0};
    std::mutex mu_;
    std::vector<StreamEndCallback> stream_end_;
    std::vector<ConnectionChangeCallback> connection_change_;
    std::map<int64_t, ConnectionInfo> connection_;
    std::condition_variable connection_cv_;
};

MediaDescription media_from_file(const std::string& path, int pos, double speed, bool video);

const std::string& version();

}  // namespace advait::ntg
