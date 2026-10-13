#include "advait/core/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace advait::utils {

std::string trim(std::string s) {
    auto notsp = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notsp));
    s.erase(std::find_if(s.rbegin(), s.rend(), notsp).base(), s.end());
    return s;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string getenv_str(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    if (!v) return def;
    auto t = trim(v);
    return t.empty() ? def : t;
}

bool parse_bool(const std::string& v, bool def) {
    if (v.empty()) return def;
    auto s = to_lower(v);
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return def;
}

int parse_int(const std::string& v, int def) {
    if (v.empty()) return def;
    try {
        return std::stoi(v);
    } catch (...) {
        return def;
    }
}

int64_t parse_int64(const std::string& v, int64_t def) {
    if (v.empty()) return def;
    try {
        return std::stoll(v);
    } catch (...) {
        return def;
    }
}

std::vector<std::string> split_fields(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v) {
        if (c == ',' || c == ';' || c == ' ') {
            auto t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    auto t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

std::string format_bytes(int64_t size) {
    const int64_t unit = 1024;
    if (size < unit) return std::to_string(size) + " B";
    int64_t div = unit;
    int exp = 0;
    for (int64_t n = size / unit; n >= unit; n /= unit) {
        div *= unit;
        ++exp;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %ciB",
                  static_cast<double>(size) / static_cast<double>(div), "KMGTPE"[exp]);
    return buf;
}

std::string format_duration(int sec) {
    if (sec < 0) sec = 0;
    if (sec < 60) return std::to_string(sec) + "s";
    if (sec < 3600) return std::to_string(sec / 60) + "m " + std::to_string(sec % 60) + "s";
    if (sec < 86400) return std::to_string(sec / 3600) + "h " + std::to_string((sec % 3600) / 60) + "m";
    return std::to_string(sec / 86400) + "d " + std::to_string((sec % 86400) / 3600) + "h";
}

static std::string pad_zero(int v) {
    if (v < 10) return "0" + std::to_string(v);
    return std::to_string(v);
}

std::string format_time(int sec) {
    if (sec < 0) sec = 0;
    int h = sec / 3600;
    int m = (sec % 3600) / 60;
    int s = sec % 60;
    if (h > 0) return std::to_string(h) + ":" + pad_zero(m) + ":" + pad_zero(s);
    return pad_zero(m) + ":" + pad_zero(s);
}

std::string format_uptime(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    int64_t days = seconds / 86400;
    seconds %= 86400;
    int64_t hours = seconds / 3600;
    seconds %= 3600;
    int64_t minutes = seconds / 60;
    int64_t secs = seconds % 60;
    std::string r;
    if (days > 0) r += std::to_string(days) + "d ";
    if (hours > 0) r += std::to_string(hours) + "h ";
    if (minutes > 0) r += std::to_string(minutes) + "m ";
    r += std::to_string(secs) + "s";
    return r;
}

std::string get_progress_bar(int played_sec, int duration_sec) {
    static const char* bars[] = {
        "◉———————", "—◉——————", "——◉—————", "———◉————", "————◉———",
        "—————◉——", "——————◉—", "———————◉", "————————◉"};
    if (duration_sec <= 0 || played_sec <= 0) return bars[0];
    if (played_sec >= duration_sec) return bars[8];
    int index = (played_sec * 9) / duration_sec;
    if (index > 8) index = 8;
    return bars[index];
}

std::string short_title(std::string title, int max) {
    if (static_cast<int>(title.size()) <= max) return title;
    return title.substr(0, static_cast<size_t>(max)) + "…";
}

std::string escape_html(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c; break;
        }
    }
    return o;
}

std::string html_mention(const std::string& name, int64_t id) {
    std::string n = trim(name);
    if (n.empty()) n = "user";
    return "<a href=\"tg://user?id=" + std::to_string(id) + "\">" + escape_html(n) + "</a>";
}

std::string mention(const std::string& name, int64_t id) { return html_mention(name, id); }

// Ports Aishamusic-2 core.Stylish: smallcaps font for ASCII letters, leaving
// HTML tags (<...>) and entities (&...;) untouched.
std::string stylish(const std::string& in) {
    if (in.empty()) return in;
    static const std::unordered_map<uint32_t, std::string> kMap = [] {
        const std::string lower[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                     "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
        const char* mapped[] = {"ᴀ", "ʙ", "ᴄ", "ᴅ", "є", "ғ", "ɢ", "ʜ", "ɪ", "ᴊ", "ᴋ", "ʟ", "ϻ",
                                "η", "σ", "ᴘ", "ǫ", "ʀ", "s", "ᴛ", "υ", "ᴠ", "ᴡ", "x", "ʏ", "ᴢ"};
        std::unordered_map<uint32_t, std::string> m;
        for (int i = 0; i < 26; i++) {
            m[static_cast<uint32_t>('A' + i)] = mapped[i];
            m[static_cast<uint32_t>('a' + i)] = mapped[i];
        }
        return m;
    }();

    // Decode UTF-8 into code points (cheap: we only re-encode what we replace).
    std::string out;
    out.reserve(in.size() + in.size() / 2);
    auto it = in.begin();
    auto last = in.end();
    while (it != last) {
        unsigned char c = static_cast<unsigned char>(*it);
        if (c == '<') {
            // Copy the tag verbatim.
            auto j = it;
            while (j != last && *j != '>') ++j;
            if (j == last) {
                out.append(it, last);
                break;
            }
            out.append(it, j + 1);
            it = j + 1;
            continue;
        }
        if (c == '&') {
            auto j = it;
            while (j != last && *j != ';') ++j;
            if (j == last) {
                out.push_back(*it++);
                continue;
            }
            out.append(it, j + 1);
            it = j + 1;
            continue;
        }
        if (c < 0x80) {
            auto found = kMap.find(c);
            if (found != kMap.end()) {
                out += found->second;
            } else {
                out.push_back(static_cast<char>(c));
            }
            ++it;
            continue;
        }
        // Multi-byte sequence: pass through.
        out.push_back(*it++);
        if (c >= 0xC0 && c < 0xE0) {
            if (it != last) out.push_back(*it++);
        } else if (c >= 0xE0 && c < 0xF0) {
            for (int k = 0; k < 2 && it != last; k++) out.push_back(*it++);
        } else if (c >= 0xF0 && c < 0xF8) {
            for (int k = 0; k < 3 && it != last; k++) out.push_back(*it++);
        }
    }
    return out;
}

namespace {
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}  // namespace

std::string base64_encode(const std::string& in) {
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(kB64[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(kB64[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back(0x3D);
    return out;
}

std::string base64_decode(const std::string& in) {
    std::vector<int> rev(256, -1);
    for (int i = 0; i < 64; ++i) rev[(unsigned char)kB64[i]] = i;
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == 0x3D) break;
        if (rev[c] == -1) continue;
        val = (val << 6) + rev[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

}  // namespace advait::utils

