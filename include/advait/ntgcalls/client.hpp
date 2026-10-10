#pragma once

#include <cstdint>
#include <functional>
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
    void close();
    uintptr_t ptr() const { return ptr_; }

private:
    uintptr_t ptr_{0};
    std::mutex mu_;
    std::vector<StreamEndCallback> stream_end_;
};

MediaDescription media_from_file(const std::string& path, int pos, double speed, bool video);

const std::string& version();

}  // namespace advait::ntg
