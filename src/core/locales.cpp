#include "advait/core/locales.hpp"
#include "advait/core/config.hpp"
#include "advait/core/logger.hpp"
#include "advait/database/database.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace advait {

namespace {

std::string expand_emojis_impl(std::string s) {
    static const std::unordered_map<std::string, std::string> tokens = {
        {"{emoji:🎵}", "5359529383319084413"},
        {"{emoji:👍}", "5368324170671202286"},
    };
    static const std::regex re(R"(\{emoji:([^}]+)\})");
    std::string out;
    std::sregex_iterator it(s.begin(), s.end(), re), end;
    size_t last = 0;
    for (; it != end; ++it) {
        out.append(s, last, static_cast<size_t>(it->position()) - last);
        auto match = it->str();
        auto raw = (*it)[1].str();
        auto f = tokens.find(match);
        if (f != tokens.end() && !f->second.empty()) {
            out += "<tg-emoji emoji-id=\"" + f->second + "\">" + raw + "</tg-emoji>";
        } else {
            out += raw;
        }
        last = static_cast<size_t>(it->position() + it->length());
    }
    out.append(s, last, std::string::npos);
    return out;
}

std::unordered_map<std::string, std::string> parse_yaml_flat(const std::string& text) {
    std::unordered_map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    std::string key;
    std::string val;
    bool in_block = false;
    auto flush = [&]() {
        if (key.empty()) return;
        while (!val.empty() && (val.back() == '\n' || val.back() == ' ')) val.pop_back();
        out[key] = val;
        key.clear();
        val.clear();
        in_block = false;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (in_block) {
            if (line.empty()) {
                val += "\n";
                continue;
            }
            if (line[0] == ' ' || line[0] == '\t') {
                auto start = line.find_first_not_of(" \t");
                if (start == std::string::npos) {
                    val += "\n";
                } else {
                    if (!val.empty()) val += "\n";
                    val += line.substr(start);
                }
                continue;
            }
            flush();
        }
        auto t = line;
        auto hash = t.find('#');
        std::string raw = t;
        if (t.empty()) continue;
        auto first = t.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (t[first] == '#') continue;
        auto colon = t.find(':');
        if (colon == std::string::npos) continue;
        auto k = t.substr(0, colon);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        auto rest = t.substr(colon + 1);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.erase(rest.begin());
        if (rest == "|" || rest == ">") {
            key = k;
            val.clear();
            in_block = true;
            continue;
        }
        if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
            rest = rest.substr(1, rest.size() - 2);
            std::string un;
            un.reserve(rest.size());
            for (size_t i = 0; i < rest.size(); ++i) {
                if (rest[i] == '\\' && i + 1 < rest.size()) {
                    char n = rest[++i];
                    switch (n) {
                        case 'n': un += '\n'; break;
                        case 't': un += '\t'; break;
                        case 'r': un += '\r'; break;
                        case '"': un += '"'; break;
                        case '\\': un += '\\'; break;
                        default: un += '\\'; un += n; break;
                    }
                } else {
                    un += rest[i];
                }
            }
            rest = un;
        } else if (!rest.empty() && rest.front() == '#') {
            rest.clear();
        } else {
            auto cmt = rest.find(" #");
            if (cmt != std::string::npos) rest = rest.substr(0, cmt);
        }
        out[k] = rest;
    }
    flush();
    return out;
}

}  // namespace

Locales& Locales::instance() {
    static Locales l;
    return l;
}

std::string Locales::load(const std::string& dir) {
    namespace fs = std::filesystem;
    if (!fs::exists(dir)) return "locales dir not found: " + dir;
    for (auto& p : fs::directory_iterator(dir)) {
        if (!p.is_regular_file()) continue;
        auto path = p.path();
        if (path.extension() != ".yml" && path.extension() != ".yaml") continue;
        std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        auto lang = path.stem().string();
        data_[lang] = parse_yaml_flat(ss.str());
        ADVAIT_LOG_INFO("locales", "loaded " + lang + " (" + std::to_string(data_[lang].size()) + " keys)");
    }
    auto def = Config::get().default_lang;
    if (data_.find(def) == data_.end()) return "default language " + def + " not found";
    return {};
}

std::string Locales::get(const std::string& lang, const std::string& key, const LocaleArg& args) const {
    std::vector<std::string> try_langs{lang, Config::get().default_lang, "en"};
    std::string val;
    bool found = false;
    for (auto& l : try_langs) {
        auto it = data_.find(l);
        if (it == data_.end()) continue;
        auto k = it->second.find(key);
        if (k != it->second.end()) {
            val = k->second;
            found = true;
            break;
        }
    }
    if (!found) {
        ADVAIT_LOG_ERROR("locales", "Missing translation lang=" + lang + " key=" + key);
        return "[" + key + "]";
    }
    for (auto& [k, v] : args) {
        auto needle = "{" + k + "}";
        for (;;) {
            auto p = val.find(needle);
            if (p == std::string::npos) break;
            val.replace(p, needle.size(), v);
        }
    }
    return expand_emojis_impl(val);
}

std::vector<std::string> Locales::available() const {
    std::vector<std::string> out;
    for (auto& [k, _] : data_) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

std::string F(int64_t chat_id, const std::string& key, const LocaleArg& args) {
    auto lang = db::language(chat_id);
    if (lang.empty()) lang = Config::get().default_lang;
    return Locales::instance().get(lang, key, args);
}

std::string expand_emojis(std::string s) { return expand_emojis_impl(std::move(s)); }

}  // namespace advait
