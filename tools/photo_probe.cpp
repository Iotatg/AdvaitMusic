// One-off probe: find which inputMessagePhoto shape TDLib 1.8.66 accepts.
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <unistd.h>

using json = nlohmann::json;

using TdCreate = int (*)();
using TdSend = void (*)(int, const char*);
using TdReceive = const char* (*)(double);
using TdExecute = const char* (*)(const char*);

static TdSend g_send;
static TdReceive g_receive;
static int g_extra = 1;
static int g_client = 0;

static void sendq(const json& q) {
    json qq = q;
    qq["@extra"] = g_extra++;
    g_send(g_client, qq.dump().c_str());
}

int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    // argv: chat_id, photo_path
    (void)argc;
    const char* lib = "./lib/libtdjson.so";
    void* h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        std::printf("dlopen fail %s\n", dlerror());
        return 1;
    }
    auto create = (TdCreate)dlsym(h, "td_create_client_id");
    g_send = (TdSend)dlsym(h, "td_send");
    g_receive = (TdReceive)dlsym(h, "td_receive");
    auto destroy = (void (*)(int))nullptr; (void)destroy;

    // read env from KanhaMusic/.env
    FILE* f = fopen("/workspace/KanhaMusic/.env", "rb");
    if (!f) return 2;
    std::string api_id, api_hash, token, owner;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        std::string k(line, eq - line);
        std::string v(eq + 1);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        if (k.rfind("API_ID", 0) == 0) api_id = v;
        else if (k.rfind("API_HASH", 0) == 0) api_hash = v;
        else if (k.rfind("BOT_TOKEN", 0) == 0 || k.rfind("TOKEN", 0) == 0) token = v;
        else if (k == "OWNER_ID") owner = v;
    }
    fclose(f);
    if (token.empty() || api_id.empty()) {
        std::printf("config missing\n");
        return 2;
    }
    int64_t chat = argc > 1 ? atoll(argv[1])
                                       : (!owner.empty() ? atoll(owner.c_str()) : -1003915522165LL);
    std::printf("TARGET chat=%lld owner_len=%zu\n", (long long)chat, owner.size());
    std::string path = argc > 2 ? argv[2] : "/workspace/AdvaitMusic-cpp/cache/art/b868089a19e0edad.jpg";

    g_client = create();
    g_send(g_client, R"({"@type":"setLogVerbosityLevel","new_verbosity_level":0})");
    sendq({{"@type", "getOption"}, {"key", "version"}});
    bool ready = false;
    auto t0 = time(nullptr);
    while (!ready && time(nullptr) - t0 < 30) {
        const char* r = g_receive(1.0);
        if (!r) continue;
        json j = json::parse(r, nullptr, false);
        if (j.is_discarded()) continue;
        std::string t;
        try { t = j.value("@type", ""); } catch (...) { continue; }
        if (t == "updateAuthorizationState") {
            auto st = j["authorization_state"].value("@type", "");
            std::printf("AUTH %s\n", st.c_str());
            if (st == "authorizationStateWaitTdlibParameters") {
                sendq({{"@type", "setTdlibParameters"},
                       {"database_directory", argc > 3 ? argv[3] : "/tmp/opencode/tdtest"},
                       {"use_message_database", true},
                       {"use_secret_chats", false},
                       {"api_id", std::stoi(api_id)},
                       {"api_hash", api_hash},
                       {"system_language_code", "en"},
                       {"device_model", "photoprobe"},
                       {"application_version", "1.0"}});
            } else if (st == "authorizationStateWaitPhoneNumber") {
                sendq({{"@type", "checkAuthenticationBotToken"}, {"token", token}});
            } else if (st == "authorizationStateReady") {
                ready = true;
            }
        } else if (t == "option") {
            std::printf("OPTION %s\n", j.dump().substr(0, 120).c_str());
        } else if (t == "error") {
            std::printf("ERR %s\n", j.dump().substr(0, 300).c_str());
        }
    }
    if (!ready) {
        std::printf("not ready\n");
        return 3;
    }

    if (chat > 0) {
        // fresh bot-mode instance: create DM chat before sending
        json cq = {{"@type", "createChat"},
                   {"participant_ids", json::array({chat})},
                   {"title", "photoprobe"}};
        int cex = g_extra;
        cq["@extra"] = cex;
        g_send(g_client, cq.dump().c_str());
        auto c0 = time(nullptr);
        while (time(nullptr) - c0 < 15) {
            const char* r = g_receive(1.0);
            if (!r) continue;
            json j = json::parse(r, nullptr, false);
            if (j.is_discarded()) continue;
            if (j.value("@extra", -1) == cex) {
                std::printf("createChat -> %s\n", j.dump().substr(0, 400).c_str());
                if (j.value("@type", "") == "chat") chat = j["id"].get<int64_t>();
                break;
            }
        }
    }

    struct Variant {
        const char* label;
        json photo;
    };
    std::vector<Variant> vs = {
        {"A absolute inputFileLocal", {{"@type", "inputFileLocal"}, {"local_path", path}}},
        {"B remote URL", {{"@type", "inputFileRemote"}, {"id", "https://i.ytimg.com/vi/dQw4w9WgXcQ/hq720.jpg"}}},
    };
    for (auto& v : vs) {
        json q = {
            {"@type", "sendMessage"},
            {"chat_id", chat},
            {"input_message_content",
             {{"@type", "inputMessagePhoto"},
              {"photo", v.photo},
              {"caption", {{"@type", "formattedText"}, {"text", std::string("photoprobe ") + v.label}}}}},
        };
        int ex = g_extra;
        q["@extra"] = ex;
        std::printf("%s label sent\n", v.label);
        g_send(g_client, q.dump().c_str());
        auto s0 = time(nullptr);
        while (time(nullptr) - s0 < 15) {
            const char* r = g_receive(1.0);
            if (!r) continue;
            json j = json::parse(r, nullptr, false);
            if (j.is_discarded()) continue;
            if (j.value("@extra", -1) == ex || j.value("@type", "") == "updateNewMessage") {
                std::printf("recv %s\n", j.dump().substr(0, 250).c_str());
            if (j.value("@extra", -1) == ex) {
                    // clean up if it sent
                    if (j.value("@type", "") == "messageId") {
                        sendq({{"@type", "deleteMessages"}, {"chat_id", chat},
                               {"message_ids", json::array({j["value"].get<int64_t>()})}, {"revoke", true}});
                        // drain a moment
                    }
                    break;
                }
            }
        }
    }
    (void)destroy;
    _exit(0);
}
