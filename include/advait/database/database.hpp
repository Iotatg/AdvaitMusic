#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace advait::db {

struct PlaylistEntry {
    std::string title;
    std::string url;
    int duration{0};
    std::string requester;
};

struct FilterRow {
    std::string keyword;
    std::string text;
};

struct WarnRow {
    int64_t user_id{0};
    int64_t mod_id{0};
    int count{0};
    std::string reason;
};

using StickerPacks = std::map<std::string, std::vector<std::string>>;

bool init(const std::string& mongo_uri);
void shutdown();

int get_assistant(int64_t chat_id);
void init_assistant_indexes(int count);

bool is_sudo(int64_t user_id);
bool add_sudo(int64_t user_id);
bool del_sudo(int64_t user_id);
std::vector<int64_t> sudo_list();

bool is_authorized(int64_t chat_id, int64_t user_id);
bool add_auth_user(int64_t chat_id, int64_t user_id);
bool del_auth_user(int64_t chat_id, int64_t user_id);
std::vector<int64_t> auth_users(int64_t chat_id);

bool is_blacklisted_user(int64_t user_id);
bool add_blacklisted_user(int64_t user_id);
bool del_blacklisted_user(int64_t user_id);
std::vector<int64_t> blacklisted_users();

bool is_blacklisted_chat(int64_t chat_id);
bool add_blacklisted_chat(int64_t chat_id);
bool del_blacklisted_chat(int64_t chat_id);
std::vector<int64_t> blacklisted_chats();

bool welcome_on(int64_t chat_id);
void set_welcome_on(int64_t chat_id, bool on);
bool maintenance();
void set_maintenance(bool on);

std::string language(int64_t chat_id);
void set_language(int64_t chat_id, const std::string& lang);

bool play_mode_admins_only(int64_t chat_id);
void set_play_mode_admins_only(int64_t chat_id, bool value);
bool thumbnails_disabled(int64_t chat_id);
void set_thumbnails_disabled(int64_t chat_id, bool value);
bool autoplay(int64_t chat_id);
void set_autoplay(int64_t chat_id, bool value);

int64_t linked_channel(int64_t chat_id);
void set_linked_channel(int64_t chat_id, int64_t channel_id);

std::string get_note(int64_t chat_id, const std::string& key);
void set_note(int64_t chat_id, const std::string& key, const std::string& content);
std::vector<std::string> list_notes(int64_t chat_id);
bool delete_note(int64_t chat_id, const std::string& key);

void save_filter(int64_t chat_id, const std::string& keyword, const std::string& text);
std::vector<FilterRow> list_filters(int64_t chat_id);
bool delete_filter(int64_t chat_id, const std::string& key);
int clear_filters(int64_t chat_id);
bool match_filter(int64_t chat_id, const std::string& text, FilterRow* out);

int add_warning(int64_t chat_id, int64_t user_id, int64_t mod_id, const std::string& reason);
int warn_limit(int64_t chat_id);
std::string warn_mode(int64_t chat_id);
int clear_warnings(int64_t chat_id, int64_t user_id);
bool remove_last_warning(int64_t chat_id, int64_t user_id);
int count_warnings(int64_t chat_id, int64_t user_id);
std::vector<WarnRow> warnings_list(int64_t chat_id, int64_t user_id);
int clear_all_warnings(int64_t chat_id);
void set_warn_limit(int64_t chat_id, int value);
void set_warn_mode(int64_t chat_id, const std::string& mode);

void add_served_user(int64_t id);
void add_served_chat(int64_t id);
std::vector<int64_t> served_users();
std::vector<int64_t> served_chats();

void add_sticker_to_pack(const std::string& mood, const std::string& remote_id);
std::vector<std::string> sticker_pack_ids(const std::string& mood);
StickerPacks sticker_packs();
int clear_sticker_pack(const std::string& mood);

bool create_playlist(int64_t user_id, const std::string& name);
bool playlist_exists(int64_t user_id, const std::string& name);
bool delete_playlist(int64_t user_id, const std::string& name);
bool add_to_playlist(int64_t user_id, const std::string& name, const PlaylistEntry& entry);
bool remove_from_playlist(int64_t user_id, const std::string& name, size_t index);
std::vector<std::string> playlists_of(int64_t user_id);
std::vector<PlaylistEntry> playlist_entries(int64_t user_id, const std::string& name);

}  // namespace advait::db
