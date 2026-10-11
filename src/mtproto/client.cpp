#include <zlib.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <random>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "advait/mtproto/client.hpp"
#include "advait/mtproto/tl.hpp"

// Object CRCs (verified against gogram @ e3d2247 / telegram layer 229)
#define CRC_PING 0x7abe77ecu
#define CRC_PONG 0x347773c5u
#define CRC_INVOKE_WITH_LAYER 0xda9b0d0du
#define CRC_INIT_CONNECTION 0xc1cd5ea9u
#define CRC_RPC_ERROR 0x2144ca19u
#define CRC_RPC_ANSWER 0x1b2b2800u
#define CRC_RPC_RESULT 0xf35c6d01u
#define CRC_MESSAGE_CONTAINER 0x73f1f8dcu
#define CRC_MSGS_ACK 0x62d6b459u
#define CRC_NEW_SESSION_CREATED 0x9ec453c9u
#define CRC_NEW_SESSION_ALTERED 0x9ec20908u
#define CRC_BAD_MSG_NOTIFICATION 0xa7eff811u
#define CRC_BAD_SERVER_SALT 0xedab447bu
#define CRC_GZIP_PACKED 0x3072cfa1u
#define CRC_HTTP_WAIT 0x9299359fu
#define CRC_PING_REQ 0x7abe77ecu
#define CRC_PONG 0x23b63b77u
#define CRC_HIGH_PONG 0x347773c5u
#define CRC_GET_CONFIG 0xc4f9186bu

namespace advait::mtproto {

namespace {

constexpr int kLayer = 227;  // gogram v1.7.71 ApiVersion: matches schemes/api.tl used for all CRCs

std::string sha256_of(std::initializer_list<std::string> parts) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    for (const auto& p : parts) EVP_DigestUpdate(ctx, p.data(), p.size());
    std::string out(32, '\0');
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()), &len);
    EVP_MD_CTX_free(ctx);
    return out;
}

std::string sha1_of(const std::string& data) {
    std::string out(20, '\0');
    SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         reinterpret_cast<unsigned char*>(out.data()));
    return out;
}

std::string random_bytes(size_t n) {
    static thread_local std::mt19937_64 rng(std::random_device{}() ^
                                            static_cast<uint64_t>(::getpid()));
    std::string out;
    out.resize(n);
    auto* p = reinterpret_cast<uint64_t*>(out.data());
    size_t i = 0;
    for (; i + 8 <= n; i += 8) p[i / 8] = rng();
    if (i < n) {
        uint64_t v = rng();
        std::memcpy(&out[i], &v, n - i);
    }
    return out;
}

// AES-256-IGE, 16-byte chained blocks. Encrypt: C_i = E(P_i ^ C_{i-1}) ^ P_{i-1}
// with P_{-1} = iv[16:32], C_{-1} = iv[0:16]. Decrypt: P_i = D(C_i ^ P_{i-1}) ^ C_{i-1}.
struct AesIge {
    static bool ecb(const unsigned char* key32, bool enc, const unsigned char* in, unsigned char* out) {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        int olen = 0, tlen = 0;
        EVP_CipherInit_ex(ctx, EVP_aes_256_ecb(), nullptr, key32, nullptr, enc ? 1 : 0);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        bool ok = EVP_CipherUpdate(ctx, out, &olen, in, 16) == 1;
        if (ok) ok = EVP_CipherFinal_ex(ctx, out + olen, &tlen) == 1 && olen + tlen == 16;
        EVP_CIPHER_CTX_free(ctx);
        return ok;
    }
    static bool encrypt(const std::string& key32, const std::string& iv32, const std::string& in,
                        std::string& out) {
        if (in.empty() || in.size() % 16 != 0) return false;
        out.assign(in.size(), '\0');
        const auto* k = reinterpret_cast<const unsigned char*>(key32.data());
        std::array<unsigned char, 16> p_prev{}, c_prev{};
        std::memcpy(c_prev.data(), iv32.data(), 16);
        std::memcpy(p_prev.data(), iv32.data() + 16, 16);
        std::array<unsigned char, 16> block{};
        for (size_t i = 0; i < in.size(); i += 16) {
            for (int j = 0; j < 16; j++) block[j] = static_cast<unsigned char>(in[i + j]) ^ c_prev[j];
            if (!ecb(k, true, block.data(), block.data())) return false;
            for (int j = 0; j < 16; j++) {
                unsigned char o = block[j] ^ p_prev[j];
                out[i + static_cast<size_t>(j)] = static_cast<char>(o);
                c_prev[j] = o;
                p_prev[j] = static_cast<unsigned char>(in[i + j]);
            }
        }
        return true;
    }
    static bool decrypt(const std::string& key32, const std::string& iv32, const std::string& in,
                        std::string& out) {
        if (in.empty() || in.size() % 16 != 0) return false;
        out.assign(in.size(), '\0');
        const auto* k = reinterpret_cast<const unsigned char*>(key32.data());
        std::array<unsigned char, 16> p_prev{}, c_prev{};
        std::memcpy(c_prev.data(), iv32.data(), 16);
        std::memcpy(p_prev.data(), iv32.data() + 16, 16);
        std::array<unsigned char, 16> block{};
        for (size_t i = 0; i < in.size(); i += 16) {
            std::memcpy(block.data(), in.data() + i, 16);
            std::array<unsigned char, 16> x{};
            std::array<unsigned char, 16> cin{};
            std::memcpy(cin.data(), in.data() + i, 16);  // C_i (wire) needed for chain feedback
            for (int j = 0; j < 16; j++) x[j] = block[j] ^ p_prev[j];
            if (!ecb(k, false, x.data(), block.data())) return false;
            for (int j = 0; j < 16; j++) {
                unsigned char o = block[j] ^ c_prev[j];
                out[i + static_cast<size_t>(j)] = static_cast<char>(o);
                p_prev[j] = o;    // x_i = P_i
                c_prev[j] = cin[j];  // y_i = C_i (ciphertext, not decrypt output)
            }
        }
        return true;
    }
};

// Official SHA-256 key scheme; decode=false -> x=0 (client->server), true -> x=8.
void derive_key_iv(const std::string& auth_key, const std::string& msg_key16, bool decode, std::string& key32,
                   std::string& iv32) {
    int x = decode ? 8 : 0;
    // sha256_a = SHA256(msg_key + substr(auth_key, x, 36))
    std::string a = sha256_of({msg_key16, auth_key.substr(x, 36)});
    // sha256_b = SHA256(substr(auth_key, 40+x, 36) + msg_key)
    std::string b = sha256_of({auth_key.substr(40 + x, 36), msg_key16});
    key32 = a.substr(0, 8) + b.substr(8, 16) + a.substr(24, 8);
    iv32 = b.substr(0, 8) + a.substr(8, 16) + b.substr(24, 8);
}

bool gunzip(const std::string& in, std::string& out) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return false;
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    char buf[16384];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return false;
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (out.size() > 64u << 20) {
            inflateEnd(&zs);
            return false;
        }
        if (ret == Z_BUF_ERROR && zs.avail_in == 0) break;
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return true;
}

}  // namespace

Client::Client(Session session, ClientConfig cfg) : session_(std::move(session)), cfg_(std::move(cfg)) {}

Client::~Client() { disconnect(); }

bool Client::socket_connect(std::string* err) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port = std::to_string(session_.port);
    if (::getaddrinfo(session_.ip.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        if (err) *err = "mtproto: dns failed for " + session_.ip;
        return false;
    }
    fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0) {
        ::freeaddrinfo(res);
        if (err) *err = "mtproto: socket() failed";
        return false;
    }
    timeval tv{};
    tv.tv_sec = 15;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (::connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
        ::close(fd_);
        fd_ = -1;
        ::freeaddrinfo(res);
        if (err) *err = "mtproto: tcp connect failed to " + session_.ip;
        return false;
    }
    ::freeaddrinfo(res);
    // abridged transport announcement
    const char magic = static_cast<char>(0xef);
    if (!send_all(std::string(1, magic))) {
        if (err) *err = "mtproto: transport announcement failed";
        return false;
    }
    return true;
}

bool Client::send_all(const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool Client::write_msg(const std::string& msg) {
    std::lock_guard<std::mutex> lk(send_mu_);
    std::string framed;
    size_t words = msg.size() / 4;
    if (words < 0x7f) {
        framed.push_back(static_cast<char>(words));
    } else {
        framed.push_back(static_cast<char>(0x7f));
        framed.push_back(static_cast<char>(words & 0xff));
        framed.push_back(static_cast<char>((words >> 8) & 0xff));
        framed.push_back(static_cast<char>((words >> 16) & 0xff));
    }
    framed += msg;
    return send_all(framed);
}

bool Client::wait_socket_readable(int timeout_ms) {
    pollfd p{fd_, POLLIN, 0};
    int n = ::poll(&p, 1, timeout_ms);
    return n > 0 && (p.revents & POLLIN);
}

bool Client::recv_chunk(int timeout_ms) {
    if (fd_ < 0) return false;
    if (!wait_socket_readable(timeout_ms)) return true;  // timeout, not error
    char buf[65536];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n > 0) {
        recv_buf_.append(buf, static_cast<size_t>(n));
        return true;
    }
    if (n == 0) {
        disconnect();
        return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
    return false;
}

// Returns true when a message (possibly empty = quick ack) was consumed with data already buffered.
bool Client::read_msg(std::string* msg) {
    int64_t deadline = mono_ms() + 3000;
    while (true) {
        if (recv_buf_.empty()) {
            if (!recv_chunk(200)) return false;
            if (recv_buf_.empty()) {
                if (mono_ms() >= deadline) return false;
                continue;  // timeout, keep waiting until deadline
            }
        }
        unsigned char b0 = static_cast<unsigned char>(recv_buf_[0]);
        size_t len;
        if (b0 < 0x7f) {
            len = static_cast<size_t>(b0) * 4;
            size_t hdr = 1;
            if (recv_buf_.size() < hdr + len) return false;  // wait
            *msg = recv_buf_.substr(hdr, len);
            recv_buf_.erase(0, hdr + len);
            return true;
        }
        // 0x7f prefix: 3 LE bytes of word length
        if (recv_buf_.size() < 4) {
            recv_chunk(1000);
            if (recv_buf_.size() < 4) return false;
        }
        len = (static_cast<size_t>(static_cast<unsigned char>(recv_buf_[1])) |
               (static_cast<size_t>(static_cast<unsigned char>(recv_buf_[2])) << 8) |
               (static_cast<size_t>(static_cast<unsigned char>(recv_buf_[3])) << 16)) *
              4;
        if (recv_buf_.size() < 4 + len) return false;
        *msg = recv_buf_.substr(4, len);
        recv_buf_.erase(0, 4 + len);
        return true;
    }
}

static int64_t next_msg_id(int64_t& last) {
    timeval tv{};
    ::gettimeofday(&tv, nullptr);
    int64_t id = (static_cast<int64_t>(tv.tv_sec) << 32) |
                 (static_cast<int64_t>(tv.tv_usec / 1000 * 1000000) & ~3);
    if (id <= last) id = last + 4;
    last = id;
    return id;
}

std::string Client::encrypt_body(const std::string& body, int64_t* msg_id, bool content_related) {
    int64_t mid = next_msg_id(last_msg_id_);  // time-based, %4==0 (pyrogram-verified)
    if (msg_id) *msg_id = mid;

    int32_t seq;
    if (content_related) {
        seq = static_cast<int32_t>(2 * seq_no_ + 1);
        seq_no_ = seq / 2 + 1;
    } else {
        seq = static_cast<int32_t>(2 * seq_no_);
    }

    TlWriter w;
    w.i64(server_salt_);   // server salt (0 until bad_server_salt reveals one)
    w.i64(session_id_);
    w.i64(mid);
    w.i32(seq);
    w.i32(static_cast<int32_t>(body.size()));
    w.raw(body);
    std::string plain = w.out();
    plain += random_bytes(16 + ((16 - (plain.size() % 16)) & 15));
    if (plain.size() % 16 != 0) {
        // never happens with 16..31 byte padding
        plain.resize(((plain.size() / 16) + 1) * 16, '\0');
    }

    // msg_key = substr(SHA256(substr(auth_key,88,32)+padded),8,16)
    std::string large = sha256_of({session_.auth_key.substr(88, 32), plain});
    std::string msg_key = large.substr(8, 16);
    std::string key32, iv32;
    derive_key_iv(session_.auth_key, msg_key, false, key32, iv32);
    std::string cipher;
    if (!AesIge::encrypt(key32, iv32, plain, cipher)) return {};

    std::string akh = sha1_of(session_.auth_key).substr(12, 8);  // auth_key_id
    return akh + msg_key + cipher;
}

bool Client::decrypt_message(const std::string& blob, std::string* body, int64_t* msg_id_out) {
    if (blob.size() < 24) return false;
    std::string msg_key = blob.substr(8, 16);
    std::string cipher = blob.substr(24);
    if (cipher.empty() || cipher.size() % 16 != 0) return false;
    std::string key32, iv32;
    derive_key_iv(session_.auth_key, msg_key, true, key32, iv32);
    std::string plain;
    if (!AesIge::decrypt(key32, iv32, cipher, plain)) return false;
    if (plain.size() < 32) return false;
    TlReader r(plain);
    r.i64();       // salt
    r.i64();       // session id
    int64_t mid = r.i64();
    r.i32();       // seqno
    int32_t blen = r.i32();
    if (!r.ok() || blen < 4 || blen > static_cast<int32_t>(plain.size()) - 32) return false;
    *body = r.blob(blen);
    if (msg_id_out) *msg_id_out = mid;
    return r.ok();
}

void Client::dispatch_update(const std::string& obj) {
    std::function<void(const UpdateEvent&)> handler;
    {
        std::lock_guard<std::mutex> lk(updates_mu_);
        handler = update_handler_;
    }
    if (!handler) return;
    UpdateEvent ev;
    TlReader r(obj);
    uint32_t crc = r.u32();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", crc);
    ev.kind = std::string("update:") + buf;
    ev.payload = obj;
    handler(ev);
}

// message_container children are plaintext messages: msg_id(8) seqno(4) body_len(4) body.
bool Client::handle_plaintext(const std::string& body, int64_t req_id, std::string* out_result, bool* matched) {
    *matched = false;
    if (body.size() < 4) return true;
    TlReader r(body);
    uint32_t crc = r.u32();
    switch (crc) {
        case CRC_MESSAGE_CONTAINER: {
            uint32_t count = static_cast<uint32_t>(r.i32());  // not a vector: bare int32
            if (!r.ok() || count > 4096) return false;
            for (uint32_t i = 0; i < count; i++) {
                r.i64();  // msg_id
                r.i32();  // seqno
                int32_t blen = r.i32();
                std::string child = r.blob(blen);
                if (!r.ok()) break;
                bool m = false;
                if (!handle_plaintext(child, req_id, out_result, &m)) return false;
                if (m) {
                    *matched = true;
                    return true;
                }
            }
            return true;
        }
        case CRC_RPC_RESULT: {
            // server may deliver rpc_result as its own message body
            int64_t answer = r.i64();
            if (answer == req_id) {
                *out_result = r.remaining() ? body.substr(r.pos()) : std::string();
                *matched = r.ok();
            }
            return true;
        }
        case CRC_RPC_ANSWER: {
            int64_t answer = r.i64();
            std::string inner = body.substr(r.pos());
            if (answer == req_id) {
                TlReader ir(inner);
                uint32_t ic = ir.u32();
                if (ic == CRC_RPC_RESULT) {
                    ir.i64();  // req_msg_id
                    *out_result = ir.remaining() ? inner.substr(ir.pos()) : std::string();
                    if (ir.ok()) {
                        *matched = true;
                        return true;
                    }
                } else if (ic == CRC_MSGS_ACK || ic == 0x5bc0ee87u /*rpc_answer_dropped_running*/ ||
                           ic == 0xde53dab) {
                    return true;
                } else {
                    // unexpected inner object for our request (e.g. bad server salt inside)
                    return handle_plaintext(inner, 0, out_result, matched);
                }
            }
            return true;
        }
        case CRC_GZIP_PACKED: {
            // deployed layout: gzip_packed crc + int32 + zlib/gzip stream to end of body
            std::string packed = body.size() > 8 ? body.substr(8) : std::string();
            std::string inflated;
            if (!gunzip(packed, inflated)) return false;
            return handle_plaintext(inflated, req_id, out_result, matched);
        }
        case CRC_BAD_SERVER_SALT: {
            // bad_server_salt (layer-158 shape): bad_msg_id:long bad_msg_seqno:int error_code:int new_server_salt:long
            r.i64();  // bad_msg_id
            r.i32();  // bad_msg_seqno
            r.i32();  // error_code
            int64_t salt = r.i64();
            if (r.ok()) {
                server_salt_ = salt;
                salt_retry_ = true;
            }
            return true;
        }
        case CRC_NEW_SESSION_CREATED: {
            r.i64();  // first_msg_id
            r.i64();  // unique_id
            int64_t salt = r.i64();
            if (r.ok()) {
                server_salt_ = salt;
                salt_retry_ = true;
            }
            return true;
        }
        case CRC_PONG:
        case CRC_HIGH_PONG: {
            if (expect_pong_) {
                *out_result = body;
                *matched = true;
            }
            return true;
        }
        default: {
            // pushed update / misc notice: dispatch, keep waiting for our rpc_answer
            dispatch_update(body);
            return true;
        }
    }
}

// message_container children are plaintext message bodies: length-prefixed sub-blobs.
// helper to read inner object length (body is already the child body; length is read by parent)
// -> we implement container iteration manually in call_rpc instead of via this function.

bool Client::connect(std::string* err) {
    if (connected()) return true;
    std::lock_guard<std::mutex> lk(send_mu_);
    if (!socket_connect(err)) return false;
    session_id_ = static_cast<int64_t>(std::random_device{}());
    last_msg_id_ = 0;
    seq_no_ = 0;
    recv_buf_.clear();
    initialized_ = true;
    return true;
}

void Client::disconnect() {
    std::lock_guard<std::mutex> lk(send_mu_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    initialized_ = false;
}

static void wrap_init_connection(const std::string& raw_request, int32_t api_id, const ClientConfig& cfg,
                                 std::string& out) {
    // invokeWithLayer#da9b0d0d {layer} initConnection#c1cd5ea9 {flags} {... query}
    // Deployed schema HAS flags (proxy/params bits, pyrogham 2.0.106 writer verified).
    TlWriter w;
    w.u32(CRC_INVOKE_WITH_LAYER);
    w.i32(kLayer);
    w.u32(CRC_INIT_CONNECTION);
    w.i32(0);      // flags: no proxy, no params
    w.i32(api_id);
    w.str(cfg.device_model);
    w.str(cfg.system_version);
    w.str(cfg.app_version);
    w.str(cfg.lang_code);  // system_lang_code
    w.str("");             // lang_pack
    w.str(cfg.lang_code);
    w.raw(raw_request);
    out = w.out();
}

bool Client::init_connection(std::string* err) {
    // mirrors pyrogram Session.start(): Ping(ping_id=0) first, then
    // InvokeWithLayer(InitConnection(help.getConfig)). Both tolerate one salt retry.
    TlWriter pw;
    pw.u32(CRC_PING_REQ);
    pw.i64(0x41647661LL);  // static ping id
    std::string pong = call_rpc(pw.out(), err, false, true);
    if (pong.empty()) {
        if (err && err->empty()) *err = "init ping: no pong";
        return false;
    }
    TlWriter gc;
    gc.u32(CRC_GET_CONFIG);
    std::string cfg_resp = call_rpc(gc.out(), err, true);
    if (cfg_resp.empty()) {
        if (err && err->empty()) *err = "init help.getConfig failed";
        return false;
    }
    rpc_ready_ = true;
    return true;
}

std::string Client::call_rpc(const std::string& serialized_request, std::string* err, bool wrap_init,
                             bool match_pong) {
    struct PongGuard {
        bool* f;
        ~PongGuard() { *f = false; }
    } pg{&expect_pong_};
    auto fail = [&](const std::string& m) -> std::string {
        if (err) *err = m;
        disconnect();
        rpc_ready_ = false;
        return {};
    };
    // one-time connection init (pyrogram order): bare Ping then initConnection(help.getConfig)
    if (!rpc_ready_ && !init_active_) {
        init_active_ = true;
        bool ok = init_connection(err);
        init_active_ = false;
        if (!ok) return {};
    }
    // pyrogram semantics: BadServerSalt discards the message; resend exactly once per notice.
    std::string to_send;
    if (wrap_init) {
        wrap_init_connection(serialized_request, session_.api_id, cfg_, to_send);
    } else {
        to_send = serialized_request;
    }
    expect_pong_ = match_pong;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!connect(err)) {
            if (err) *err = "mtproto: " + *err;
            return {};
        }

        int64_t req_id = 0;
        std::string enc = encrypt_body(to_send, &req_id, !match_pong);
        if (enc.empty()) return fail("mtproto: encrypt failed");
        if (!write_msg(enc)) return fail("mtproto: send failed");

        int64_t deadline = mono_ms() + 25000;
        while (mono_ms() < deadline) {
            std::string msg;
            if (!read_msg(&msg)) {
                if (fd_ < 0) return fail("mtproto: connection closed");
                continue;  // timeout with no data
            }
            if (msg.empty()) continue;  // quick ack
            // verify auth_key_id matches
            std::string akh = sha1_of(session_.auth_key).substr(12, 8);
            if (msg.size() >= 8 && msg.substr(0, 8) != akh) {
                if (msg.size() == 4) {
                    int32_t code = 0;
                    std::memcpy(&code, msg.data(), 4);
                    return fail("mtproto: transport error " + std::to_string(code));
                }
                return fail("mtproto: auth_key_id mismatch len=" + std::to_string(msg.size()));
            }
            std::string body;
            int64_t mid = 0;
            if (!decrypt_message(msg, &body, &mid)) continue;
            if (match_pong && body.size() >= 12) {
                TlReader pr(body);
                uint32_t pcrc = pr.u32();
                if (pcrc == CRC_PONG || pcrc == CRC_HIGH_PONG) return body;
            }
            bool matched = false;
            std::string result;
            if (!handle_plaintext(body, req_id, &result, &matched)) {
                return fail("mtproto: malformed server message");
            }
            if (salt_retry_) {
                // pyrogram session.py: store salt then resend the same request (message was
                // discarded by the server; waiting without resend just times out).
                salt_retry_ = false;
                enc = encrypt_body(to_send, &req_id, !match_pong);
                if (enc.empty()) return fail("mtproto: encrypt failed after salt update");
                if (!write_msg(enc)) return fail("mtproto: resend failed");
                continue;
            }
            if (matched) {
                // answers may arrive gzip-packed: transparently unwrap
                while (result.size() > 4) {
                    TlReader gr(result);
                    if (gr.u32() != CRC_GZIP_PACKED) break;
                    std::string inflated;
                    if (result.size() <= 8 || !gunzip(result.substr(8), inflated)) break;
                    result = std::move(inflated);
                }
                // translate rpc_error objects into errors
                if (result.size() >= 4) {
                    TlReader rr(result);
                    uint32_t crc = rr.u32();
                    if (crc == CRC_RPC_ERROR) {
                        int32_t code = rr.i32();
                        std::string text = rr.str();
                        *err = "telegram_error:" + std::to_string(code) + ":" + (rr.ok() ? text : "malformed");
                        return {};
                    }
                }
                return result;
            }
        }
        return fail("mtproto: rpc timeout");
    }
    return {};
}

void Client::poll_updates() {
    if (!connected()) return;
    while (true) {
        std::string msg;
        if (!read_msg(&msg)) {
            if (fd_ < 0) return;
            return;  // no more buffered data right now
        }
        if (msg.empty()) continue;
        std::string body;
        int64_t mid = 0;
        if (!decrypt_message(msg, &body, &mid)) continue;
        // unwrap containers, dispatch pushes
        if (body.size() >= 4) {
            TlReader peek(body);
            uint32_t crc = peek.u32();
            bool matched = false;
            std::string ignore;
            if (crc == CRC_MESSAGE_CONTAINER) {
                TlReader r(body);
                r.u32();  // crc
                uint32_t n = static_cast<uint32_t>(r.i32());
                while (n-- && r.ok()) {
                    r.i64();  // msg_id
                    r.i32();  // seqno
                    int32_t len = r.i32();
                    std::string child = r.blob(len);
                    if (!r.ok()) break;
                    handle_plaintext(child, 0, &ignore, &matched);
                }
            } else {
                handle_plaintext(body, 0, &ignore, &matched);
            }
        }
    }
}

void Client::set_update_handler(std::function<void(const UpdateEvent&)> h) {
    std::lock_guard<std::mutex> lk(updates_mu_);
    update_handler_ = std::move(h);
}

}  // namespace advait::mtproto
