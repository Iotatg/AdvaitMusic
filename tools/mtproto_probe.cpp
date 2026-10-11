// Phase-1 proof-of-life for the C++ MTProto assistant transport:
// parses a string session, connects to the DC over abridged MTProto and
// performs an authenticated mtproto.ping round-trip. Prints only non-secret
// results (pong id / DC / user id), never key material.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include <cstring>

#include "advait/core/config.hpp"
#include "advait/mtproto/client.hpp"
#include "advait/mtproto/tl.hpp"
#ifdef ADVAIT_WITH_NTGCALLS
#include "advait/ntgcalls/client.hpp"
#endif

namespace {

std::string build_ping(int64_t& ping_id) {
    using namespace advait::mtproto;
    std::random_device rd;
    ping_id = (static_cast<int64_t>(rd()) << 32) | rd();
    TlWriter w;
    w.u32(0x7abe77ecu);
    w.i64(ping_id);
    return w.out();
}

}  // namespace

int main(int argc, char** argv) {
    using namespace advait::mtproto;
    const char* mode = argc > 1 ? argv[1] : "config";
    long long chat_id = argc > 2 ? std::atoll(argv[2]) : 0;
    auto& cfg = advait::Config::get();
    std::string err = cfg.load();
    if (cfg.string_sessions.empty()) {
        std::printf("NO_SESSIONS err=%s\n", err.c_str());
        return 1;
    }
    Session ses;
    try {
        ses = Client::parse_session_string(cfg.string_sessions[0]);
    } catch (const std::exception& e) {
        std::printf("SESSION_PARSE_FAIL %s\n", e.what());
        return 2;
    }
    std::printf("SESSION_OK dc=%d api=%d user=%lld\n", ses.dc_id, ses.api_id,
                static_cast<long long>(ses.user_id));

    Client client(ses, {});
    if (!client.connect(&err)) {
        std::printf("CONNECT_FAIL %s\n", err.c_str());
        return 3;
    }
    std::printf("CONNECT_OK\n");

    if (std::strcmp(mode, "config") == 0) {
        // help.getConfig#00c4f9186b (unauthenticated, proves full RPC round-trip)
        advait::mtproto::TlWriter gc;
        gc.u32(0xc4f9186bu);
        std::string resp = client.call_rpc(gc.out(), &err);
        if (resp.empty()) {
            std::printf("PING_FAIL %s\n", err.c_str());
            return 4;
        }
        TlReader r(resp);
        uint32_t crc = r.u32();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%08x", crc);
        if (crc == 0xcc1a241eu || crc == 0x6d13fe3cu || crc == 0xc4180bd4u) {
            std::printf("RPC_OK config crc=%s len=%zu\n", buf, resp.size());
            return 0;
        }
        std::printf("PING_UNEXPECTED crc=%s len=%zu\n", buf, resp.size());
        return 5;
    }

    if (std::strcmp(mode, "ah") == 0) {
        int64_t ah = 0;
        if (!client.self_access_hash(&ah, &err)) {
            std::printf("SELF_AH_FAIL %s\n", err.c_str());
            return 6;
        }
        std::printf("SELF_AH %lld\n", static_cast<long long>(ah));
        return 0;
    }

    Client::GroupCallRef call;
    if (std::strcmp(mode, "call") == 0 || std::strcmp(mode, "join") == 0) {
        if (chat_id == 0) {
            std::printf("USAGE: mtproto_probe call|join <chat_id>\n");
            return 1;
        }
        if (!client.get_group_call(chat_id, &call, &err)) {
            std::printf("GET_CALL_FAIL %s\n", err.c_str());
            return 7;
        }
        std::printf("CALL id=%lld ah=%lld\n", static_cast<long long>(call.id),
                    static_cast<long long>(call.access_hash));
        if (std::strcmp(mode, "call") == 0) {
            return 0;
        }
#ifdef ADVAIT_WITH_NTGCALLS
        advait::ntg::Client ntg;
        std::string offer;
        if (!ntg.create(chat_id, &offer)) {
            std::printf("NTG_CREATE_FAIL\n");
            return 8;
        }
        std::printf("OFFER len=%zu\n", offer.size());
        std::string srv;
        if (!client.join_group_call(call, offer, &srv, &err)) {
            std::printf("JOIN_FAIL %s\n", err.c_str());
            return 9;
        }
        std::printf("JOIN_OK server_params_len=%zu\n", srv.size());
        if (!ntg.connect(chat_id, srv)) {
            std::printf("NTG_CONNECT_FAIL\n");
            return 10;
        }
        std::printf("NTG_CONNECT_OK\n");
        return 0;
#else
        std::printf("NO_NTGCALLS_BUILD\n");
        return 8;
#endif
    }
    std::printf("UNKNOWN mode %s\n", mode);
    return 1;
}
