#include "advait/platforms/platforms.hpp"
#include "advait/core/config.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/utils.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <regex>
#include <unordered_map>

namespace advait::platforms {

namespace {

constexpr const char* kInnerTubeKey = "AIzaSyBOti4mM-6x9WDnZIjIeyEU21OpBXqWBgw";
constexpr const char* kInnerTubeVer = "2.20250101.01.00";
constexpr const char* kInnerTubeName = "WEB";

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string http_post(const std::string& url, const std::string& body, long* status) {
    std::string out;
    CURL* c = curl_easy_init();
    if (!c) return {};
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Linux; Android 13) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    auto rc = curl_easy_perform(c);
    if (status) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return {};
    return out;
}

std::string http_get(const std::string& url, long* status) {
    std::string out;
    CURL* c = curl_easy_init();
    if (!c) return {};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "AdvaitMusic/cpp");
    auto rc = curl_easy_perform(c);
    if (status) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return {};
    return out;
}

int parse_duration(const std::string& s) {
    int total = 0, mult = 1;
    std::string cur;
    std::vector<int> parts;
    for (char ch : s) {
        if (ch == ':') {
            parts.push_back(std::atoi(cur.c_str()));
            cur.clear();
        } else
            cur.push_back(ch);
    }
    if (!cur.empty()) parts.push_back(std::atoi(cur.c_str()));
    for (int i = static_cast<int>(parts.size()) - 1; i >= 0; --i) {
        total += parts[static_cast<size_t>(i)] * mult;
        mult *= 60;
    }
    return total;
}

std::string jstr(const nlohmann::json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number()) return std::to_string(j.get<int64_t>());
    return {};
}

nlohmann::json dig(nlohmann::json cur, std::initializer_list<nlohmann::json> path) {
    for (auto& k : path) {
        if (k.is_string()) {
            if (!cur.is_object() || !cur.contains(k.get<std::string>())) return nullptr;
            cur = cur[k.get<std::string>()];
        } else if (k.is_number_integer()) {
            auto i = k.get<int>();
            if (!cur.is_array() || i >= static_cast<int>(cur.size())) return nullptr;
            cur = cur[i];
        }
    }
    return cur;
}

std::string thumb_url(const nlohmann::json& vid) {
    nlohmann::json thumbs;
    if (vid.contains("thumbnail") && vid["thumbnail"].contains("thumbnails"))
        thumbs = vid["thumbnail"]["thumbnails"];
    else if (vid.contains("videoDetails") && vid["videoDetails"].contains("thumbnail"))
        thumbs = vid["videoDetails"]["thumbnail"]["thumbnails"];
    if (thumbs.is_array() && !thumbs.empty()) return jstr(thumbs.back()["url"]);
    return {};
}

bool is_live(const nlohmann::json& vid) {
    if (vid.contains("badges") && vid["badges"].is_array()) {
        for (auto& b : vid["badges"]) {
            if (jstr(dig(b, {"metadataBadgeRenderer", "style"})) == "BADGE_STYLE_TYPE_LIVE_NOW") return true;
        }
    }
    auto vc = jstr(dig(vid, {"viewCountText", "runs", 0, "text"}));
    return utils::to_lower(vc).find("watching") != std::string::npos;
}

const std::regex& re_yt_link() {
    static const std::regex re(R"(^(?:https?://)?(?:www\.|m\.|music\.)?(?:youtube\.com|youtu\.be)/\S+)",
                               std::regex::ECMAScript | std::regex::icase);
    return re;
}
const std::regex& re_vid1() {
    static const std::regex re(R"((?:youtube\.com/(?:watch\?v=|embed/|shorts/|live/)|youtu\.be/)([A-Za-z0-9_-]{11}))",
                               std::regex::ECMAScript | std::regex::icase);
    return re;
}
const std::regex& re_vid2() {
    static const std::regex re(R"((?:v=|/)([0-9A-Za-z_-]{11}))");
    return re;
}
const std::regex& re_pl1() {
    static const std::regex re(R"((?:youtube\.com|music\.youtube\.com).*(?:\?|&)list=([A-Za-z0-9_-]+))",
                               std::regex::ECMAScript | std::regex::icase);
    return re;
}
const std::regex& re_pl2() {
    static const std::regex re(R"(list=([0-9A-Za-z_-]+))");
    return re;
}

}  // namespace

class YouTubePlatform : public Platform {
public:
    PlatformName name() const override { return kPlatformYouTube; }
    int priority() const override { return 90; }
    bool can_get(const std::string& query) const override { return std::regex_search(query, re_yt_link()); }
    bool can_download(const PlatformName&) const override { return false; }
    std::string download(const Track&, std::string* err, const DownloadHooks* = nullptr) override {
        if (err) *err = "youtube platform does not support downloading";
        return {};
    }

    std::vector<Track> get(const std::string& input, bool video) override {
        auto query = utils::trim(input);
        if (query.empty()) return {};
        std::vector<Track> tracks;
        if (!std::regex_search(query, re_yt_link())) {
            tracks = perform_search(query, Config::get().queue_limit);
        } else {
            auto pl = extract_playlist(query);
            auto vid = extract_video(query);
            if (!pl.empty() && !vid.empty()) {
                auto v = handle_track(query);
                auto p = handle_playlist(query);
                if (!v.empty()) {
                    tracks = v;
                    for (auto& t : p)
                        if (t.id != v[0].id) tracks.push_back(t);
                } else
                    tracks = p.empty() ? v : p;
            } else if (!pl.empty())
                tracks = handle_playlist(query);
            else
                tracks = handle_track(query);
        }
        for (auto& t : tracks) t.video = video;
        return tracks;
    }

    std::string extract_video(const std::string& u) {
        std::smatch m;
        if (std::regex_search(u, m, re_vid1()) && m.size() > 1) return m[1];
        if (std::regex_search(u, m, re_vid2()) && m.size() > 1) return m[1];
        return {};
    }
    std::string extract_playlist(const std::string& u) {
        std::smatch m;
        if (std::regex_search(u, m, re_pl1()) && m.size() > 1) return m[1];
        if (std::regex_search(u, m, re_pl2()) && m.size() > 1) return m[1];
        return {};
    }

    nlohmann::json call_innertube(const std::string& endpoint, const nlohmann::json& payload) {
        auto url = std::string("https://m.youtube.com/youtubei/v1/") + endpoint + "?key=" + kInnerTubeKey;
        long st = 0;
        auto body = http_post(url, payload.dump(), &st);
        if (st >= 400 || body.empty()) return nullptr;
        try {
            return nlohmann::json::parse(body);
        } catch (...) {
            return nullptr;
        }
    }

    void parse_nodes(const nlohmann::json& node, std::vector<Track>& tracks, int limit, const std::string& key) {
        if (limit > 0 && static_cast<int>(tracks.size()) >= limit) return;
        if (node.is_array()) {
            for (auto& item : node) parse_nodes(item, tracks, limit, key);
            return;
        }
        if (!node.is_object()) return;
        if (node.contains(key) && node[key].is_object()) {
            auto vid = node[key];
            if (key == "videoRenderer" && is_live(vid)) return;
            auto id = jstr(vid.value("videoId", ""));
            if (id.empty()) return;
            std::string duration_text = jstr(dig(vid, {"lengthText", "simpleText"}));
            if (duration_text.empty()) duration_text = jstr(dig(vid, {"lengthText", "runs", 0, "text"}));
            std::string title = jstr(dig(vid, {"title", "runs", 0, "text"}));
            if (title.empty()) title = jstr(dig(vid, {"title", "simpleText"}));
            if (title.empty()) return;
            Track t;
            t.url = "https://www.youtube.com/watch?v=" + id;
            t.title = title;
            t.id = id;
            t.artwork = thumb_url(vid);
            t.duration = parse_duration(duration_text);
            t.source = kPlatformYouTube;
            tracks.push_back(t);
            return;
        }
        for (auto it = node.begin(); it != node.end(); ++it) parse_nodes(it.value(), tracks, limit, key);
    }

    std::vector<Track> perform_search(const std::string& query, int limit) {
        nlohmann::json payload = {
            {"context", {{"client", {{"clientName", kInnerTubeName}, {"clientVersion", kInnerTubeVer}, {"hl", "en-IN"}, {"gl", "IN"}}}}},
            {"query", query},
            {"params", "CAASAhAB"},
        };
        auto result = call_innertube("search", payload);
        if (result.is_null()) return {};
        auto contents = dig(result, {"contents", "twoColumnSearchResultsRenderer", "primaryContents", "sectionListRenderer", "contents"});
        std::vector<Track> tracks;
        parse_nodes(contents, tracks, limit, "videoRenderer");
        return tracks;
    }

    std::vector<Track> handle_track(const std::string& raw) {
        auto video_id = extract_video(raw);
        if (video_id.empty()) return {};
        nlohmann::json payload = {
            {"context", {{"client", {{"clientName", kInnerTubeName}, {"clientVersion", kInnerTubeVer}}}}},
            {"videoId", video_id},
        };
        auto result = call_innertube("player", payload);
        if (!result.is_null() && result.contains("videoDetails")) {
            auto d = result["videoDetails"];
            Track t;
            t.id = jstr(d.value("videoId", video_id));
            t.title = jstr(d.value("title", ""));
            t.url = "https://www.youtube.com/watch?v=" + t.id;
            t.artwork = thumb_url(result);
            t.duration = std::atoi(jstr(d.value("lengthSeconds", "0")).c_str());
            t.source = kPlatformYouTube;
            if (!t.title.empty()) return {t};
        }
        auto found = perform_search(video_id, 8);
        for (auto& t : found)
            if (t.id == video_id) return {t};
        return found.empty() ? std::vector<Track>{} : std::vector<Track>{found.front()};
    }

    std::vector<Track> handle_playlist(const std::string& raw) {
        auto playlist_id = extract_playlist(raw);
        if (playlist_id.empty()) return {};
        if (playlist_id.rfind("RD", 0) == 0) return fetch_mix(playlist_id, Config::get().queue_limit);
        std::string browse = playlist_id;
        if (browse.rfind("VL", 0) != 0) browse = "VL" + browse;
        nlohmann::json payload = {
            {"context", {{"client", {{"clientName", kInnerTubeName}, {"clientVersion", kInnerTubeVer}}}}},
            {"browseId", browse},
        };
        auto result = call_innertube("browse", payload);
        std::vector<Track> tracks;
        parse_nodes(result, tracks, Config::get().queue_limit, "playlistVideoRenderer");
        return tracks;
    }

    std::vector<Track> fetch_mix(const std::string& playlist_id, int limit) {
        nlohmann::json payload = {
            {"context", {{"client", {{"clientName", kInnerTubeName}, {"clientVersion", kInnerTubeVer}}}}},
            {"playlistId", playlist_id},
        };
        auto result = call_innertube("next", payload);
        auto items = dig(result, {"contents", "twoColumnWatchNextResults", "playlist", "playlist", "contents"});
        std::vector<Track> tracks;
        if (!items.is_array()) return tracks;
        for (auto& item : items) {
            if (limit > 0 && static_cast<int>(tracks.size()) >= limit) break;
            if (!item.contains("playlistPanelVideoRenderer")) continue;
            auto vid = item["playlistPanelVideoRenderer"];
            auto id = jstr(vid.value("videoId", ""));
            if (id.empty()) continue;
            Track t;
            t.url = "https://www.youtube.com/watch?v=" + id;
            t.title = jstr(dig(vid, {"title", "simpleText"}));
            t.id = id;
            t.artwork = thumb_url(vid);
            t.duration = parse_duration(jstr(dig(vid, {"lengthText", "simpleText"})));
            t.source = kPlatformYouTube;
            tracks.push_back(t);
        }
        return tracks;
    }
};

void register_youtube() { register_platform(std::make_unique<YouTubePlatform>()); }

}  // namespace advait::platforms
