#include "advait/ntgcalls/client.hpp"
#include "advait/core/logger.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <sstream>

#ifdef ADVAIT_WITH_NTGCALLS
extern "C" {
#include "ntgcalls.h"
}
#endif

namespace advait::ntg {

#ifdef ADVAIT_WITH_NTGCALLS
namespace {
struct Future {
    std::mutex mu;
    std::condition_variable cv;
    bool done{false};
    // Written directly by libntgcalls while the future references this struct;
    // must be read only after wait_future() reports completion.
    int error{0};
    char* message{nullptr};
};

void promise_cb(void* p) {
    auto* f = static_cast<Future*>(p);
    std::lock_guard<std::mutex> g(f->mu);
    f->done = true;
    f->cv.notify_all();
}

ntg_async_struct make_future(Future* f) {
    ntg_async_struct a{};
    a.userData = f;
    a.errorCode = &f->error;
    a.errorMessage = &f->message;
    a.promise = promise_cb;
    return a;
}

bool wait_future(Future* f) {
    std::unique_lock<std::mutex> lk(f->mu);
    f->cv.wait_for(lk, std::chrono::seconds(30), [&] { return f->done; });
    return f->done && f->error == 0;
}

void log_cb(ntg_log_message_struct m) {
    const char* lvl = "?";
    switch (m.level) {
        case NTG_LOG_DEBUG: lvl = "TRACE"; break;
        case NTG_LOG_INFO: lvl = "DEBUG"; break;
        case NTG_LOG_WARNING: lvl = "WARN"; break;
        case NTG_LOG_ERROR: lvl = "ERROR"; break;
        default: break;
    }
    ADVAIT_LOG_INFO("ntgcore", std::string(lvl) + " [" + (m.file ? m.file : "") + ":" + std::to_string(m.line) + "] " +
                                     (m.message ? m.message : ""));
}
}  // namespace
#endif

Client::Client() {
#ifdef ADVAIT_WITH_NTGCALLS
    ntg_register_logger(log_cb);
    ptr_ = ntg_init();

    ADVAIT_LOG_INFO("ntgcalls", "ntg_init ptr=" + std::to_string(ptr_));
#else
    ADVAIT_LOG_WARN("ntgcalls", "built without libntgcalls; playback is a no-op");
#endif
}

Client::~Client() { close(); }

void Client::close() {
#ifdef ADVAIT_WITH_NTGCALLS
    if (ptr_) {
        ntg_destroy(ptr_);
        ptr_ = 0;
    }
#endif
}

bool Client::create(int64_t chat_id, std::string* sdp) {
#ifdef ADVAIT_WITH_NTGCALLS
    Future f;
    char* buf = nullptr;
    int sync = ntg_create(ptr_, chat_id, &buf, make_future(&f));
    if (sync != 0 || !wait_future(&f)) {
        std::ostringstream os;
        os << "create failed sync=" << sync << " err=" << f.error << " msg=" << (f.message ? f.message : "");
        ADVAIT_LOG_ERROR("ntgcalls", os.str());
        return false;
    }
    if (sdp && buf) {
        *sdp = buf;
        std::free(buf);
    }
    return true;
#else
    (void)chat_id;
    (void)sdp;
    return true;
#endif
}

bool Client::connect(int64_t chat_id, const std::string& params) {
#ifdef ADVAIT_WITH_NTGCALLS
    Future f;
    ntg_connect(ptr_, chat_id, const_cast<char*>(params.c_str()), false, make_future(&f));
    return wait_future(&f);
#else
    (void)chat_id;
    (void)params;
    return true;
#endif
}

bool Client::set_stream_sources(int64_t chat_id, const MediaDescription& desc) {
#ifdef ADVAIT_WITH_NTGCALLS
    ntg_media_description_struct d{};
    ntg_audio_description_struct mic{};
    ntg_video_description_struct cam{};
    if (desc.microphone) {
        mic.mediaSource = static_cast<ntg_media_source_enum>(desc.microphone->media_source);
        mic.input = const_cast<char*>(desc.microphone->input.c_str());
        mic.sampleRate = desc.microphone->sample_rate;
        mic.channelCount = desc.microphone->channel_count;
        d.microphone = &mic;
    }
    if (desc.camera) {
        cam.mediaSource = static_cast<ntg_media_source_enum>(desc.camera->media_source);
        cam.input = const_cast<char*>(desc.camera->input.c_str());
        cam.width = desc.camera->width;
        cam.height = desc.camera->height;
        cam.fps = desc.camera->fps;
        d.camera = &cam;
    }
    Future f;
    ntg_set_stream_sources(ptr_, chat_id, NTG_STREAM_CAPTURE, d, make_future(&f));
    return wait_future(&f);
#else
    (void)chat_id;
    (void)desc;
    return true;
#endif
}

bool Client::pause(int64_t chat_id) {
#ifdef ADVAIT_WITH_NTGCALLS
    Future f;
    ntg_pause(ptr_, chat_id, make_future(&f));
    return wait_future(&f);
#else
    (void)chat_id;
    return true;
#endif
}

bool Client::resume(int64_t chat_id) {
#ifdef ADVAIT_WITH_NTGCALLS
    Future f;
    ntg_resume(ptr_, chat_id, make_future(&f));
    return wait_future(&f);
#else
    (void)chat_id;
    return true;
#endif
}

bool Client::mute(int64_t chat_id) {
#ifdef ADVAIT_WITH_NTGCALLS
    Future f;
    ntg_mute(ptr_, chat_id, make_future(&f));
    return wait_future(&f);
#else
    (void)chat_id;
    return true;
#endif
}

bool Client::unmute(int64_t chat_id) {
#ifdef ADVAIT_WITH_NTGCALLS
    Future f;
    ntg_unmute(ptr_, chat_id, make_future(&f));
    return wait_future(&f);
#else
    (void)chat_id;
    return true;
#endif
}

bool Client::stop(int64_t chat_id) {
#ifdef ADVAIT_WITH_NTGCALLS
    Future f;
    ntg_stop(ptr_, chat_id, make_future(&f));
    return wait_future(&f);
#else
    (void)chat_id;
    return true;
#endif
}

int64_t Client::time(int64_t chat_id) {
#ifdef ADVAIT_WITH_NTGCALLS
    Future f;
    int64_t t = 0;
    ntg_time(ptr_, chat_id, NTG_STREAM_CAPTURE, &t, make_future(&f));
    wait_future(&f);
    return t;
#else
    (void)chat_id;
    return 0;
#endif
}

void Client::on_stream_end(StreamEndCallback cb) {
    std::lock_guard<std::mutex> g(mu_);
    stream_end_.push_back(std::move(cb));
#ifdef ADVAIT_WITH_NTGCALLS
    ntg_on_stream_end(
        ptr_,
        [](uintptr_t, int64_t chat, ntg_stream_type_enum, ntg_stream_device_enum, void* ud) {
            auto* self = static_cast<Client*>(ud);
            std::vector<StreamEndCallback> cbs;
            {
                std::lock_guard<std::mutex> g(self->mu_);
                cbs = self->stream_end_;
            }
            for (auto& c : cbs) c(chat);
        },
        this);
#endif
}

MediaDescription media_from_file(const std::string& path, int pos, double speed, bool video) {
    if (speed < 0.5) speed = 0.5;
    if (speed > 4.0) speed = 4.0;
    std::ostringstream base;
    base << "ffmpeg ";
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)
        base << "-reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5 ";
    if (pos > 0) base << "-ss " << pos << " ";
    base << "-v warning -i \"" << path << "\" ";

    static thread_local AudioDescription audio;
    audio.media_source = 1 << 1;  // NTG_SHELL
    audio.sample_rate = 96000;
    audio.channel_count = 2;
    std::ostringstream ac;
    ac << base.str() << "-filter:a \"atempo=" << std::fixed << speed << "\" "
       << "-f s16le -ac " << static_cast<int>(audio.channel_count) << " -ar " << audio.sample_rate << " pipe:1";
    audio.input = ac.str();

    MediaDescription d;
    d.microphone = &audio;
    if (!video) return d;

    static thread_local VideoDescription cam;
    cam.media_source = 1 << 1;
    cam.width = 1280;
    cam.height = 720;
    cam.fps = 30;
    std::ostringstream vc;
    vc << base.str() << "-filter:v \"scale=1280:720:force_original_aspect_ratio=decrease,"
       << "pad=1280:720:(ow-iw)/2:(oh-ih)/2,fps=30,setpts=PTS/" << speed << "\" "
       << "-f rawvideo -r 30 -pix_fmt yuv420p pipe:1";
    cam.input = vc.str();
    d.camera = &cam;
    return d;
}

const std::string& version() {
#ifdef ADVAIT_WITH_NTGCALLS
    static const std::string v = [] {
        char* buf = nullptr;
        ntg_get_version(&buf);
        std::string s = buf ? buf : "unknown";
        std::free(buf);
        return s;
    }();
#else
    static const std::string v = std::string("unknown");
#endif
    return v;
}

}  // namespace advait::ntg
