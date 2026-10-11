#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Little-endian TL binary writer.
namespace advait::mtproto {

class TlWriter {
public:
    void u32(uint32_t v) {
        char b[4];
        std::memcpy(b, &v, 4);
        buf_.append(b, 4);
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void i64(int64_t v) { u32(static_cast<uint32_t>(v)); u32(static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32)); }
    void bytes(const void* p, size_t n) { buf_.append(static_cast<const char*>(p), n); }
    void raw(const std::string& s) { buf_.append(s); }
    void str(const std::string& s) {
        size_t len = s.size();
        if (len < 254) {
            buf_.push_back(static_cast<char>(len));
            buf_.append(s);
            size_t pad = (4 - ((1 + len) % 4)) % 4;
            buf_.append(pad, '\0');
        } else {
            buf_.push_back(static_cast<char>(0xfe));
            buf_.push_back(static_cast<char>(len & 0xff));
            buf_.push_back(static_cast<char>((len >> 8) & 0xff));
            buf_.push_back(static_cast<char>((len >> 16) & 0xff));
            buf_.append(s);
            size_t pad = (4 - ((4 + len) % 4)) % 4;
            buf_.append(pad, '\0');
        }
    }
    void vec_header(uint32_t count) { u32(0x1cb5c415); u32(count); }
    const std::string& out() const { return buf_; }
    // Back-patch helper used to splice pre-encoded TL objects (e.g. initConnection query).
    std::string& mutable_out() { return buf_; }

private:
    std::string buf_;
};

// Bounds-checked TL reader.
class TlReader {
public:
    TlReader(const std::string& s, size_t off = 0) : s_(s), p_(off) {}

    bool ok() const { return err_ == 0 && p_ <= s_.size(); }
    size_t pos() const { return p_; }
    size_t remaining() const { return p_ <= s_.size() ? s_.size() - p_ : 0; }

    uint32_t u32() {
        need(4);
        uint32_t v;
        std::memcpy(&v, s_.data() + p_, 4);
        p_ += 4;
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    int64_t i64() {
        uint32_t lo = u32();
        uint32_t hi = u32();
        return static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
    }
    std::string blob(size_t n) {
        need(n);
        std::string out = s_.substr(p_, n);
        p_ += n;
        return out;
    }
    std::string str() {
        size_t l0 = static_cast<uint8_t>(blob(1)[0]);
        size_t len, hdr;
        if (l0 < 254) {
            len = l0;
            hdr = 1;
        } else {
            std::string b = blob(3);
            len = static_cast<uint8_t>(b[0]) | (static_cast<uint8_t>(b[1]) << 8) | (static_cast<uint8_t>(b[2]) << 16);
            hdr = 4;
        }
        std::string out = blob(len);
        size_t pad = (4 - ((hdr + len) % 4)) % 4;
        blob(pad);
        return out;
    }
    uint32_t expect_vec() {
        uint32_t magic = u32();
        if (magic != 0x1cb5c515) {
            err_ = 1;
            return 0;
        }
        return u32();
    }

private:
    void need(size_t n) {
        if (err_ || p_ + n > s_.size()) {
            err_ = 1;
            p_ = s_.size();
        }
    }
    const std::string& s_;
    size_t p_;
    int err_ = 0;
};

}  // namespace advait::mtproto
