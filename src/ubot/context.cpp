#include "advait/ubot/context.hpp"
#include "advait/core/logger.hpp"

namespace advait::ubot {

Context::Context(std::shared_ptr<mtproto::Client> mt)
    : binding_(std::make_unique<ntg::Client>()), mt_(std::move(mt)) {}
Context::~Context() { close(); }

bool Context::connected(int64_t chat_id) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = calls_.find(chat_id);
    return it != calls_.end() && it->second.joined;
}

// Mirrors ubot.connect_call group branch: CreateCall -> SetStreamSources ->
// GetInputGroupCall -> PhoneJoinGroupCall -> Connect(serverParams).
bool Context::join_locked(int64_t chat_id, const ntg::MediaDescription& desc, std::string* err) {
    Mounted& m = calls_[chat_id];
    std::string offer;
    if (!binding_->create(chat_id, &offer)) {
        binding_->stop(chat_id);
        if (err) {
            *err = "webrtc create failed";
        }
        return false;
    }
    if (!binding_->set_stream_sources(chat_id, desc)) {
        binding_->stop(chat_id);
        if (err) {
            *err = "set_stream_sources failed";
        }
        return false;
    }
    if (!mt_->get_group_call(chat_id, &m.call, err)) {
        binding_->stop(chat_id);
        return false;
    }
    std::string server;
    if (!mt_->join_group_call(m.call, offer, &server, err)) {
        binding_->stop(chat_id);
        return false;
    }
    if (!binding_->connect(chat_id, server)) {
        binding_->stop(chat_id);
        if (err) {
            *err = "webrtc connect failed";
        }
        return false;
    }
    m.joined = true;
    ADVAIT_LOG_INFO("ubot", "joined group call for chat " + std::to_string(chat_id));
    return true;
}

bool Context::play(int64_t chat_id, const ntg::MediaDescription& desc, std::string* err) {
    if (!binding_ || !mt_) {
        return false;
    }
    std::lock_guard<std::mutex> g(mu_);
    std::string cerr;
    if (!mt_->connect(&cerr)) {
        if (err) {
            *err = "mtproto: " + cerr;
        }
        return false;
    }
    auto it = calls_.find(chat_id);
    if (it == calls_.end() || !it->second.joined) {
        return join_locked(chat_id, desc, err);
    }
    if (!binding_->set_stream_sources(chat_id, desc)) {
        if (err) {
            *err = "set_stream_sources failed";
        }
        return false;
    }
    return true;
}

bool Context::pause(int64_t chat_id) { return binding_ && binding_->pause(chat_id); }
bool Context::resume(int64_t chat_id) { return binding_ && binding_->resume(chat_id); }
bool Context::mute(int64_t chat_id) { return binding_ && binding_->mute(chat_id); }
bool Context::unmute(int64_t chat_id) { return binding_ && binding_->unmute(chat_id); }

bool Context::stop(int64_t chat_id) {
    if (!binding_) {
        return false;
    }
    bool ok = binding_->stop(chat_id);
    std::lock_guard<std::mutex> g(mu_);
    calls_.erase(chat_id);
    return ok;
}

void Context::on_stream_end(ntg::StreamEndCallback cb) {
    if (binding_) binding_->on_stream_end(std::move(cb));
}

void Context::on_call_discarded(std::function<void(int64_t)> cb) {
    std::lock_guard<std::mutex> g(mu_);
    discarded_.push_back(std::move(cb));
}

void Context::close() {
    std::lock_guard<std::mutex> g(mu_);
    calls_.clear();
    if (binding_) binding_->close();
}

}  // namespace advait::ubot
