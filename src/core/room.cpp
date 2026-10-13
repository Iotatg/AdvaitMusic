#include "advait/core/room.hpp"
#include "advait/core/assistant.hpp"
#include "advait/core/config.hpp"
#include "advait/core/logger.hpp"
#include "advait/database/database.hpp"
#include "advait/ntgcalls/client.hpp"
#include "advait/ubot/context.hpp"

#include <algorithm>
#include <chrono>
#include <random>

namespace advait {

namespace {
std::mutex rooms_mu;
std::unordered_map<int64_t, std::unique_ptr<RoomState>> rooms;
constexpr double kMinSpeed = 0.50;
constexpr double kMaxSpeed = 4.0;
constexpr int kSeekEndThreshold = 10;
constexpr int kSeekSafetyMargin = 5;
}  // namespace

bool RoomState::active() const {
    std::lock_guard<std::mutex> g(mu_);
    return playing_ && track_.has_value();
}

bool RoomState::paused() const {
    std::lock_guard<std::mutex> g(mu_);
    return paused_;
}

bool RoomState::muted() const {
    std::lock_guard<std::mutex> g(mu_);
    return muted_;
}

bool RoomState::autoplay() const {
    std::lock_guard<std::mutex> g(mu_);
    return autoplay_;
}

void RoomState::set_autoplay(bool v) {
    std::lock_guard<std::mutex> g(mu_);
    autoplay_ = v;
}

int RoomState::position() const {
    std::lock_guard<std::mutex> g(mu_);
    return position_;
}

double RoomState::speed() const {
    std::lock_guard<std::mutex> g(mu_);
    return speed_;
}

int RoomState::loop() const {
    std::lock_guard<std::mutex> g(mu_);
    return loop_;
}

void RoomState::set_loop(int v) {
    std::lock_guard<std::mutex> g(mu_);
    loop_ = v;
}

std::optional<Track> RoomState::track() const {
    std::lock_guard<std::mutex> g(mu_);
    return track_;
}

std::optional<Track> RoomState::next_track() {
    if (destroyed()) return std::nullopt;
    std::lock_guard<std::mutex> g(mu_);
    if (track_ && loop_ > 0) {
        position_ = 0;
        playing_ = false;
        paused_ = false;
        muted_ = false;
        loop_--;
        updated_at_ = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        return track_;
    }
    if (queue_.empty()) return std::nullopt;
    size_t index = 0;
    if (shuffle_ && queue_.size() > 1) {
        index = static_cast<size_t>(std::mt19937{std::random_device{}()}() % queue_.size());
    }
    Track next = queue_[index];
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(index));
    track_ = next;
    add_played_locked(next);
    position_ = 0;
    playing_ = false;
    paused_ = false;
    muted_ = false;
    updated_at_ = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return next;
}

std::vector<Track> RoomState::queue() const {
    std::lock_guard<std::mutex> g(mu_);
    return queue_;
}

void RoomState::clear_queue() {
    std::lock_guard<std::mutex> g(mu_);
    queue_.clear();
}

bool RoomState::remove_at(size_t index) {
    std::lock_guard<std::mutex> g(mu_);
    if (index >= queue_.size()) return false;
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool RoomState::move(size_t from, size_t to) {
    std::lock_guard<std::mutex> g(mu_);
    if (from >= queue_.size() || to >= queue_.size()) return false;
    auto t = queue_[from];
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(from));
    queue_.insert(queue_.begin() + static_cast<std::ptrdiff_t>(to), t);
    return true;
}

void RoomState::shuffle() {
    std::lock_guard<std::mutex> g(mu_);
    std::shuffle(queue_.begin(), queue_.end(), std::mt19937{std::random_device{}()});
    shuffle_ = true;
}

std::string RoomState::file_path() const {
    std::lock_guard<std::mutex> g(mu_);
    return file_path_;
}

std::vector<std::string> RoomState::played_history() const {
    std::lock_guard<std::mutex> g(mu_);
    return played_history_;
}

void RoomState::update_position() {
    if (!playing_ || paused_ || updated_at_ == 0) return;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    int delta = static_cast<int>((now - updated_at_) * speed_);
    if (delta > 0) position_ += delta;
    updated_at_ = now;
}

void RoomState::add_played_locked(const Track& t) {
    played_history_.push_back("id:" + t.id);
    auto n = normalize_track_title(t.title);
    if (!n.empty()) played_history_.push_back("title:" + n);
    if (played_history_.size() > 100) {
        played_history_.erase(played_history_.begin(),
                              played_history_.begin() + static_cast<std::ptrdiff_t>(played_history_.size() - 100));
    }
}

 bool RoomState::play_native(std::string* err) {
     if (!assistant || !assistant->ntg) {
         if (err) *err = "no assistant configured for this chat (check STRING_SESSIONS)";
         return false;
     }
     auto desc = ntg::media_from_file(file_path_, position_, speed_, track_ && track_->video);
     std::string native_err;
     if (!assistant->ntg->play(id, desc, &native_err)) {
         if (err) *err = native_err.empty() ? "voice engine refused the stream" : native_err;
         ADVAIT_LOG_WARN("room", "play_native failed for " + std::to_string(id) + ": " + *err);
         return false;
     }
     return true;
 }

 bool RoomState::play(const Track& t, const std::string& path, bool force, std::string* err) {
     if (destroyed()) {
         if (err) *err = "room already destroyed";
         return false;
     }
     {
         std::lock_guard<std::mutex> g(mu_);
         if (!force && playing_ && track_) {
             queue_.push_back(t);
             return true;
         }
         if (!track_ || track_->id != t.id) loop_ = 0;
         track_ = t;
         add_played_locked(t);
         playing_ = true;
         file_path_ = path;
         position_ = 0;
         paused_ = false;
         muted_ = false;
         updated_at_ = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
     }
     if (path.empty()) {
         // No local file: keep the request queued so the monitor downloads and
         // starts it, instead of reporting success while nothing streams.
         std::lock_guard<std::mutex> g(mu_);
         queue_.push_back(t);
         return true;
     }
     if (!play_native(err)) {
         std::lock_guard<std::mutex> g(mu_);
         track_.reset();
         playing_ = false;
         file_path_.clear();
         return false;
     }
     return true;
 }

bool RoomState::pause() {
    if (destroyed()) return false;
    bool was_muted = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (paused_) return true;
        was_muted = muted_;
    }
    if (assistant && assistant->ntg) assistant->ntg->pause(id);
    // Go: pausing a muted room unmutes the call output so the later resume is audible.
    if (was_muted && assistant && assistant->ntg) {
        std::lock_guard<std::mutex> g(mu_);
        if (muted_ && assistant && assistant->ntg) assistant->ntg->unmute(id);
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        update_position();
        paused_ = true;
        muted_ = false;
    }
    return true;
}

bool RoomState::resume() {
    if (destroyed()) return false;
    if (!active()) return false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!paused_) return true;
    }
    if (assistant && assistant->ntg) assistant->ntg->resume(id);
    {
        std::lock_guard<std::mutex> g(mu_);
        paused_ = false;
        muted_ = false;
        playing_ = true;
        updated_at_ = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    }
    return true;
}

bool RoomState::replay() {
    if (destroyed()) return false;
    bool was_muted = false;
    int old_pos = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!track_ || file_path_.empty()) return false;
        was_muted = muted_;
        old_pos = position_;
        position_ = 0;
    }
    if (!play_native()) {
        std::lock_guard<std::mutex> g(mu_);
        position_ = old_pos;
        return false;
    }
    if (was_muted && assistant && assistant->ntg) assistant->ntg->unmute(id);
    std::lock_guard<std::mutex> g(mu_);
    position_ = 0;
    paused_ = false;
    muted_ = false;
    playing_ = true;
    return true;
}

bool RoomState::stop() {
    if (destroyed()) return false;
    if (assistant && assistant->ntg) assistant->ntg->stop(id);
    std::lock_guard<std::mutex> g(mu_);
    track_.reset();
    position_ = 0;
    playing_ = false;
    paused_ = false;
    muted_ = false;
    updated_at_ = 0;
    autoplay_ = false;
    return true;
}

bool RoomState::seek(int seconds) {
    if (destroyed()) return false;
    bool was_muted = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!track_ || file_path_.empty()) return false;
        update_position();
        if (seconds > 0 && track_->duration - position_ <= kSeekEndThreshold) return false;
        int new_pos = position_ + seconds;
        if (new_pos >= track_->duration) new_pos = track_->duration - kSeekSafetyMargin;
        if (new_pos < 0) new_pos = 0;
        was_muted = muted_;
        position_ = new_pos;
        paused_ = false;
        muted_ = false;
    }
    if (!play_native()) return false;
    // Go: seeks drop mute state; if the call output was muted, unmute it again.
    if (was_muted && assistant && assistant->ntg) assistant->ntg->unmute(id);
    return true;
}

bool RoomState::set_speed(double speed) {
    if (destroyed()) return false;
    if (speed < kMinSpeed || speed > kMaxSpeed) return false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!track_ || file_path_.empty()) return false;
        if (speed_ == speed) return true;
        update_position();
        speed_ = speed;
        playing_ = true;
        paused_ = false;
        muted_ = false;
    }
    return play_native();
}

bool RoomState::mute() {
    if (destroyed()) return false;
    bool was_paused = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (muted_) return true;
        was_paused = paused_;
    }
    // Go: muting a paused room resumes playback first (mute implies audible-stream state).
    if (was_paused) {
        if (assistant && assistant->ntg) assistant->ntg->resume(id);
        std::lock_guard<std::mutex> g(mu_);
        paused_ = false;
    }
    if (assistant && assistant->ntg) assistant->ntg->mute(id);
    std::lock_guard<std::mutex> g(mu_);
    muted_ = true;
    return true;
}

bool RoomState::unmute() {
    if (destroyed()) return false;
    if (assistant && assistant->ntg) assistant->ntg->unmute(id);
    std::lock_guard<std::mutex> g(mu_);
    update_position();
    muted_ = false;
    paused_ = false;
    return true;
}

RoomState* create_room(int64_t chat_id, Assistant* ass) {
    std::lock_guard<std::mutex> g(rooms_mu);
    auto it = rooms.find(chat_id);
    if (it != rooms.end()) {
        // A room created before an assistant was available (or bound to a
        // different one) keeps its null/old assistant and can never stream
        // again; refresh it when we now have one.
        if (ass && it->second->assistant != ass) {
            it->second->assistant = ass;
            ADVAIT_LOG_INFO("room", "reassigned assistant for chat " + std::to_string(chat_id));
        }
        return it->second.get();
    }
    auto r = std::make_unique<RoomState>();
    r->id = chat_id;
    r->chat_id = chat_id;
    r->assistant = ass;
    r->set_autoplay(db::autoplay(chat_id));
    auto* p = r.get();
    rooms[chat_id] = std::move(r);
    return p;
}

RoomState* room_for(int64_t chat_id) {
    std::lock_guard<std::mutex> g(rooms_mu);
    auto it = rooms.find(chat_id);
    return it == rooms.end() ? nullptr : it->second.get();
}

bool drop_room(int64_t chat_id) {
    std::lock_guard<std::mutex> g(rooms_mu);
    auto it = rooms.find(chat_id);
    if (it == rooms.end()) return false;
    it->second->stop();
    rooms.erase(it);
    return true;
}

std::unordered_map<int64_t, RoomState*> all_rooms() {
    std::lock_guard<std::mutex> g(rooms_mu);
    std::unordered_map<int64_t, RoomState*> out;
    for (auto& [k, v] : rooms) out[k] = v.get();
    return out;
}

}  // namespace advait
