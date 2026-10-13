// Diagnostic probe: answers which TDLib send variant works for reposted stickers.
// Fetches a real sticker from a chat's history, dumps its raw content JSON,
// then tries send variants in order and prints each response.
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

typedef int (*TdCreate)();
typedef void (*TdSend)(int, const char*);
typedef const char* (*TdReceive)(double);
typedef const char* (*TdExecute)(const char*);

static TdCreate create_;
static TdSend send_;
static TdReceive receive_;
static TdExecute execute_;

static std::mutex g_mu;
static std::map<std::string, json> g_pend;
static std::atomic<int> g_client{0};

static std::string env_get(const std::string& file, const std::string& key) {
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto k = line.substr(0, eq);
        if (k != key) continue;
        auto v = line.substr(eq + 1);
        while (!v.empty() && (v.front() == '"' || v.front() == '\'')) v.erase(v.begin());
        while (!v.empty() && (v.back() == '"' || v.back() == '\'')) v.pop_back();
        return v;
    }
    return {};
}

static void pump(double seconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const char* r = receive_(0.5);
        if (!r) continue;
        try {
            auto j = json::parse(r);
            if (j.contains("@extra") && j["@extra"].is_string()) {
                std::lock_guard<std::mutex> g(g_mu);
                g_pend[j["@extra"].get<std::string>()] = j;
            }
        } catch (...) {
        }
    }
}

static json ask(const json& q, double timeout_s = 15.0) {
    static std::atomic<long long> seq{5000};
    auto extra = std::to_string(++seq);
    json rq = q;
    rq["@extra"] = extra;
    send_(g_client.load(), rq.dump().c_str());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> g(g_mu);
            auto it = g_pend.find(extra);
            if (it != g_pend.end()) {
                auto out = it->second;
                g_pend.erase(it);
                return out;
            }
        }
        const char* r = receive_(0.2);
        if (!r) continue;
        try {
            auto j = json::parse(r);
            if (j.contains("@extra") && j["@extra"].is_string()) {
                std::lock_guard<std::mutex> g(g_mu);
                g_pend[j["@extra"].get<std::string>()] = j;
            }
        } catch (...) {
        }
    }
    return json{{"@type", "probe-timeout"}};
}


int main(int argc, char** argv) {
    int64_t chat_id = (argc > 1) ? std::atoll(argv[1]) : 6998484205;

    void* h = dlopen("./lib/libtdjson.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("./build/libtdjson.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("/workspace/AdvaitMusic-cpp/build/libtdjson.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        std::cerr << "tdjson load failed: " << dlerror() << "\n";
        return 1;
    }
    create_ = reinterpret_cast<TdCreate>(dlsym(h, "td_create_client_id"));
    send_ = reinterpret_cast<TdSend>(dlsym(h, "td_send"));
    receive_ = reinterpret_cast<TdReceive>(dlsym(h, "td_receive"));
    execute_ = reinterpret_cast<TdExecute>(dlsym(h, "td_execute"));

    auto api_id = std::atoi(env_get("../KanhaMusic/.env", "API_ID").c_str());
    auto api_hash = env_get("../KanhaMusic/.env", "API_HASH");
    auto token = env_get("../KanhaMusic/.env", "TOKEN");
    if (api_id == 0 || api_hash.empty() || token.empty()) {
        std::cerr << "missing creds in ../KanhaMusic/.env (API_ID/API_HASH/TOKEN)\n";
        return 2;
    }

    g_client = create_();
    json params = {{"@type", "setTdlibParameters"},
                   {"database_directory", "/tmp/opencode/sticker-probe-db"},
                   {"use_file_database", true},
                   {"use_chat_info_database", true},
                   {"use_message_database", true},
                   {"use_secret_chats", false},
                   {"api_id", api_id},
                   {"api_hash", api_hash},
                   {"system_language_code", "en"},
                   {"device_model", "probe"},
                   {"application_version", "1.0"}};
    ask(params, 5.0);
    ask({{"@type", "setLogVerbosityLevel"}, {"new_verbosity_level", 0}}, 5.0);
    ask({{"@type", "setAuthenticationBotParameters"}, {"token", token}}, 5.0);

    bool ready = false;
    for (int i = 0; i < 200 && !ready; ++i) {
        auto r = json::parse(std::string(execute_(R"({"@type":"getAuthorizationState"})")));
        auto t = r.value("@type", "");
        if (t == "authorizationStateReady") ready = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cout << "authorized=" << (ready ? "y" : "n") << "\n";
    if (!ready) return 3;

    auto chat_created = ask({{"@type", "createPrivateChat"}, {"user_id", chat_id}, {"force", true}}, 10.0);
    std::cout << "createPrivateChat: " << chat_created.value("@type", "?") << "\n";

    auto hist = ask({{"@type", "getChatHistory"},
                     {"chat_id", chat_id},
                     {"from_message_id", 0},
                     {"offset", 0},
                     {"limit", 20},
                     {"only_local", false}},
                    20.0);
    std::string remote_id, local_id, raw_content;
    for (auto& mid : hist.value("messages", json::array())) {
        json c = mid.value("content", json::object());
        if (c.value("@type", "") != "messageSticker") continue;
        raw_content = c.dump();
        json s = c.value("sticker", json::object());
        if (s.contains("sticker") && s["sticker"].contains("remote"))
            remote_id = s["sticker"]["remote"].value("id", "");
        if (s.contains("sticker") && s["sticker"].is_number())
            local_id = std::to_string(s["sticker"].get<int64_t>());
        break;
    }
    std::cout << "remote_id=" << remote_id << "\n";
    std::cout << "local_id=" << local_id << "\n";
    std::cout << "raw_content=" << raw_content.substr(0, 1500) << "\n";
    if (remote_id.empty()) {
        std::cout << "NO STICKER FOUND IN HISTORY\n";
        return 4;
    }

    struct Variant {
        const char* name;
        json sticker;
        bool wHE;
    };
    std::vector<Variant> variants = {
        {"A inputFileRemote+WHE", json{{"@type", "inputFileRemote"}, {"id", remote_id}}, true},
        {"B inputFileId+WHE", json{{"@type", "inputFileId"}, {"id", std::atoll(local_id.c_str())}}, true},
        {"C inputFileRemote bare", json{{"@type", "inputFileRemote"}, {"id", remote_id}}, false},
    };
    for (auto& v : variants) {
        json content = json{{"@type", "inputMessageSticker"}, {"sticker", v.sticker}};
        if (v.wHE) {
            content["width"] = 512;
            content["height"] = 512;
            content["emoji"] = "";
        }
        auto q = json{{"@type", "sendMessage"}, {"chat_id", chat_id}, {"input_message_content", content}};
        auto r = ask(q, 20.0);
        std::cout << "variant " << v.name << " -> " << r.dump().substr(0, 400) << "\n";
        std::cout.flush();
    }
    std::cout << "PROBE DONE\n";
    return 0;
}
