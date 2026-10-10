#pragma once

#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace advait {

using PlatformName = std::string;

inline constexpr const char* kPlatformYouTube = "YouTube";
inline constexpr const char* kPlatformSpotify = "Spotify";
inline constexpr const char* kPlatformDeezer = "Deezer";
inline constexpr const char* kPlatformIotaApi = "IotaApi";
inline constexpr const char* kPlatformTelegram = "Telegram";

struct Track {
    std::string id;
    std::string title;
    int duration{0};
    std::string artwork;
    std::string url;
    std::string requester;
    bool video{false};
    PlatformName source;
    bool is_autoplay{false};
    std::string engine_token;

    void mark_autoplay(const std::string& who, const std::string& token) {
        is_autoplay = true;
        requester = who;
        engine_token = token;
    }

    bool is_verified_autoplay(const std::string& token) const {
        if (!is_autoplay) return false;
        if (!token.empty() && engine_token != token) return false;
        return true;
    }
};

inline std::string normalize_track_title(std::string title) {
    if (title.empty()) return {};
    for (char& c : title) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::regex paren(R"([\(\[\{].*?[\)\]\}])");
    title = std::regex_replace(title, paren, " ");
    static const char* reps[] = {
        "official music video", "official video", "music video", "lyric video",
        "lyrics", "full video", "video song", "audio song", "audio track",
        "official audio", "audio", "4k", "8k", "hd", "remix", "song", "full track"};
    for (const char* r : reps) {
        for (;;) {
            auto p = title.find(r);
            if (p == std::string::npos) break;
            title.replace(p, std::char_traits<char>::length(r), " ");
        }
    }
    std::string out;
    out.reserve(title.size());
    bool sp = false;
    for (unsigned char c : title) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(c));
            sp = false;
        } else if (!sp) {
            out.push_back(' ');
            sp = true;
        }
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace advait
