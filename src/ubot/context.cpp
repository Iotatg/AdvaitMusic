#include "advait/ubot/context.hpp"
#include "advait/core/logger.hpp"

namespace advait::ubot {

Context::Context() : binding_(std::make_unique<ntg::Client>()) {}
Context::~Context() { close(); }

bool Context::play(int64_t chat_id, const ntg::MediaDescription& desc) {
    if (!binding_) return false;
    std::string sdp;
    if (!binding_->create(chat_id, &sdp)) {
        ADVAIT_LOG_WARN("ubot", "create returned false; trying set_stream_sources anyway");
    }
    return binding_->set_stream_sources(chat_id, desc);
}

bool Context::pause(int64_t chat_id) { return binding_ && binding_->pause(chat_id); }
bool Context::resume(int64_t chat_id) { return binding_ && binding_->resume(chat_id); }
bool Context::mute(int64_t chat_id) { return binding_ && binding_->mute(chat_id); }
bool Context::unmute(int64_t chat_id) { return binding_ && binding_->unmute(chat_id); }
bool Context::stop(int64_t chat_id) { return binding_ && binding_->stop(chat_id); }

void Context::on_stream_end(ntg::StreamEndCallback cb) {
    if (binding_) binding_->on_stream_end(std::move(cb));
}

void Context::on_call_discarded(std::function<void(int64_t)> cb) {
    std::lock_guard<std::mutex> g(mu_);
    discarded_.push_back(std::move(cb));
}

void Context::close() {
    if (binding_) binding_->close();
}

}  // namespace advait::ubot
