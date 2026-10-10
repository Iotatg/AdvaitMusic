#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace advait {

struct Config {
    int32_t api_id{0};
    std::string api_hash;
    std::string token;
    std::string mongo_uri;
    std::vector<std::string> string_sessions;

    std::string db_name{"MeowMusic"};
    std::string session_type{"pyrogram"};
    int64_t logger_id{0};
    int64_t owner_id{0};
    std::string owner_username;
    std::string dev_url;
    bool disable_colour{false};

    std::string meow_api_url;
    std::string meow_api_key;
    std::string yuki_api_url{"https://music.yukiapi.site"};
    std::string yuki_api_key;
    std::string iota_api_url{"http://127.0.0.1:8000"};
    std::string iota_api_key;

    std::string default_lang{"en"};
    int duration_limit{4200};
    bool leave_on_demoted{false};
    int queue_limit{24};

    std::string support_chat{"https://t.me/+Q493A_VBAUpkMmRl"};
    std::string support_channel{"https://t.me/im_iota"};
    std::string cookies_link;
    bool set_cmds{false};
    int max_auth_users{25};

    std::vector<std::string> start_images;
    std::string ping_image{
        "https://telegra.ph/file/91533956c91d0fd7c9f20.jpg"};
    std::string repo_image{"https://yukiapi.site/file/6RldzdNt"};
    std::string port{"8000"};

    std::chrono::steady_clock::time_point start_time{std::chrono::steady_clock::now()};

    static Config& get();
    std::string load();
    std::string start_image() const;
};

}  // namespace advait
