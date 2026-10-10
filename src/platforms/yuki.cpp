#include "advait/platforms/platforms.hpp"
#include "advait/core/config.hpp"
#include "advait/core/logger.hpp"

#include <curl/curl.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace advait::platforms {

namespace {

size_t yk_write_file(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* f = static_cast<std::ofstream*>(userdata);
    f->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

struct YkXferCtx {
    const DownloadHooks* hooks;
    std::chrono::steady_clock::time_point start;
    curl_off_t last_size = 0;
    std::chrono::steady_clock::time_point last_time;
};

int yk_xferinfo(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* x = static_cast<YkXferCtx*>(userdata);
    if (!x || !x->hooks) return 0;
    if (x->hooks->cancel.load()) return 1;
    if (!x->hooks->progress) return 0;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - x->start).count();
    double interval = std::chrono::duration<double>(now - x->last_time).count();
    double speed = (interval > 0 && dlnow >= x->last_size) ? static_cast<double>(dlnow - x->last_size) / interval : 0.0;
    x->last_size = dlnow;
    x->last_time = now;
    double pct = dltotal > 0 ? static_cast<double>(dlnow) / static_cast<double>(dltotal) * 100.0 : 0.0;
    long eta = 0;
    if (speed > 0 && dltotal > dlnow) eta = static_cast<long>(static_cast<double>(dltotal - dlnow) / speed);
    x->hooks->progress(pct, speed, eta, elapsed);
    return 0;
}

std::string yk_escape(const std::string& s) {
    CURL* c = curl_easy_init();
    if (!c) return s;
    char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string out = e ? e : s;
    if (e) curl_free(e);
    curl_easy_cleanup(c);
    return out;
}

bool yk_transient(long code) {
    return code == 429 || code >= 500;
}

std::string yk_sanitize(const std::string& e, const std::string& key) {
    if (key.empty()) return e;
    auto out = e;
    for (;;) {
        auto p = out.find(key);
        if (p == std::string::npos) break;
        out.replace(p, key.size(), "***REDACTED***");
    }
    return out;
}

std::string yk_find_file(const Track& track) {
    auto t = track.video ? "video" : "audio";
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists("downloads", ec)) return {};
    for (auto& p : fs::directory_iterator("downloads", ec)) {
        auto name = p.path().filename().string();
        if (name.rfind(std::string(t) + "_" + track.id, 0) == 0 && p.file_size(ec) > 0) return p.path().string();
    }
    return {};
}

}  // namespace

class YukiPlatform : public Platform {
public:
    PlatformName name() const override { return "Yuki"; }
    int priority() const override { return 85; }
    bool can_get(const std::string&) const override { return false; }
    std::vector<Track> get(const std::string&, bool) override { return {}; }
    bool can_download(const PlatformName& source) const override {
        auto& cfg = Config::get();
        if (cfg.yuki_api_url.empty() || cfg.yuki_api_key.empty()) return false;
        return source == kPlatformYouTube;
    }

    std::string download(const Track& track, std::string* err, const DownloadHooks* hooks = nullptr) override {
        if (auto p = yk_find_file(track); !p.empty()) {
            ADVAIT_LOG_DEBUG("Yuki", "cache hit " + p);
            return p;
        }
        if (track.id.empty()) {
            if (err) *err = "yuki: missing video id";
            return {};
        }
        std::string type = "audio", quality = "128", ext = ".mp3";
        long timeout = 180;
        if (track.video) {
            type = "video";
            quality = "480";
            ext = ".mp4";
            timeout = 600;
        }
        std::string path = std::string("downloads/") + (track.video ? "video_" : "audio_") + track.id + ext;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        auto& cfg = Config::get();
        auto base = cfg.yuki_api_url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        auto stream = base + "/stream/" + yk_escape(track.id) + "?key=" + cfg.yuki_api_key +
                      "&type=" + type + "&quality=" + quality;

        std::string last;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            if (hooks && hooks->cancel.load()) {
                if (err) *err = "canceled";
                return {};
            }
            if (attempt > 1) std::this_thread::sleep_for(std::chrono::seconds(1));
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                last = "yuki: cannot open " + path;
                continue;
            }
            CURL* c = curl_easy_init();
            if (!c) {
                last = "yuki: curl init failed";
                continue;
            }
            struct curl_slist* h = nullptr;
            auto ua = std::string("User-Agent: AdvaitMusic/yuki");
            h = curl_slist_append(h, ua.c_str());
            YkXferCtx xctx{hooks, std::chrono::steady_clock::now(), 0, std::chrono::steady_clock::now()};
            curl_easy_setopt(c, CURLOPT_URL, stream.c_str());
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, yk_write_file);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            if (hooks) {
                curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, yk_xferinfo);
                curl_easy_setopt(c, CURLOPT_XFERINFODATA, &xctx);
            }
            long st = 0;
            auto rc = curl_easy_perform(c);
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
            curl_slist_free_all(h);
            curl_easy_cleanup(c);
            out.close();
            if (rc == CURLE_ABORTED_BY_CALLBACK) {
                std::filesystem::remove(path);
                if (err) *err = "canceled";
                return {};
            }
            if (rc != CURLE_OK) {
                std::filesystem::remove(path);
                last = yk_sanitize(std::string("yuki request failed: ") + curl_easy_strerror(rc), cfg.yuki_api_key);
                continue;
            }
            if (st >= 400) {
                std::filesystem::remove(path);
                last = yk_sanitize("yuki returned " + std::to_string(st), cfg.yuki_api_key);
                if (!yk_transient(st)) {
                    if (err) *err = last;
                    return {};
                }
                continue;
            }
            std::error_code ec;
            auto sz = std::filesystem::file_size(path, ec);
            if (ec || sz < 10000) {
                std::filesystem::remove(path);
                last = "yuki returned an invalid or empty file";
                continue;
            }
            return path;
        }
        if (err) *err = last;
        return {};
    }
};

void register_yuki() { register_platform(std::make_unique<YukiPlatform>()); }

}  // namespace advait::platforms
