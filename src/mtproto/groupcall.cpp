// Group voice-call RPC helpers built on the raw call_rpc transport.
// Requests are encoded structurally from schemes/api.tl (layer 227); large
// response objects (Channel, ChannelFull, User, Updates) are located by
// scanning for nested constructor ids instead of carrying full generated
// schema decoders. Each scan target appears at most once in these specific
// responses, and a wrong guess is rejected by the server on the next request.
#include <cstdio>
#include <cstring>

#include "advait/mtproto/client.hpp"

namespace advait::mtproto {
namespace {

// Little-endian constructor id search.
size_t find_crc(const std::string& hay, uint32_t crc, size_t from = 0) {
    char pat[4];
    std::memcpy(pat, &crc, 4);
    return hay.find(pat, from, 4);
}

}  // namespace

// Resolve the account-visible access_hash of a superchannel, accepting raw
// ids as well as marked/TDLib forms such as -100xxxxxxxx or
// -(min_channel_id << 40) - id; returns the resolved raw id via channel_out.
bool Client::resolve_channel(int64_t chat_id, int64_t* channel_out, int64_t* ah, std::string* err) {
    std::vector<int64_t> candidates;
    const int64_t raw = chat_id >= 0 ? chat_id : -chat_id;
    if (raw >= 100000000000 && raw < 1000000000000000) {
        // Marked form: -100 prefixed to the decimal digits of the channel id.
        uint64_t m = static_cast<uint64_t>(raw);
        for (uint64_t pow10 = 1000000000ULL; pow10 <= 100000000000000ULL; pow10 *= 10) {
            uint64_t stripped = m - 100 * pow10;
            if (stripped < pow10) {
                candidates.push_back(static_cast<int64_t>(stripped));
                break;
            }
        }
    }
    candidates.push_back(static_cast<int64_t>(raw));
    std::string last_err;
    for (int64_t cand : candidates) {
        TlWriter w;
        w.u32(0xa7f6bbb);      // channels.getChannels
        w.vec_header(1);
        w.u32(0xf35aec28u);    // inputChannel (access_hash 0 = resolve)
        w.i64(cand);
        w.i64(0);
        std::string err_local;
        std::string resp = call_rpc(w.out(), &err_local);
        if (resp.empty()) {
            last_err = err_local;
            continue;
        }
        if (resp.size() == 12 && find_crc(resp, 0x64ff9fd5u) != std::string::npos) {
            last_err = "channel not visible to session account (add the assistant to the group)";
            continue;
        }
        // Server may answer with either current constructor; both share the
        // flags/flags2/id/access_hash prefix we read.
        size_t p = find_crc(resp, 0x1c32b11cu);  // channel#1c32b11c
        if (p == std::string::npos) {
            p = find_crc(resp, 0xd49f34c6u);  // channel#d49f34c6 (deployed)
        }
        if (p == std::string::npos || p + 28 > resp.size()) {
            char hx[3];
            std::string dump;
            for (size_t i = 0; i < std::min<size_t>(40, resp.size()); i++) {
                std::snprintf(hx, sizeof(hx), "%02x", static_cast<unsigned char>(resp[i]));
                dump += hx;
            }
            last_err = "channel object not found len=" + std::to_string(resp.size()) + " " + dump;
            continue;
        }
        // channel: flags{bools} flags2 id:long access_hash:flags.13?long ...
        TlReader r(resp, p + 4);
        uint32_t f1 = r.u32();
        r.u32();  // flags2
        int64_t id = r.i64();
        int64_t hash = 0;
        if (f1 & (1u << 13)) {
            hash = r.i64();
        }
        if (!r.ok() || id != cand || hash == 0) {
            last_err = "channel access_hash missing";
            continue;
        }
        *channel_out = cand;
        *ah = hash;
        return true;
    }
    if (err) {
        *err = last_err;
    }
    return false;
}

bool Client::get_group_call(int64_t chat_id, GroupCallRef* out, std::string* err) {
    int64_t cah = 0;
    int64_t channel_id = chat_id;
    if (!resolve_channel(chat_id, &channel_id, &cah, err)) {
        return false;
    }
    TlWriter w;
    w.u32(0x8736a09u);        // channels.getFullChannel
    w.u32(0xf35aec28u);       // inputChannel
    w.i64(channel_id);
    w.i64(cah);
    std::string resp = call_rpc(w.out(), err);
    if (resp.empty()) {
        return false;
    }
    if (find_crc(resp, 0x2633421bu) == std::string::npos &&   // chatFull
        find_crc(resp, 0xa04e8d3au) == std::string::npos) {   // channelFull
        if (err) {
            *err = "getFullChannel: unexpected response shape";
        }
        return false;
    }
    size_t p = find_crc(resp, 0xd8aa840fu);  // inputGroupCall
    if (p == std::string::npos || p + 20 > resp.size()) {
        if (err) {
            *err = "no active group call";
        }
        return false;
    }
    TlReader r(resp, p + 4);
    out->id = r.i64();
    out->access_hash = r.i64();
    return r.ok() && out->id != 0 && out->access_hash != 0;
}

bool Client::self_access_hash(int64_t* out, std::string* err) {
    if (self_ah_ != 0) {
        *out = self_ah_;
        return true;
    }
    TlWriter w;
    w.u32(0xd91a548u);     // users.getUsers
    w.vec_header(1);
    w.u32(0xf7c1b13fu);    // inputUserSelf
    std::string resp = call_rpc(w.out(), err);
    if (resp.empty()) {
        return false;
    }
    // users.users{Vector<User>} or bare user; user#31774388 field order is
    // flags{self:flags.10 ...} id:long access_hash:flags.0?long ...
    size_t p = find_crc(resp, 0x31774388u);
    if (p == std::string::npos || p + 24 > resp.size()) {
        if (err) {
            char hx[3];
            std::string dump;
            for (size_t i = 0; i < std::min<size_t>(24, resp.size()); i++) {
                std::snprintf(hx, sizeof(hx), "%02x", static_cast<unsigned char>(resp[i]));
                dump += hx;
            }
            *err = "self user object not found len=" + std::to_string(resp.size()) + " " + dump;
        }
        return false;
    }
    TlReader r(resp, p + 4);
    uint32_t flags = r.u32();
    r.u32();  // flags2 (zero-width bools between carry no bytes)
    int64_t id = r.i64();
    if (!r.ok()) {
        if (err) {
            *err = "self user truncated";
        }
        return false;
    }
    if (!(flags & 1u) || id != session_.user_id) {
        if (err) {
            char hx[64];
            std::snprintf(hx, sizeof(hx), "id=%lld/%lld flags=%08x", (long long)id,
                          (long long)session_.user_id, flags);
            *err = std::string("self user mismatch ") + hx;
        }
        return false;
    }
    int64_t ah = r.i64();
    if (!r.ok() || ah == 0) {
        if (err) {
            *err = "self access hash missing";
        }
        return false;
    }
    self_ah_ = ah;
    *out = ah;
    return true;
}

bool Client::join_group_call(const GroupCallRef& call, const std::string& params_json, std::string* server_params,
                             std::string* err) {
    int64_t ah = 0;
    if (!self_access_hash(&ah, err)) {
        return false;
    }
    TlWriter w;
    w.u32(0x8fb53057u);    // phone.joinGroupCall
    w.u32(0x4u);           // flags: video_stopped (audio-only join), muted=false
    w.u32(0xd8aa840fu);    // inputGroupCall
    w.i64(call.id);
    w.i64(call.access_hash);
    w.u32(0xdde8a54cu);    // inputPeerUser (join as self)
    w.i64(session_.user_id);
    w.i64(ah);
    w.u32(0x7d748d04u);    // dataJSON (our WebRTC offer from libntgcalls)
    w.str(params_json);
    std::string resp = call_rpc(w.out(), err);
    if (resp.empty()) {
        return false;
    }
    // updates#74ae4240 ... updateGroupCallConnection#0b783982 flags presentation;params:DataJSON
    size_t p = find_crc(resp, 0x0b783982u);
    if (p == std::string::npos) {
        if (err) {
            *err = "join accepted but no connection update";
        }
        return false;
    }
    TlReader r(resp, p + 4);
    r.u32();  // flags
    if (r.u32() != 0x7d748d04u) {
        if (err) {
            *err = "malformed connection params object";
        }
        return false;
    }
    *server_params = r.str();
    if (!r.ok() || server_params->empty()) {
        if (err) {
            *err = "empty connection params";
        }
        return false;
    }
    return true;
}

}  // namespace advait::mtproto
