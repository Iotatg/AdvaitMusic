#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace advait::utils {

std::string format_bytes(int64_t size);
std::string format_duration(int sec);
std::string format_time(int sec);
std::string format_uptime(int64_t seconds);
std::string get_progress_bar(int played_sec, int duration_sec);
std::string short_title(std::string title, int max = 25);
std::string escape_html(const std::string& s);
std::string mention(const std::string& name, int64_t id);
std::vector<std::string> split_fields(const std::string& v);
bool parse_bool(const std::string& v, bool def);
int parse_int(const std::string& v, int def);
int64_t parse_int64(const std::string& v, int64_t def);
std::string getenv_str(const char* key, const std::string& def = {});
std::string trim(std::string s);
std::string to_lower(std::string s);
std::string html_mention(const std::string& name, int64_t id);
std::string base64_encode(const std::string& in);
std::string base64_decode(const std::string& in);

}  // namespace advait::utils
