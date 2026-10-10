#include "advait/platforms/platforms.hpp"
#include "advait/core/config.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/utils.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace advait::platforms {

namespace {

size_t curl_write_file(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* f = static_cast<std::ofstream*>(userdata);
    f->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

size_t curl_write_str(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

struct XferCtx {
    const DownloadHooks* hooks;
    std::chrono::steady_clock::time_point start;
    curl_off_t last_size = 0;
    std::chrono::steady_clock::time_point last_time;
};

int curl_xferinfo(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* x = static_cast<XferCtx*>(userdata);
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

std::string curl_escape_path(const std::string& s) {
    CURL* c = curl_easy_init();
    if (!c) return s;
    char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string out = e ? e : s;
    if (e) curl_free(e);
    curl_easy_cleanup(c);
    return out;
}

bool is_transient(long code) {
    return code == 429 || code == 502 || code == 503 || code == 504 || code >= 500;
}

std::string sanitize(const std::string& e, const std::string& key) {
    if (key.empty()) return e;
    auto out = e;
    for (;;) {
        auto p = out.find(key);
        if (p == std::string::npos) break;
        out.replace(p, key.size(), "***REDACTED***");
    }
    return out;
}

std::string find_file(const Track& track) {
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

std::string get_path(const Track& track, const std::string& ext) {
    auto t = track.video ? "video" : "audio";
    return std::string("downloads/") + t + "_" + track.id + ext;
}

}  // namespace

std::string iota_media_id(const Track& t) {
    if (t.id.empty()) return {};
    if (t.source == kPlatformYouTube) return "yt:" + t.id;
    if (t.source == kPlatformSpotify) return "sp:" + t.id;
    if (t.source == kPlatformDeezer) return "dz:" + t.id;
    if (t.source == kPlatformIotaApi) return t.id;
    return {};
}

std::vector<Track> iota_similar(const std::string& media_id, int limit) {
    auto& cfg = Config::get();
    if (cfg.iota_api_url.empty() || cfg.iota_api_key.empty() || media_id.empty()) return {};
    if (limit <= 0 || limit > 20) limit = 8;
    auto base = cfg.iota_api_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    auto url = base + "/similar/" + curl_escape_path(media_id) + "?key=" + cfg.iota_api_key +
               "&limit=" + std::to_string(limit);
    std::string body;
    CURL* c = curl_easy_init();
    if (!c) return {};
    struct curl_slist* h = nullptr;
    auto ua = std::string("User-Agent: AdvaitMusic/iota");
    auto keyh = std::string("X-API-Key: ") + cfg.iota_api_key;
    h = curl_slist_append(h, ua.c_str());
    h = curl_slist_append(h, keyh.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_str);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    long st = 0;
    auto rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || st >= 400) return {};
    try {
        auto j = nlohmann::json::parse(body);
        std::vector<Track> out;
        for (auto& item : j.value("results", nlohmann::json::array())) {
            Track t;
            t.id = item.value("media_id", "");
            if (t.id.empty()) continue;
            t.title = item.value("title", "");
            auto artist = item.value("artist", "");
            if (!artist.empty() && !t.title.empty()) t.title += " - " + artist;
            t.duration = item.value("duration_seconds", 0);
            t.artwork = item.value("thumbnail_url", "");
            t.source = kPlatformIotaApi;
            out.push_back(t);
        }
        return out;
    } catch (...) {
        return {};
    }
}

class IotaApiPlatform : public Platform {
public:
    PlatformName name() const override { return kPlatformIotaApi; }
    int priority() const override { return 95; }
    bool can_get(const std::string&) const override { return false; }
    std::vector<Track> get(const std::string&, bool) override { return {}; }
    bool can_download(const PlatformName& source) const override {
        auto& cfg = Config::get();
        if (cfg.iota_api_url.empty() || cfg.iota_api_key.empty()) return false;
        return source == kPlatformYouTube || source == kPlatformSpotify || source == kPlatformDeezer ||
               source == kPlatformIotaApi;
    }

    std::string download(const Track& track, std::string* err, const DownloadHooks* hooks = nullptr) override {
        if (auto p = find_file(track); !p.empty()) {
            ADVAIT_LOG_DEBUG("IotaApi", "cache hit " + p);
            return p;
        }
        auto media_id = iota_media_id(track);
        if (media_id.empty()) {
            if (err) *err = "iotaapi: missing or unsupported media id";
            return {};
        }
        std::string kind = "audio", ext = ".m4a";
        long timeout = 180;
        if (track.video) {
            kind = "video";
            ext = ".mp4";
            timeout = 600;
        }
        auto path = get_path(track, ext);
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        auto& cfg = Config::get();
        auto base = cfg.iota_api_url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        auto stream = base + "/download/" + curl_escape_path(media_id) + "?kind=" + kind + "&key=" + cfg.iota_api_key;
        if (kind == "video") stream += "&max_height=1080";

        std::string last;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            if (hooks && hooks->cancel.load()) {
                if (err) *err = "canceled";
                return {};
            }
            if (attempt > 1) std::this_thread::sleep_for(std::chrono::seconds(1));
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                last = "iotaapi: cannot open " + path;
                continue;
            }
            CURL* c = curl_easy_init();
            if (!c) {
                last = "iotaapi: curl init failed";
                continue;
            }
            struct curl_slist* h = nullptr;
            auto ua = std::string("User-Agent: AdvaitMusic/iota");
            auto keyh = std::string("X-API-Key: ") + cfg.iota_api_key;
            h = curl_slist_append(h, ua.c_str());
            h = curl_slist_append(h, keyh.c_str());
            XferCtx xctx{hooks, std::chrono::steady_clock::now(), 0, std::chrono::steady_clock::now()};
            curl_easy_setopt(c, CURLOPT_URL, stream.c_str());
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_file);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            if (hooks) {
                curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curl_xferinfo);
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
                last = sanitize(std::string("iotaapi request failed: ") + curl_easy_strerror(rc), cfg.iota_api_key);
                continue;
            }
            if (st >= 400) {
                std::filesystem::remove(path);
                last = sanitize("iotaapi returned " + std::to_string(st), cfg.iota_api_key);
                if (!is_transient(st)) {
                    if (err) *err = last;
                    return {};
                }
                continue;
            }
            std::error_code ec;
            auto sz = std::filesystem::file_size(path, ec);
            if (ec || sz < 10000) {
                std::filesystem::remove(path);
                last = "iotaapi returned an invalid or empty file";
                continue;
            }
            return path;
        }
        if (err) *err = last;
        return {};
    }
};

void register_iota() { register_platform(std::make_unique<IotaApiPlatform>()); }

}  // namespace advait::platforms
