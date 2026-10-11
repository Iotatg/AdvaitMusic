#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string>

#include "advait/mtproto/client.hpp"

// Session parser supporting the two formats used across the KanhaMusic ecosystem:
//   * pyrogram string sessions (SESSION_TYPE=pyrogram): 271 raw bytes, big-endian
//     dc_id(1) api_id(4) test_mode(1) auth_key(256) user_id(8) bot(1)
//   * gogram string sessions: ascii "1BvE" + base64url json {key,hash,dc_id,ip_addr,app_id,...}
namespace advait::mtproto {

namespace {

std::string b64url_decode(const std::string& in) {
    // Supports both standard base64 (+/) and base64url (-_) alphabets.
    static const std::string alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int rev[256];
    std::fill(std::begin(rev), std::end(rev), -1);
    for (int i = 0; i < 64; i++) rev[static_cast<unsigned char>(alpha[i])] = i;
    rev[static_cast<unsigned char>('+')] = 62;
    rev[static_cast<unsigned char>('/')] = 63;
    std::string out;
    int bits = 0, acc = 0;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int v = rev[c];
        if (v < 0) throw std::runtime_error("mtproto: bad base64url char");
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xff));
            acc &= (1 << bits) - 1;
        }
    }
    return out;
}

uint32_t be32(const std::string& s, size_t off) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(s[off])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(s[off + 3]));
}

uint64_t be64(const std::string& s, size_t off) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++) v = (v << 8) | static_cast<uint8_t>(s[off + i]);
    return v;
}

// Minimal JSON object reader (flat, string/number values) for the gogram session payload.
struct MiniJson {
    const std::string& s;
    size_t i = 0;
    void ws() { while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) i++; }
    std::string key() {
        ws();
        if (s[i] != '"') throw std::runtime_error("mtproto: json key");
        size_t st = ++i;
        while (i < s.size() && s[i] != '"') i++;
        std::string k = s.substr(st, i - st);
        i++;  // closing quote
        ws();
        if (s[i] != ':') throw std::runtime_error("mtproto: json colon");
        i++;
        ws();
        return k;
    }
    std::string raw_value() {
        size_t st = i;
        if (s[i] == '"') {
            i++;
            while (i < s.size() && s[i] != '"') i++;
            return s.substr(st + 1, i - st - 1);
        }
        if (s[i] == '{' || s[i] == '[') {
            int depth = 0;
            for (; i < s.size(); i++) {
                if (s[i] == '{' || s[i] == '[') depth++;
                else if (s[i] == '}' || s[i] == ']') depth--;
                if (depth == 0) { i++; break; }
            }
            return s.substr(st, i - st);
        }
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
        return s.substr(st, i - st);
    }
};

}  // namespace

std::string Client::dc_ip(int dc_id) {
    // gogram default DC table (main net)
    switch (dc_id) {
        case 1: return "149.154.175.58";
        case 2: return "149.154.167.50";
        case 3: return "149.154.175.100";
        case 4: return "149.154.167.91";
        case 5: return "91.108.56.151";
        default: return {};
    }
}

Session Client::parse_session_string(const std::string& encoded) {
    Session ses;
    // gogram-native format: version prefix + base64url(json)
    if (encoded.size() > 4 && encoded.compare(0, 4, "1BvE") == 0) {
        std::string js = b64url_decode(encoded.substr(4));
        MiniJson j{js};
        std::string key_b64;
        int dc_id = 0;
        std::string ip;
        int64_t app_id = 0;
        size_t i = 0;
        while (i < js.size()) {
            // walk flat object members
            size_t start = js.find_first_not_of(" \t\n\r{,", i);
            if (start == std::string::npos || js[start] == '}') break;
            i = start;
            std::string k = j.key();
            std::string v = j.raw_value();
            // raw_value left us at delimiter; skip separators
            j.i = js.find_first_not_of(" \t\n\r", j.i);
            i = j.i;
            if (k == "key") key_b64 = v;
            else if (k == "dc_id") dc_id = atoi(v.c_str());
            else if (k == "ip_addr") ip = v;
            else if (k == "app_id") app_id = atoll(v.c_str());
        }
        if (key_b64.empty() || dc_id == 0) throw std::runtime_error("mtproto: gogram session missing key/dc");
        ses.auth_key = b64url_decode(key_b64);
        if (ses.auth_key.size() != 256) throw std::runtime_error("mtproto: gogram key length " + std::to_string(ses.auth_key.size()));
        ses.dc_id = dc_id;
        ses.ip = !ip.empty() ? ip : dc_ip(dc_id);
        ses.api_id = static_cast<int32_t>(app_id);
        return ses;
    }

    // pyrogram format (SESSION_TYPE=pyrogram): standard base64 of 271 bytes
    // Also accept plain base64url variant.
    std::string raw = b64url_decode(encoded);
    if (raw.size() == 271) {
        ses.dc_id = static_cast<uint8_t>(raw[0]);
        ses.api_id = static_cast<int32_t>(be32(raw, 1));
        // raw[5] = test_mode
        ses.auth_key = raw.substr(6, 256);
        ses.user_id = static_cast<int64_t>(be64(raw, 262));
        // raw[270] = bot flag
        if (ses.dc_id < 1 || ses.dc_id > 5) throw std::runtime_error("mtproto: pyrogram session dc=" + std::to_string(ses.dc_id));
        ses.ip = dc_ip(ses.dc_id);
        return ses;
    }
    throw std::runtime_error("mtproto: unknown session format len=" + std::to_string(raw.size()));
}

}  // namespace advait::mtproto
