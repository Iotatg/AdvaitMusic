#include "advait/platforms/platforms.hpp"
#include "advait/core/logger.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace advait::platforms {

namespace {
std::mutex mu;
std::vector<std::unique_ptr<Platform>> all;
}

void register_youtube();
void register_iota();
void register_yuki();

void register_platform(std::unique_ptr<Platform> p) {
    std::lock_guard<std::mutex> g(mu);
    all.push_back(std::move(p));
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a->priority() > b->priority(); });
}

Platform* get_platform(const PlatformName& name) {
    std::lock_guard<std::mutex> g(mu);
    for (auto& p : all)
        if (p->name() == name) return p.get();
    return nullptr;
}

static std::vector<Platform*> ordered() {
    std::lock_guard<std::mutex> g(mu);
    std::vector<Platform*> out;
    for (auto& p : all) out.push_back(p.get());
    return out;
}

static Platform* find_for(const std::string& query) {
    for (auto* p : ordered())
        if (p->can_get(query)) return p;
    return nullptr;
}

std::vector<Track> resolve_query(const std::string& query, bool video) {
    if (auto* p = find_for(query)) {
        auto got = p->get(query, video);
        if (!got.empty()) return got;
    }
    auto* yt = get_platform(kPlatformYouTube);
    if (!yt) return {};
    auto tracks = yt->get(query, video);
    if (tracks.empty()) return {};
    return {tracks.front()};
}

std::string download_track(const Track& track, std::string* err, const DownloadHooks* hooks) {
    std::string last;
    for (auto* p : ordered()) {
        if (!p->can_download(track.source)) continue;
        std::string e;
        auto path = p->download(track, &e, hooks);
        if (!path.empty()) return path;
        last = p->name() + ": " + e;
        if (e == "canceled") break;
    }
    if (err) *err = last.empty() ? ("no downloader for source: " + track.source) : last;
    return {};
}

std::vector<Track> search_tracks(const std::string& query, bool video) {
    auto* yt = get_platform(kPlatformYouTube);
    if (!yt) return {};
    return yt->get(query, video);
}

std::vector<Track> autoplay_tracks(const Track& last, int limit, const std::vector<std::string>& history) {
    auto* yt = get_platform(kPlatformYouTube);
    if (!yt) return {};
    auto candidates = yt->get(last.title + " song", last.video);
    std::unordered_map<std::string, bool> hist;
    for (auto& h : history) hist[h] = true;
    std::vector<Track> filtered;
    for (auto& t : candidates) {
        if (t.id.empty() || t.id == last.id) continue;
        if (hist["id:" + t.id]) continue;
        auto n = normalize_track_title(t.title);
        if (!n.empty() && hist["title:" + n]) continue;
        t.video = last.video;
        filtered.push_back(t);
        if (limit > 0 && static_cast<int>(filtered.size()) >= limit) break;
    }
    return filtered;
}

void init_platforms() {
    register_iota();
    register_yuki();
    register_youtube();
    ADVAIT_LOG_INFO("platforms", "registered youtube + iotaapi + yuki");
}

}  // namespace advait::platforms
