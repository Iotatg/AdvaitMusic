#pragma once

#include "advait/core/models.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace advait::platforms {

// DownloadHooks lets the caller observe and abort an in-flight download,
// mirroring Go's context.Context cancellation and UpdateFile progress edits.
struct DownloadHooks {
    std::atomic<bool> cancel{false};
    // percentage 0-100 (0 when total unknown), bytes_per_sec, eta_sec, elapsed_sec
    std::function<void(double percentage, double bytes_per_sec, long eta_sec, double elapsed_sec)> progress;
};

// Fetches artwork into cache/art/ and returns the cached local path, "" on
// failure. TDLib inputFile needs a local file; remote URLs are not accepted.
std::string artwork_local_path(const std::string& url);

class Platform {
public:
    virtual ~Platform() = default;
    virtual PlatformName name() const = 0;
    virtual int priority() const = 0;
    virtual bool can_get(const std::string& query) const = 0;
    virtual std::vector<Track> get(const std::string& query, bool video) = 0;
    virtual bool can_download(const PlatformName& source) const = 0;
    virtual std::string download(const Track& track, std::string* err, const DownloadHooks* hooks = nullptr) = 0;
};

void register_platform(std::unique_ptr<Platform> p);
Platform* get_platform(const PlatformName& name);
std::vector<Track> resolve_query(const std::string& query, bool video);
std::string download_track(const Track& track, std::string* err, const DownloadHooks* hooks = nullptr);
std::vector<Track> search_tracks(const std::string& query, bool video);
std::vector<Track> autoplay_tracks(const Track& last, int limit, const std::vector<std::string>& history);
std::string iota_media_id(const Track& t);
std::vector<Track> iota_similar(const std::string& media_id, int limit);

void init_platforms();

}  // namespace advait::platforms
