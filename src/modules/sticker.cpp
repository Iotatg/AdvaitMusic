// Sticker reply engine — ports Aishamusic-2 modules/sticker.go (which itself
// ports Iota's sticker_reply.py). A received sticker is classified into a mood
// by its emoji (falling back to its pack's emojis, then message text), saved
// into a crowd-sourced mood pool, and answered with a real sticker of the same
// mood ("sticker to sticker"). Trigger rules: DMs always, groups only on a
// reply to the bot or an @mention. Before replying the bot raises the
// "choosing a sticker" chat action.
#include "advait/modules/sticker.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "advait/core/config.hpp"
#include "advait/core/logger.hpp"
#include "advait/core/utils.hpp"
#include "advait/database/database.hpp"
#include "advait/modules/gutil.hpp"

namespace advait::modules {
namespace {
using namespace advait::modules::gutil;
using tg::Message;
using tg::TdClient;

// Aishamusic-2 core.OwnerOnlyPlain.
const char* kOwnerOnly = "\xF0\x9F\xA7\x8F\xE2\x80\x8D\xE2\x99\x82\xEF\xB8\x8F S\xCA\x9C\xCA\x9C\xE2\x80\xA6 T\xCA\x9C\xE1\xB4\x80\xCA\x9C'\xCA\x9C M\xCA\x8F O\xCA\xB7\xE1\xB4\x8B\xE1\xB4\x87\xCA\x80 \xF0\x9F\x98\x8F";
inline std::string owner_only() { return kOwnerOnly; }


// ── mood detection (mirrors Iota's _EMOJI_MOOD / _MOOD_RELATED) ─────────────
// Single code points only: a two-rune spelling such as U+2764 U+FE0F can never
// match, matching the Go map's rune-key design.
const std::map<std::string, std::string>& mood_emoji() {
    static const std::map<std::string, std::string> m = {
        {"\U0001F602", "laugh"},    {"\U0001F923", "laugh"},    {"\U0001F639", "laugh"},
        {"\U0001F480", "laugh"},    {"\U0001F60D", "love"},     {"\U0001F970", "love"},
        {"\U0001F495", "love"},     {"\U0001F49E", "love"},     {"\u2764", "love"},
        {"\U0001FAF6", "love"},     {"\U0001F49D", "love"},     {"\U0001F498", "love"},
        {"\U0001F62D", "sad"},      {"\U0001F622", "sad"},      {"\U0001F494", "sad"},
        {"\U0001F614", "sad"},      {"\U0001F61E", "sad"},      {"\U0001F621", "angry"},
        {"\U0001F92C", "angry"},    {"\U0001F624", "angry"},    {"\U0001F4A2", "angry"},
        {"\U0001F973", "happy"},    {"\U0001F60A", "happy"},    {"\U0001F601", "happy"},
        {"\U0001F389", "happy"},    {"\U0001F604", "happy"},    {"\U0001F92E", "happy"},
        {"\U0001F38A", "happy"},    {"\U0001F62E", "surprise"}, {"\U0001F631", "surprise"},
        {"\U0001F632", "surprise"}, {"\U0001F440", "surprise"}, {"\U0001F483", "dance"},
        {"\U0001F57A", "dance"},    {"\U0001F3B5", "dance"},    {"\U0001F3B6", "dance"},
        {"\U0001F60E", "cool"},     {"\U0001F919", "cool"},     {"\u270C", "cool"},
        {"\U0001F97A", "cute"},     {"\U0001F338", "cute"},     {"\u2728", "cute"},
        {"\U0001F337", "cute"},     {"\U0001F351", "cute"},     {"\U0001F33C", "cute"},
    };
    return m;
}

const std::map<std::string, std::vector<std::string>>& mood_related() {
    static const std::map<std::string, std::vector<std::string>> m = {
        {"happy", {"laugh", "dance", "cute", "cool"}},
        {"laugh", {"happy", "cute", "dance"}},
        {"love", {"cute", "sad", "happy"}},
        {"cute", {"love", "happy", "laugh"}},
        {"sad", {"love", "cute"}},
        {"angry", {"cool", "surprise"}},
        {"surprise", {"cool", "angry"}},
        {"dance", {"happy", "cool", "laugh"}},
        {"cool", {"happy", "dance", "surprise"}},
    };
    return m;
}

struct MoodKw {
    std::vector<const char*> words;
    const char* mood;
};
const std::vector<MoodKw>& mood_keywords() {
    static const std::vector<MoodKw> m = {
        {{"dance", "vibe", "groove"}, "dance"},
        {{"love", "kiss", "heart", "pyaar"}, "love"},
        {{"funny", "lol", "haha", "laugh"}, "laugh"},
        {{"angry", "mad", "rage", "gussa"}, "angry"},
        {{"cute", "kawaii", "aww", "soft"}, "cute"},
    };
    return m;
}

// Iterates code points (not bytes) so emoji lookups behave like Go's runes.
template <typename F>
void for_each_codepoint(const std::string& s, F&& fn) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) len = 1;
        fn(s.substr(i, len));
        i += len;
    }
}

std::string mood_from_emoji(const std::string& s) {
    const auto& emap = mood_emoji();
    std::string result = "default";
    for_each_codepoint(s, [&](const std::string& cp) {
        if (result != "default") return;
        auto it = emap.find(cp);
        if (it != emap.end()) result = it->second;
    });
    return result;
}

std::string mood_from_text(const std::string& text) {
    auto mood = mood_from_emoji(text);
    if (mood != "default") return mood;
    auto low = utils::to_lower(text);
    for (auto& g : mood_keywords())
        for (auto* w : g.words)
            if (low.find(w) != std::string::npos) return g.mood;
    return "default";
}

// ── sticker emoji lookup with per-set cache (stickerSetTTL = 6h) ────────────
// TDLib >= 1.8.66 layout: messageSticker["sticker"] is a `sticker` object
// (id/set_id/emoji) that itself contains `sticker:file`. value() throws
// type_error.306 on non-object receivers — a thrown error in a message hook
// kills the whole dispatch — so every field read below is guarded, and when
// parsing looks wrong we log the raw node once for diagnosis.
std::string sticker_file_id(const json& content) {
    auto s = content.find("sticker");
    if (s == content.end() || !s->is_object()) return {};
    // The sendable id is the REMOTE file id on the embedded file:
    // sticker["sticker"]["remote"]["id"] — a stable string usable via
    // inputFileRemote (Aishamusic-2: stickerRemoteID returns st.Sticker.Remote.Id).
    // The local numeric id is session-scoped and goes stale on restart.
    auto f = s->find("sticker");
    if (f != s->end() && f->is_object()) {
        auto r = f->find("remote");
        if (r != f->end() && r->is_object()) {
            auto rid = r->find("id");
            if (rid != r->end() && rid->is_string()) return rid->get<std::string>();
        }
    }
    return {};
}

std::string str_field(const json& j, const char* key) { return jstr(j, key); }

std::string sticker_emoji_for(TdClient& bot, const json& content) {
    if (!content.is_object()) return {};
    auto st = content.find("sticker");
    if (st == content.end() || !st->is_object()) return {};
    std::string emoji = str_field(*st, "emoji");
    if (!emoji.empty()) return emoji;
    int64_t set_id = 0;
    if (auto sid = st->find("set_id"); sid != st->end() && sid->is_number()) set_id = sid->get<int64_t>();
    if (set_id == 0) return {};

    std::string st_key;
    if (auto idp = st->find("id"); idp != st->end() && idp->is_number()) st_key = std::to_string(idp->get<int64_t>());
    if (st_key.empty()) return {};

    static std::mutex mu;
    static std::unordered_map<int64_t, std::pair<std::unordered_map<std::string, std::string>,
                                                 std::chrono::steady_clock::time_point>>
        cache;
    {
        std::lock_guard<std::mutex> g(mu);
        auto it = cache.find(set_id);
        if (it != cache.end() && std::chrono::steady_clock::now() - it->second.second <
                                     std::chrono::hours(6)) {
            auto& by = it->second.first;
            auto j = by.find(st_key);
            if (j != by.end()) return j->second;
            // Cache is authoritative for the TTL window; don't refetch per message.
            return {};
        }
    }

    auto set = bot.get_sticker_set(set_id);
    if (set.is_null()) {
        ADVAIT_LOG_WARN("sticker",
                        "getStickerSet(" + std::to_string(set_id) + ") failed; set cached as empty");
        std::lock_guard<std::mutex> g(mu);
        cache[set_id] = {{}, std::chrono::steady_clock::now()};
        return {};
    }
    std::unordered_map<std::string, std::string> by;
    if (set.contains("stickers") && set["stickers"].is_array()) {
        for (auto& s : set["stickers"]) {
            if (!s.is_object()) continue;
            std::string e = str_field(s, "emoji");
            if (e.empty()) continue;
            if (auto id = s.find("id"); id != s.end() && id->is_number())
                by[std::to_string(id->get<int64_t>())] = std::move(e);
        }
    }
    {
        std::lock_guard<std::mutex> g(mu);
        cache[set_id] = {by, std::chrono::steady_clock::now()};
    }
    ADVAIT_LOG_INFO("sticker",
                    "cached emoji for set=" + std::to_string(set_id) + " stickers=" + std::to_string(by.size()));
    auto j = by.find(st_key);
    return j == by.end() ? std::string() : j->second;
}

// stickerMoodFor parity: sticker emoji, set emoji, then message text.
std::string mood_for(TdClient& bot, const Message& m, const json& content) {
    auto mood = mood_from_emoji(sticker_emoji_for(bot, content));
    if (mood != "default") return mood;
    if (mood = mood_from_text(m.text); mood != "default") return mood;
    return "default";
}

// Sendable id of a sticker: the REMOTE file id under
// content["sticker"]["sticker"]["remote"]["id"]. inputFileRemote is alive in
// TDLib 1.8.x; the remote id is what addresses a send across sessions
// (Aishamusic-2 sticker.go: "Packs store the sticker's remote file id (a
// string) and send with InputFileRemote").
std::string remote_id_of(const json& content) { return sticker_file_id(content); }

json sticker_content_of(const json& message) {
    if (!message.is_object()) return {};
    if (!message.contains("content") || !message["content"].is_object()) return {};
    auto& content = message["content"];
    auto t = content.find("@type");
    if (t == content.end() || !t->is_string() || t->get<std::string>() != "messageSticker") return {};
    return content;
}

// shouldStickerRespond parity: DMs always; groups on reply-to-bot or @mention.
bool should_respond(TdClient& bot, const Message& m) {
    if (m.is_private) return true;
    if (m.reply_to != 0) {
        auto rep = bot.get_replied_message(m.chat_id, m.id);
        if (rep.is_object()) {
            auto s = rep.find("sender_id");
            if (s != rep.end() && s->is_object()) {
                auto uid = s->find("user_id");
                if (uid != s->end() && uid->is_number() && uid->get<int64_t>() == bot.me_id()) return true;
            }
        }
    }
    std::string uname = utils::to_lower(bot.me_username());
    if (uname.empty()) return false;
    auto text = utils::to_lower(m.text);
    return text.find("@" + uname) != std::string::npos;
}

// ── send path ───────────────────────────────────────────────────────────────
bool sticker_send_one(TdClient& bot, const Message& m, const std::vector<std::string>& ids) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    if (ids.empty()) return false;
    const auto& id = ids[rng() % ids.size()];
    return bot.send_sticker_sync(m.chat_id, id, m.id) != 0;
}

std::vector<std::string> pick_other(const std::vector<std::string>& ids, const std::string& current) {
    std::vector<std::string> out;
    for (auto& i : ids)
        if (i != current && !i.empty()) out.push_back(i);
    return out;
}

// stickerReplyHandler parity: exact mood, then related moods, then any pack.
void sticker_engine(TdClient& bot, const Message& m) {
    auto st = sticker_content_of(m.raw);
    if (!st.is_object()) return;
    if (!should_respond(bot, m)) return;
    bot.send_chat_action(m.chat_id, "chatActionChoosingSticker");

    std::string mood = mood_for(bot, m, st);
    std::string incoming = remote_id_of(st);
    if (!incoming.empty()) db::add_sticker_to_pack(mood, incoming);
    ADVAIT_LOG_INFO("sticker",
                    "reply attempted mood=" + mood + " incoming=" + incoming.substr(0, 24) + "...");

    std::vector<std::string> chain{mood};
    auto rel = mood_related().find(mood);
    if (rel != mood_related().end())
        for (auto& r : rel->second) chain.push_back(r);
    for (auto& cand : chain) {
        auto pick = pick_other(db::sticker_pack_ids(cand), incoming);
        if (!pick.empty() && sticker_send_one(bot, m, pick)) return;
    }
    for (auto& [pack_mood, ids] : db::sticker_packs()) {
        (void)pack_mood;
        auto pick = pick_other(ids, incoming);
        if (!pick.empty() && sticker_send_one(bot, m, pick)) return;
    }
    ADVAIT_LOG_INFO("sticker", "no candidate to reply with (mood=" + mood + ")");
}

// ── owner curation commands ─────────────────────────────────────────────────
bool owner_only(const Message& m) { return Config::get().owner_id != 0 && Config::get().owner_id == m.sender_id; }

// Diagnostic: pull the last sticker from chat history and attempt sends with
// every InputFile variant, dumping the exact request and TDLib's response.
void stickerdiag_handler(TdClient& bot, const Message& m) {
    if (!owner_only(m)) {
        reply(bot, m, owner_only());
        return;
    }
    auto hist = bot.call_sync({{"@type", "getChatHistory"},
                               {"chat_id", m.chat_id},
                               {"from_message_id", 0},
                               {"offset", 0},
                               {"limit", 15},
                               {"only_local", false}},
                              15000);
    ADVAIT_LOG_INFO("sticker", "diag hist type=" + jstr(hist, "@type"));
    std::string remote_id, raw;
    int64_t local_id = 0;
    for (auto& mid : hist.value("messages", json::array())) {
        json c = mid.value("content", json::object());
        if (c.value("@type", "") != "messageSticker") continue;
        raw = c.dump();
        json s = c.value("sticker", json::object());
        json f = s.value("sticker", json::object());
        if (f.contains("remote")) remote_id = f["remote"].value("id", "");
        if (f.is_number()) local_id = f.get<int64_t>();
        break;
    }
    ADVAIT_LOG_INFO("sticker", "diag remote=" + remote_id + " local=" + std::to_string(local_id) +
                                    " raw=" + raw.substr(0, 600));
    reply(bot, m, styled("diag remote=" + remote_id.substr(0, 40) + " local=" + std::to_string(local_id)));

    struct V {
        const char* name;
        json sticker;
    };
    std::vector<V> vars;
    if (!remote_id.empty())
        vars.push_back({"remote", json{{"@type", "inputFileRemote"}, {"id", remote_id}}});
    if (local_id != 0)
        vars.push_back({"localid", json{{"@type", "inputFileId"}, {"id", local_id}}});
    for (auto& v : vars) {
        json q = {{"@type", "sendMessage"},
                  {"chat_id", m.chat_id},
                  {"input_message_content",
                   {{"@type", "inputMessageSticker"},
                    {"sticker", v.sticker},
                    {"width", 512},
                    {"height", 512},
                    {"emoji", ""}}}};
        auto r = bot.send_query_sync_public(q, 15000);
        ADVAIT_LOG_INFO("sticker", std::string("diag variant ") + v.name + " req=" + q.dump() +
                                        " resp=" + r.dump().substr(0, 300));
        reply(bot, m, styled(std::string("variant ") + v.name + ": " + jstr(r, "@type") + " " +
                             jstr(r, "message")));
    }
}

void add_sticker_handler(TdClient& bot, const Message& m) {
    if (!owner_only(m)) {
        reply(bot, m, owner_only());
        return;
    }
    ADVAIT_LOG_INFO("sticker",
                    "addsticker chat=" + std::to_string(m.chat_id) + " reply_to=" +
                        std::to_string(m.reply_to));
    if (m.reply_to == 0) {
        reply(bot, m, styled("\xF0\x9F\x93\x8E reply to a sticker to save it."));
        return;
    }
    auto rep = bot.get_replied_message(m.chat_id, m.id);
    ADVAIT_LOG_INFO("sticker",
                    "addsticker replied: obj=" + std::string(rep.is_object() ? "y" : "n") +
                        " type=" + jstr(rep, "@type") + " content_type=" +
                        jstr(rep.is_object() ? rep.value("content", json::object()) : json::object(), "@type"));
    auto st = sticker_content_of(rep);
    if (!st.is_object()) {
        reply(bot, m, styled("\xF0\x9F\x93\x8E reply to a sticker to save it."));
        return;
    }
    std::string remote_id = remote_id_of(st);
    if (remote_id.empty()) {
        reply(bot, m, styled("\xE2\x9D\x8C sticker has no remote file id."));
        return;
    }
    auto toks = fields(args_of(m));
    std::string mood = toks.empty() ? std::string() : utils::to_lower(toks[0]);
    if (mood.empty()) mood = mood_from_emoji(sticker_emoji_for(bot, st));
    db::add_sticker_to_pack(mood, remote_id);
    reply(bot, m, styled("\xE2\x9C\x85 added to <b>" + utils::escape_html(mood) + "</b>. " +
                         std::to_string(db::sticker_pack_ids(mood).size()) + " in pack."));
}

void sticker_packs_handler(TdClient& bot, const Message& m) {
    if (!owner_only(m)) {
        reply(bot, m, owner_only());
        return;
    }
    auto packs = db::sticker_packs();
    if (packs.empty()) {
        reply(bot, m, styled("\xF0\x9F\x93\xAD no sticker packs. /addsticker"));
        return;
    }
    std::string text = "\xF0\x9F\x8E\xAD <b>packs (" + std::to_string(packs.size()) + ")</b>\n";
    for (auto& [mood, ids] : packs) text += "\xE2\x80\xA2 <b>" + utils::escape_html(mood) + "</b> — " + std::to_string(ids.size()) + "\n";
    reply(bot, m, text);
}

void preview_sticker_handler(TdClient& bot, const Message& m) {
    auto toks = fields(args_of(m));
    if (toks.empty()) {
        reply(bot, m, "\xF0\x9F\x93\x8E usage: /previewsticker <mood>");
        return;
    }
    auto ids = db::sticker_pack_ids(utils::to_lower(toks[0]));
    if (ids.empty()) {
        reply(bot, m, styled("\xE2\x9D\x8C no stickers in <b>" + utils::escape_html(utils::to_lower(toks[0])) + "</b>"));
        return;
    }
    sticker_send_one(bot, m, ids);
}

void clear_stickers_handler(TdClient& bot, const Message& m) {
    if (!owner_only(m)) {
        reply(bot, m, owner_only());
        return;
    }
    auto toks = fields(args_of(m));
    if (toks.empty()) {
        reply(bot, m, "\xF0\x9F\x93\x8E usage: /clearstickers <mood|all>");
        return;
    }
    std::string target = utils::to_lower(toks[0]);
    int removed = db::clear_sticker_pack(target);
    if (removed == 0) {
        reply(bot, m, styled("\xE2\x9D\x8C <b>" + utils::escape_html(target) + "</b> not found."));
        return;
    }
    if (target == "all") {
        reply(bot, m, styled("\xF0\x9F\x97\x91\xEF\xB8\x8F removed " + std::to_string(removed)));
        return;
    }
    reply(bot, m, styled("\xF0\x9F\x97\x91\xEF\xB8\x8F removed pack <b>" + utils::escape_html(target) + "</b>"));
}

}  // namespace

void init_sticker(tg::TdClient& bot) {
    bot.on_message([&bot](const tg::Message& m) { sticker_engine(bot, m); });
    bot.on_command("addsticker", [&bot](const tg::Message& m) { add_sticker_handler(bot, m); });
    bot.on_command("stickerdiag", [&bot](const tg::Message& m) { stickerdiag_handler(bot, m); });
    bot.on_command("stickerpacks", [&bot](const tg::Message& m) { sticker_packs_handler(bot, m); });
    bot.on_command("previewsticker", [&bot](const tg::Message& m) { preview_sticker_handler(bot, m); });
    bot.on_command("clearstickers", [&bot](const tg::Message& m) { clear_stickers_handler(bot, m); });
    ADVAIT_LOG_INFO("sticker", "sticker reply engine registered");
}

}  // namespace advait::modules
