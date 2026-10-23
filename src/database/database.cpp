#include "advait/database/database.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace advait::db {
namespace {

using json = nlohmann::json;

struct ChatState {
    std::vector<int64_t> authorized_list;
    bool blacklisted{false};
    bool play_mode_admins_only{false};
    bool thumbnails_disabled{false};
    bool autoplay{false};
    bool welcome_on{false};
    bool maintenance{false};
    int64_t linked_channel{0};
    std::string language;
    int max_auth_users{25};
    int warn_limit{3};
    std::string warn_mode{"ban"};
    std::map<int64_t, std::vector<WarnRow>> warnings;
};

struct State {
    std::map<int64_t, bool> sudo_users;
    std::map<int64_t, bool> blacklisted_users;
    std::map<int64_t, bool> blacklisted_chats;
    std::map<int64_t, ChatState> chats;
    std::map<int64_t, std::map<std::string, std::string>> notes;
    std::map<int64_t, std::map<std::string, std::string>> filters;
    std::set<int64_t> served_users;
    std::set<int64_t> served_chats;
    StickerPacks stickers;
    std::map<int64_t, std::map<std::string, std::vector<PlaylistEntry>>> playlists;
    std::map<int64_t, int> assistant_by_chat;
    int assistant_count{0};
};

std::string trim_copy(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (unsigned char)s[b] < 0x21) ++b;
    while (e > b && (unsigned char)s[e - 1] < 0x21) --e;
    return s.substr(b, e - b);
}

std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

json state_to_json(const State& st) {
    json j = json::object();
    json sudo = json::array();
    for (auto& [uid, on] : st.sudo_users) {
        if (on) sudo.push_back(uid);
    }
    j["sudo_users"] = sudo;
    json bl_users = json::array();
    for (auto& [uid, on] : st.blacklisted_users) {
        if (on) bl_users.push_back(uid);
    }
    j["blacklisted_users"] = bl_users;
    json bl_chats = json::array();
    for (auto& [cid, on] : st.blacklisted_chats) {
        if (on) bl_chats.push_back(cid);
    }
    j["blacklisted_chats"] = bl_chats;
    json chats = json::object();
    for (auto& [cid, c] : st.chats) {
        json jc = json::object();
        json auth = json::array();
        for (auto& uid : c.authorized_list) auth.push_back(uid);
        jc["authorized"] = auth;
        jc["blacklisted"] = c.blacklisted;
        jc["play_mode_admins_only"] = c.play_mode_admins_only;
        jc["thumbnails_disabled"] = c.thumbnails_disabled;
        jc["autoplay"] = c.autoplay;
        jc["welcome_on"] = c.welcome_on;
        jc["maintenance"] = c.maintenance;
        jc["linked_channel"] = c.linked_channel;
        jc["language"] = c.language;
        jc["max_auth_users"] = c.max_auth_users;
        jc["warn_limit"] = c.warn_limit;
        jc["warn_mode"] = c.warn_mode;
        json warns = json::object();
        for (auto& [uid, rows] : c.warnings) {
            json arr = json::array();
            for (auto& r : rows) {
                arr.push_back(json{{"user_id", r.user_id}, {"mod_id", r.mod_id}, {"count", r.count}, {"reason", r.reason}});
            }
            warns[std::to_string(uid)] = arr;
        }
        jc["warnings"] = warns;
        chats[std::to_string(cid)] = jc;
    }
    j["chats"] = chats;
    json notes = json::object();
    for (auto& [cid, m] : st.notes) {
        json jm = json::object();
        for (auto& [k, v] : m) jm[k] = v;
        notes[std::to_string(cid)] = jm;
    }
    j["notes"] = notes;
    json filters = json::object();
    for (auto& [cid, m] : st.filters) {
        json jm = json::object();
        for (auto& [k, v] : m) jm[k] = v;
        filters[std::to_string(cid)] = jm;
    }
    j["filters"] = filters;
    j["served_users"] = st.served_users;
    j["served_chats"] = st.served_chats;
    json stickers = json::object();
    for (auto& [mood, ids] : st.stickers) stickers[mood] = ids;
    j["stickers"] = stickers;
    json playlists = json::object();
    for (auto& [uid, m] : st.playlists) {
        json jm = json::object();
        for (auto& [name, entries] : m) {
            json arr = json::array();
            for (auto& e : entries) {
                arr.push_back(json{{"title", e.title}, {"url", e.url}, {"duration", e.duration}, {"requester", e.requester}});
            }
            jm[name] = arr;
        }
        playlists[std::to_string(uid)] = jm;
    }
    j["playlists"] = playlists;
    json assistants = json::object();
    for (auto& [cid, idx] : st.assistant_by_chat) assistants[std::to_string(cid)] = idx;
    j["assistants"] = assistants;
    j["assistant_count"] = st.assistant_count;
    return j;
}

std::string state_path() {
    const char* env = std::getenv("ADVAIT_DB_PATH");
    return env && *env ? std::string(env) : std::string("database/advait_db.json");
}

State& state();
void save_state() {
    State& st = state();
    auto path = state_path();
    if (path.empty()) return;
    auto slash = path.find_last_of('/');
    auto dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
    if (!dir.empty()) mkdir(dir.c_str(), 0755);
    std::ofstream out(path);
    out << state_to_json(st).dump(4);
}

State& state() {
    static State s;
    return s;
}

}  // namespace

bool init(const std::string& /*mongo_uri*/) { return true; }

void shutdown() { save_state(); }

int get_assistant(int64_t chat_id) {
    State& st = state();
    auto it = st.assistant_by_chat.find(chat_id);
    if (it != st.assistant_by_chat.end()) return it->second;
    return st.assistant_count > 0 ? std::min(1, st.assistant_count) : 1;
}

void init_assistant_indexes(int count) { state().assistant_count = count; }

bool is_sudo(int64_t user_id) {
    State& st = state();
    auto it = st.sudo_users.find(user_id);
    return it != st.sudo_users.end() && it->second;
}

bool add_sudo(int64_t user_id) {
    State& st = state();
    auto [it, inserted] = st.sudo_users.emplace(user_id, true);
    save_state();
    return inserted;
}

bool del_sudo(int64_t user_id) {
    State& st = state();
    auto it = st.sudo_users.find(user_id);
    if (it == st.sudo_users.end()) return false;
    it->second = false;
    save_state();
    return true;
}

std::vector<int64_t> sudo_list() {
    State& st = state();
    std::vector<int64_t> out;
    for (auto& [uid, on] : st.sudo_users) {
        if (on) out.push_back(uid);
    }
    return out;
}

bool is_authorized(int64_t chat_id, int64_t user_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    if (it == st.chats.end()) return false;
    if (it->second.play_mode_admins_only) return false;
    for (int64_t uid : it->second.authorized_list) {
        if (uid == user_id) return true;
    }
    return false;
}

bool add_auth_user(int64_t chat_id, int64_t user_id) {
    State& st = state();
    auto& c = st.chats[chat_id];
    if (std::find(c.authorized_list.begin(), c.authorized_list.end(), user_id) != c.authorized_list.end()) return false;
    c.authorized_list.push_back(user_id);
    save_state();
    return true;
}

bool del_auth_user(int64_t chat_id, int64_t user_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    if (it == st.chats.end()) return false;
    auto jt = std::find(it->second.authorized_list.begin(), it->second.authorized_list.end(), user_id);
    if (jt == it->second.authorized_list.end()) return false;
    it->second.authorized_list.erase(jt);
    save_state();
    return true;
}

std::vector<int64_t> auth_users(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    if (it == st.chats.end()) return {};
    return it->second.authorized_list;
}

bool is_blacklisted_user(int64_t user_id) {
    State& st = state();
    auto it = st.blacklisted_users.find(user_id);
    return it != st.blacklisted_users.end() && it->second;
}

bool add_blacklisted_user(int64_t user_id) {
    State& st = state();
    st.blacklisted_users[user_id] = true;
    save_state();
    return true;
}

bool del_blacklisted_user(int64_t user_id) {
    State& st = state();
    st.blacklisted_users[user_id] = false;
    save_state();
    return true;
}

std::vector<int64_t> blacklisted_users() {
    State& st = state();
    std::vector<int64_t> out;
    for (auto& [uid, on] : st.blacklisted_users) {
        if (on) out.push_back(uid);
    }
    return out;
}

bool is_blacklisted_chat(int64_t chat_id) {
    State& st = state();
    auto it = st.blacklisted_chats.find(chat_id);
    return it != st.blacklisted_chats.end() && it->second;
}

bool add_blacklisted_chat(int64_t chat_id) {
    State& st = state();
    st.blacklisted_chats[chat_id] = true;
    save_state();
    return true;
}

bool del_blacklisted_chat(int64_t chat_id) {
    State& st = state();
    st.blacklisted_chats[chat_id] = false;
    save_state();
    return true;
}

std::vector<int64_t> blacklisted_chats() {
    State& st = state();
    std::vector<int64_t> out;
    for (auto& [cid, on] : st.blacklisted_chats) {
        if (on) out.push_back(cid);
    }
    return out;
}

bool welcome_on(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    return it != st.chats.end() && it->second.welcome_on;
}

void set_welcome_on(int64_t chat_id, bool on) {
    State& st = state();
    st.chats[chat_id].welcome_on = on;
    save_state();
}

bool maintenance() {
    State& st = state();
    auto it = st.chats.find(0);
    return it != st.chats.end() && it->second.maintenance;
}

void set_maintenance(bool on) {
    State& st = state();
    st.chats[0].maintenance = on;
    save_state();
}

std::string language(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    return it == st.chats.end() ? std::string() : it->second.language;
}

void set_language(int64_t chat_id, const std::string& lang) {
    State& st = state();
    st.chats[chat_id].language = lang;
    save_state();
}

bool play_mode_admins_only(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    return it != st.chats.end() && it->second.play_mode_admins_only;
}

void set_play_mode_admins_only(int64_t chat_id, bool value) {
    State& st = state();
    st.chats[chat_id].play_mode_admins_only = value;
    save_state();
}

bool thumbnails_disabled(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    return it != st.chats.end() && it->second.thumbnails_disabled;
}

void set_thumbnails_disabled(int64_t chat_id, bool value) {
    State& st = state();
    st.chats[chat_id].thumbnails_disabled = value;
    save_state();
}

bool autoplay(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    return it != st.chats.end() && it->second.autoplay;
}

void set_autoplay(int64_t chat_id, bool value) {
    State& st = state();
    st.chats[chat_id].autoplay = value;
    save_state();
}

std::string get_note(int64_t chat_id, const std::string& key) {
    State& st = state();
    auto it = st.notes.find(chat_id);
    if (it == st.notes.end()) return std::string();
    auto kt = it->second.find(key);
    return kt == it->second.end() ? std::string() : kt->second;
}

void set_note(int64_t chat_id, const std::string& key, const std::string& content) {
    State& st = state();
    st.notes[chat_id][key] = content;
    save_state();
}

std::vector<std::string> list_notes(int64_t chat_id) {
    State& st = state();
    std::vector<std::string> out;
    auto it = st.notes.find(chat_id);
    if (it == st.notes.end()) return out;
    for (auto& [k, v] : it->second) out.push_back(k);
    return out;
}

bool delete_note(int64_t chat_id, const std::string& key) {
    State& st = state();
    auto it = st.notes.find(chat_id);
    if (it == st.notes.end()) return false;
    auto kt = it->second.find(key);
    if (kt == it->second.end()) return false;
    it->second.erase(kt);
    save_state();
    return true;
}

void save_filter(int64_t chat_id, const std::string& keyword, const std::string& text) {
    State& st = state();
    st.filters[chat_id][to_lower_copy(keyword)] = text;
    save_state();
}

std::vector<FilterRow> list_filters(int64_t chat_id) {
    State& st = state();
    std::vector<FilterRow> out;
    auto it = st.filters.find(chat_id);
    if (it == st.filters.end()) return out;
    for (auto& [k, v] : it->second) out.push_back(FilterRow{k, v});
    return out;
}

bool delete_filter(int64_t chat_id, const std::string& key) {
    State& st = state();
    auto it = st.filters.find(chat_id);
    if (it == st.filters.end()) return false;
    auto kt = it->second.find(to_lower_copy(key));
    if (kt == it->second.end()) return false;
    it->second.erase(kt);
    save_state();
    return true;
}

int clear_filters(int64_t chat_id) {
    State& st = state();
    auto it = st.filters.find(chat_id);
    if (it == st.filters.end()) return 0;
    int n = static_cast<int>(it->second.size());
    it->second.clear();
    save_state();
    return n;
}

bool match_filter(int64_t chat_id, const std::string& text, FilterRow* out) {
    State& st = state();
    auto it = st.filters.find(chat_id);
    if (it == st.filters.end()) return false;
    std::string t = to_lower_copy(text);
    std::vector<std::string> toks;
    std::string tok;
    for (char c : t) {
        if (c == ' ' || c == '\t') {
            if (!tok.empty()) toks.push_back(tok);
            tok.clear();
        } else {
            tok.push_back(c);
        }
    }
    if (!tok.empty()) toks.push_back(tok);
    for (auto& tk : toks) {
        auto ft = it->second.find(tk);
        if (ft != it->second.end()) {
            if (out) {
                out->keyword = ft->first;
                out->text = ft->second;
            }
            return true;
        }
    }
    return false;
}

int add_warning(int64_t chat_id, int64_t user_id, int64_t mod_id, const std::string& reason) {
    State& st = state();
    auto& vec = st.chats[chat_id].warnings[user_id];
    for (auto& r : vec) {
        if (r.mod_id == mod_id) {
            r.count += 1;
            r.reason = reason;
            save_state();
            int total = 0;
            for (auto& x : vec) total += x.count;
            return total;
        }
    }
    WarnRow w;
    w.user_id = user_id;
    w.mod_id = mod_id;
    w.count = 1;
    w.reason = reason;
    vec.push_back(w);
    save_state();
    int total = 0;
    for (auto& x : vec) total += x.count;
    return total;
}

int warn_limit(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    return it == st.chats.end() ? 3 : it->second.warn_limit;
}

std::string warn_mode(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    return it == st.chats.end() ? std::string("ban") : it->second.warn_mode;
}

int clear_warnings(int64_t chat_id, int64_t user_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    if (it == st.chats.end()) return 0;
    auto ut = it->second.warnings.find(user_id);
    if (ut == it->second.warnings.end()) return 0;
    int total = 0;
    for (auto& r : ut->second) total += r.count;
    it->second.warnings.erase(ut);
    save_state();
    return total;
}

bool remove_last_warning(int64_t chat_id, int64_t user_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    if (it == st.chats.end()) return false;
    auto ut = it->second.warnings.find(user_id);
    if (ut == it->second.warnings.end() || ut->second.empty()) return false;
    ut->second.pop_back();
    save_state();
    return true;
}

int count_warnings(int64_t chat_id, int64_t user_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    if (it == st.chats.end()) return 0;
    auto ut = it->second.warnings.find(user_id);
    if (ut == it->second.warnings.end()) return 0;
    int total = 0;
    for (auto& r : ut->second) total += r.count;
    return total;
}

std::vector<WarnRow> warnings_list(int64_t chat_id, int64_t user_id) {
    State& st = state();
    std::vector<WarnRow> out;
    auto it = st.chats.find(chat_id);
    if (it == st.chats.end()) return out;
    auto ut = it->second.warnings.find(user_id);
    if (ut == it->second.warnings.end()) return out;
    for (auto& r : ut->second) out.push_back(r);
    return out;
}

int clear_all_warnings(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    if (it == st.chats.end()) return 0;
    int total = 0;
    for (auto& [uid, rows] : it->second.warnings) {
        for (auto& r : rows) total += r.count;
    }
    it->second.warnings.clear();
    save_state();
    return total;
}

void set_warn_limit(int64_t chat_id, int value) {
    State& st = state();
    st.chats[chat_id].warn_limit = value;
    save_state();
}

void set_warn_mode(int64_t chat_id, const std::string& mode) {
    State& st = state();
    st.chats[chat_id].warn_mode = mode;
    save_state();
}

void add_served_user(int64_t id) { state().served_users.insert(id); }

void add_served_chat(int64_t id) { state().served_chats.insert(id); }

std::vector<int64_t> served_users() {
    State& st = state();
    return std::vector<int64_t>(st.served_users.begin(), st.served_users.end());
}

std::vector<int64_t> served_chats() {
    State& st = state();
    return std::vector<int64_t>(st.served_chats.begin(), st.served_chats.end());
}

void add_sticker_to_pack(const std::string& mood, const std::string& remote_id) {
    State& st = state();
    auto& ids = st.stickers[mood];
    if (std::find(ids.begin(), ids.end(), remote_id) == ids.end()) ids.push_back(remote_id);
}

std::vector<std::string> sticker_pack_ids(const std::string& mood) {
    State& st = state();
    auto it = st.stickers.find(mood);
    if (it == st.stickers.end()) return {};
    return it->second;
}

StickerPacks sticker_packs() {
    State& st = state();
    return st.stickers;
}

int clear_sticker_pack(const std::string& mood) {
    State& st = state();
    if (mood == "all") {
        int n = 0;
        for (auto& [m, ids] : st.stickers) n += static_cast<int>(ids.size());
        st.stickers.clear();
        save_state();
        return n;
    }
    auto it = st.stickers.find(mood);
    if (it == st.stickers.end()) return 0;
    int n = static_cast<int>(it->second.size());
    st.stickers.erase(it);
    save_state();
    return n;
}

bool create_playlist(int64_t user_id, const std::string& name) {
    State& st = state();
    auto& m = st.playlists[user_id];
    if (m.find(name) != m.end()) return false;
    m[name] = {};
    save_state();
    return true;
}

bool playlist_exists(int64_t user_id, const std::string& name) {
    State& st = state();
    auto ut = st.playlists.find(user_id);
    if (ut == st.playlists.end()) return false;
    return ut->second.find(name) != ut->second.end();
}

bool delete_playlist(int64_t user_id, const std::string& name) {
    State& st = state();
    auto ut = st.playlists.find(user_id);
    if (ut == st.playlists.end()) return false;
    auto it = ut->second.find(name);
    if (it == ut->second.end()) return false;
    ut->second.erase(it);
    save_state();
    return true;
}

bool add_to_playlist(int64_t user_id, const std::string& name, const PlaylistEntry& entry) {
    State& st = state();
    auto& m = st.playlists[user_id];
    auto it = m.find(name);
    if (it == m.end()) return false;
    it->second.push_back(entry);
    save_state();
    return true;
}

bool remove_from_playlist(int64_t user_id, const std::string& name, size_t index) {
    State& st = state();
    auto ut = st.playlists.find(user_id);
    if (ut == st.playlists.end()) return false;
    auto it = ut->second.find(name);
    if (it == ut->second.end()) return false;
    if (index >= it->second.size()) return false;
    it->second.erase(it->second.begin() + static_cast<long>(index));
    save_state();
    return true;
}

std::vector<std::string> playlists_of(int64_t user_id) {
    State& st = state();
    std::vector<std::string> out;
    auto ut = st.playlists.find(user_id);
    if (ut == st.playlists.end()) return out;
    for (auto& [name, entries] : ut->second) out.push_back(name);
    return out;
}

std::vector<PlaylistEntry> playlist_entries(int64_t user_id, const std::string& name) {
    State& st = state();
    std::vector<PlaylistEntry> out;
    auto ut = st.playlists.find(user_id);
    if (ut == st.playlists.end()) return out;
    auto it = ut->second.find(name);
    if (it == ut->second.end()) return out;
    return it->second;
}

int64_t linked_channel(int64_t chat_id) {
    State& st = state();
    auto it = st.chats.find(chat_id);
    if (it == st.chats.end()) return 0;
    return it->second.linked_channel;
}

void set_linked_channel(int64_t chat_id, int64_t channel_id) {
    State& st = state();
    st.chats[chat_id].linked_channel = channel_id;
    save_state();
}

}  // namespace advait::db

