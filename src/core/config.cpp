#include "advait/core/config.hpp"
#include "advait/core/utils.hpp"

#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

namespace advait {

static void load_dotenv(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = utils::trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto k = utils::trim(line.substr(0, eq));
        auto v = utils::trim(line.substr(eq + 1));
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
            v = v.substr(1, v.size() - 2);
        }
        if (!k.empty() && std::getenv(k.c_str()) == nullptr) setenv(k.c_str(), v.c_str(), 0);
    }
}

Config& Config::get() {
    static Config c;
    return c;
}

std::string Config::load() {
    load_dotenv(".env");
    load_dotenv("../KanhaMusic/.env");

    api_id = static_cast<int32_t>(utils::parse_int64(utils::getenv_str("API_ID"), 0));
    api_hash = utils::getenv_str("API_HASH");
    token = utils::getenv_str("TOKEN");
    if (token.empty()) token = utils::getenv_str("BOT_TOKEN");
    mongo_uri = utils::getenv_str("MONGO_DB_URI");
    string_sessions = utils::split_fields(utils::getenv_str("STRING_SESSIONS"));
    if (string_sessions.empty()) string_sessions = utils::split_fields(utils::getenv_str("STRING_SESSION"));

    db_name = utils::getenv_str("DB_NAME", "MeowMusic");
    session_type = utils::getenv_str("SESSION_TYPE", "pyrogram");
    logger_id = utils::parse_int64(utils::getenv_str("LOGGER_ID"), 0);
    if (logger_id == 0) logger_id = utils::parse_int64(utils::getenv_str("LOG_GROUP_ID"), 0);
    owner_id = utils::parse_int64(utils::getenv_str("OWNER_ID"), 0);
    owner_username = utils::getenv_str("OWNER_USERNAME");
    dev_url = utils::getenv_str("DEV_URL", utils::getenv_str("DEVELOPER_URL"));
    disable_colour = utils::parse_bool(utils::getenv_str("DISABLE_COLOUR"), false);

    meow_api_url = utils::getenv_str("MEOW_API_URL");
    meow_api_key = utils::getenv_str("MEOW_API_KEY");
    yuki_api_url = utils::getenv_str("YUKI_API_URL", utils::getenv_str("MEOW_API_URL", "https://music.yukiapi.site"));
    yuki_api_key = utils::getenv_str("YUKI_API_KEY", utils::getenv_str("MEOW_API_KEY"));
    iota_api_url = utils::getenv_str("IOTA_API_URL", "http://127.0.0.1:8000");
    iota_api_key = utils::getenv_str("IOTA_API_KEY");

    default_lang = utils::getenv_str("DEFAULT_LANG", "en");
    duration_limit = utils::parse_int(utils::getenv_str("DURATION_LIMIT"), 4200);
    leave_on_demoted = utils::parse_bool(utils::getenv_str("LEAVE_ON_DEMOTED"), false);
    queue_limit = utils::parse_int(utils::getenv_str("QUEUE_LIMIT"), 24);

    support_chat = utils::getenv_str("SUPPORT_CHAT", "https://t.me/+Q493A_VBAUpkMmRl");
    support_channel = utils::getenv_str("SUPPORT_CHANNEL", "https://t.me/im_iota");
    cookies_link = utils::getenv_str("COOKIES_LINK");
    set_cmds = utils::parse_bool(utils::getenv_str("SET_CMDS"), false);
    max_auth_users = utils::parse_int(utils::getenv_str("MAX_AUTH_USERS"), 25);

    start_images = utils::split_fields(utils::getenv_str("START_IMAGES"));
    if (start_images.empty()) {
        auto img = utils::getenv_str("START_IMG_URL");
        if (!img.empty()) start_images.push_back(img);
    }
    ping_image = utils::getenv_str("PING_IMG_URL", ping_image);
    repo_image = utils::getenv_str("REPO_IMG_URL", repo_image);
    port = utils::getenv_str("PORT", "8000");
    start_time = std::chrono::steady_clock::now();

    if (api_id == 0) return "API_ID is required";
    if (api_hash.empty()) return "API_HASH is required";
    if (token.empty()) return "TOKEN (or BOT_TOKEN) is required";
    if (mongo_uri.empty()) return "MONGO_DB_URI is required";
    if (string_sessions.empty()) return "STRING_SESSIONS (or STRING_SESSION) is required";
    return {};
}

std::string Config::start_image() const {
    if (start_images.empty()) return {};
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<size_t> d(0, start_images.size() - 1);
    return start_images[d(rng)];
}

}  // namespace advait
