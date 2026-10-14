#include "advait/platforms/platforms.hpp"
#include "advait/core/logger.hpp"

#include <algorithm>
#include <mutex>
#include <random>
#include <string>
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
    std::unordered_map<std::string, bool> hist;
    for (auto& h : history) hist[h] = true;

    std::vector<Track> filtered;
    auto accept = [&](const Track& t) {
        if (t.id.empty() && t.url.empty()) return false;
        if (!t.id.empty() && t.id == last.id) return false;
        if (!t.id.empty() && hist["id:" + t.id]) return false;
        auto n = normalize_track_title(t.title);
        if (!n.empty() && hist["title:" + n]) return false;
        return true;
    };
    auto fill = [&](const std::vector<Track>& in) {
        for (auto& t : in) {
            if (!accept(t)) continue;
            Track c = t;
            c.video = last.video;
            filtered.push_back(c);
            if (limit > 0 && static_cast<int>(filtered.size()) >= limit) break;
        }
    };

    // ── Tier 0: provider-agnostic similarity via iota (Kanha call.go Tier 0) ──
    // Works for any source; the /similar backend excludes the input itself, and
    // the prefixed form of the current track is filtered too. Limit 8 like the
    // reference.
    auto media = iota_media_id(last);
    if (!media.empty()) {
        for (auto& t : iota_similar(media, 8)) {
            if (!t.id.empty() && t.id == media) continue;
            if (!accept(t)) continue;
            Track c = t;
            c.video = last.video;
            filtered.push_back(c);
            if (limit > 0 && static_cast<int>(filtered.size()) >= limit) break;
        }
    }

    // ── Tier 1: YouTube Official Radio Mix (Kanha call.go) ─────────────────
    // "RD<videoId>" playlists return YouTube's own related-songs ranking;
    // the first 15 entries are the golden zone. Only consulted when iota
    // similarity had nothing, mirroring the reference ordering.
    if (filtered.empty()) {
        std::string vid;
        if (last.source == kPlatformYouTube) vid = last.id;
        else if (last.id.rfind("yt:", 0) == 0) vid = last.id.substr(3);
        if (vid.size() == 11) {
            if (auto* yt = get_platform(kPlatformYouTube)) {
                for (auto& c : yt->mix_for(vid, 15)) {
                    if (!accept(c)) continue;
                    Track x = c;
                    x.video = last.video;
                    filtered.push_back(x);
                }
            }
        }
    }

    // ── Tier 2: smart search fallback (Kanha Tier 2) ─────────────────────────
    // "<cleanTitle> similar songs" beats "<raw title> song" on vibe matching:
    // artists/lyrics sites dominate the query, and junk like "(Official Video)"
    // or "| [Lyrics]" is stripped from the title first.
    if (filtered.empty()) {
        auto* yt = get_platform(kPlatformYouTube);
        if (yt) fill(yt->get(clean_song_title_for_search(last.title) + " similar songs", last.video));
    }
    return filtered;
}

// clean_song_title_for_search strips noise suffixes so the autoplay fallback
// query reads like a real search (Kanha cleanSongTitleForSearch).
std::string clean_song_title_for_search(const std::string& title) {
    std::string t = title;
    for (const char* sep : {"|", "(", "["}) {
        auto p = t.find(sep);
        if (p != std::string::npos) t = t.substr(0, p);
    }
    auto b = t.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = t.find_last_not_of(" \t\r\n");
    return t.substr(b, e - b + 1);
}

const Track& golden_zone_pick(const std::vector<Track>& candidates) {
    // Golden-Zone Selection: pick from strictly the Top 4 closest matches.
    // The relevance ranking puts the closest matches first, so a random pick
    // from the top of the list both varies the queue and stays relevant
    // (Kanha call.go:345-357).
    static thread_local std::mt19937 rng{std::random_device{}()};
    size_t pool = candidates.size() < 4 ? candidates.size() : 4;
    return candidates[pool == 1 ? 0 : rng() % pool];
}

void init_platforms() {
    register_iota();
    register_yuki();
    register_youtube();
    ADVAIT_LOG_INFO("platforms", "registered youtube + iotaapi + yuki");
}

}  // namespace advait::platforms
