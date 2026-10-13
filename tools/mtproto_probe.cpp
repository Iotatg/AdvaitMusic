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
    int64_t ah = argc > 3 ? std::atoll(argv[3]) : 0;

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
    if (std::getenv("MITM")) {
        ses.ip = "127.0.0.1";
        ses.port = 8443;
    }
    std::printf("SESSION_OK dc=%d api=%d user=%lld\n", ses.dc_id, ses.api_id,
                static_cast<long long>(ses.user_id)); { auto ak = ses.auth_key; char b[32]; for (int i=0;i<8;i++) std::snprintf(b+i*2, 3, "%02x", (unsigned char)ak[12+i]); std::printf("KEYID %s\n", b); }
    std::printf("IP %s port=%d\n", ses.ip.c_str(), ses.port);

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

    if (std::strcmp(mode, "probe1") == 0) {
        if (chat_id == 0) {
            std::printf("USAGE: mtproto_probe probe1 <chat_id>\n");
            return 1;
        }
        // getChannels (ah=0) then parse channel id/access_hash manually
        TlWriter gc;
        gc.u32(0xa7f6bbbu);
        gc.vec_header(1);
        gc.u32(0xf35aec28u);
        gc.i64(-chat_id - 1000000000000LL);
        gc.i64(0);
        std::string r0 = client.call_rpc(gc.out(), &err);
        if (r0.empty()) {
            std::printf("GETCHANNELS_FAIL %s\n", err.c_str());
            return 5;
        }
        size_t p = r0.find(std::string("\xd4\x9f\x34\xc6", 4));
        if (p == std::string::npos) {
            std::printf("CHANNEL_OBJ_NOT_FOUND len=%zu\n", r0.size());
            std::string hex;
            for (size_t i = 0; i < r0.size(); i++) {
                char hb[3];
                std::snprintf(hb, sizeof(hb), "%02x", static_cast<unsigned char>(r0[i]));
                hex += hb;
                if (hex.size() > 220) break;
            }
            std::printf("HEX %s\n", hex.c_str());
            return 5;
        }
        TlReader rr(r0, p + 4);
        uint32_t f1 = rr.u32();
        rr.u32();
        int64_t cid = rr.i64();
        int64_t cah = (f1 & (1u << 13)) ? rr.i64() : 0;
        std::printf("PARSED cid=%lld cah=%lld\n", static_cast<long long>(cid),
                    static_cast<long long>(cah));
        // raw getFullChannel with the parsed pair
        TlWriter fc;
        fc.u32(0x8736a09u);
        fc.u32(0xf35aec28u);
        fc.i64(cid);
        fc.i64(cah);
        std::string r1 = client.call_rpc(fc.out(), &err, true);
        if (r1.empty()) {
            std::printf("GETFULL_FAIL %s\n", err.c_str());
        } else {
            TlReader rr2(r1);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%08x", rr2.u32());
            std::printf("GETFULL_OK crc=%s len=%zu\n", buf, r1.size());
        }
        // control: users.getUsers(inputUserSelf)
        TlWriter gu;
        gu.u32(0xd91a548u);
        gu.vec_header(1);
        gu.u32(0xf7c1b13fu);
        std::string r2 = client.call_rpc(gu.out(), &err);
        std::printf("CONTROL_USERS %s\n", r2.empty() ? err.c_str() : ("ok len=" + std::to_string(r2.size())).c_str());
        return 0;
    }

    if (std::strcmp(mode, "silence_after") == 0) {
        if (chat_id == 0) return 1;
        // re-use the same get_group_call path the bot uses (expected timeout)
        Client::GroupCallRef tmpcall;
        std::string terr;
        bool gok = client.get_group_call(chat_id, &tmpcall, &terr);
        std::printf("GET_CALL %s\n", gok ? "OK" : terr.c_str());
        // control RPC on the same socket afterwards
        TlWriter gu;
        gu.u32(0xd91a548u);
        gu.vec_header(1);
        gu.u32(0xf7c1b13fu);
        std::string r = client.call_rpc(gu.out(), &err);
        std::printf("CONTROL_USERS_AFTER %s\n",
                    r.empty() ? err.c_str() : ("ok len=" + std::to_string(r.size())).c_str());
        // control: ping
        int64_t ping_id = 0;
        std::string pr = client.call_rpc(build_ping(ping_id), &err);
        std::printf("CONTROL_PING_AFTER %s\n", pr.empty() ? err.c_str() : "ok");
        return 0;
    }

    if (std::strcmp(mode, "fc") == 0) {
        if (chat_id == 0 || ah == 0) return 1;
        const bool wrapped = std::getenv("FC_WRAP") != nullptr;
        TlWriter fc;
        fc.u32(0x8736a09u);
        fc.u32(0xf35aec28u);
        if (std::getenv("FC_FLAGS")) fc.u32(0);
        fc.i64(chat_id);
        fc.i64(ah);
        std::string r = client.call_rpc(fc.out(), &err, wrapped);
        if (r.empty()) {
            std::printf("FC_FAIL %s\n", err.c_str());
        } else {
            TlReader rr2(r);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%08x", rr2.u32());
            std::printf("FC_OK crc=%s len=%zu wrap=%d\n", buf, r.size(), (int)wrapped);
        }
        return 0;
    }

    if (std::strcmp(mode, "probe2") == 0) {
        if (chat_id == 0) return 1;
        int64_t cid = -chat_id - 1000000000000LL;
        // 1) channels.getParticipant#77ced9c1 channel:InputChannel participant:InputPeer
        TlWriter gp;
        gp.u32(0x77ced9c1u);
        gp.u32(0xf35aec28u);
        gp.i64(cid);
        gp.i64(0);
        gp.u32(0xdde8a54cu);
        gp.i64(0);
        gp.i64(0);
        std::string r = client.call_rpc(gp.out(), &err);
        std::printf("GET_PARTICIPANT %s\n", r.empty() ? err.c_str() : ("ok len=" + std::to_string(r.size())).c_str());
        // 2) channels.getFullChannel with ah=0 (pure id lookup)
        TlWriter fc;
        fc.u32(0x8736a09u);
        fc.u32(0xf35aec28u);
        fc.i64(cid);
        fc.i64(0);
        r = client.call_rpc(fc.out(), &err);
        std::printf("GETFULL_AH0 %s\n", r.empty() ? err.c_str() : ("ok len=" + std::to_string(r.size())).c_str());
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
