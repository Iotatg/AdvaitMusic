#pragma once

#include "advait/core/models.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace advait {

struct Assistant;

class RoomState {
public:
    int64_t id{0};
    int64_t chat_id{0};
    Assistant* assistant{nullptr};

    bool play(const Track& t, const std::string& path, bool force = false);
    bool pause();
    bool resume();
    bool replay();
    bool stop();
    bool seek(int seconds);
    bool set_speed(double speed);
    bool mute();
    bool unmute();

    bool active() const;
    bool destroyed() const { return destroyed_.load(); }
    bool paused() const;
    bool muted() const;
    bool autoplay() const;
    void set_autoplay(bool v);
    int position() const;
    double speed() const;
    int loop() const;
    void set_loop(int v);
    std::optional<Track> track() const;
    std::optional<Track> next_track();
    std::vector<Track> queue() const;
    void clear_queue();
    bool remove_at(size_t index);
    bool move(size_t from, size_t to);
    void shuffle();
    std::string file_path() const;
    std::vector<std::string> played_history() const;
    std::string engine_token() const { return engine_token_; }

private:
    mutable std::mutex mu_;
    std::string file_path_;
    std::optional<Track> track_;
    bool playing_{false};
    bool paused_{false};
    bool muted_{false};
    double speed_{1.0};
    int position_{0};
    int64_t updated_at_{0};
    int loop_{0};
    std::vector<Track> queue_;
    bool shuffle_{false};
    bool autoplay_{false};
    std::vector<std::string> played_history_;
    std::string engine_token_;
    std::atomic<bool> destroyed_{false};

    bool play_native();
    void update_position();
    void add_played_locked(const Track& t);
};

RoomState* create_room(int64_t chat_id, Assistant* ass);
RoomState* room_for(int64_t chat_id);
bool drop_room(int64_t chat_id);
std::unordered_map<int64_t, RoomState*> all_rooms();

}  // namespace advait
