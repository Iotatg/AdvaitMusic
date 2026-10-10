#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace advait {

using LocaleArg = std::map<std::string, std::string>;

class Locales {
public:
    static Locales& instance();
    std::string load(const std::string& dir);
    std::string get(const std::string& lang, const std::string& key, const LocaleArg& args = {}) const;
    std::vector<std::string> available() const;

private:
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> data_;
};

std::string F(int64_t chat_id, const std::string& key, const LocaleArg& args = {});
std::string expand_emojis(std::string s);

}  // namespace advait
