// OSC receive. See osc.h for the subset, the addressing and the security note.
#include "osc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace lat {
namespace ctl {
namespace {

// ---------------------------------------------------------------------------
// primitives
//
// Every one of these takes a cursor and an END, advances the cursor only on
// success, and never trusts a length the packet itself claims. The whole
// hostile-input surface of the feature is these forty lines.
// ---------------------------------------------------------------------------

inline size_t pad4(size_t n) { return (n + 3u) & ~(size_t)3u; }

bool readU32(const u8*& p, const u8* end, u32& out) {
    if ((size_t)(end - p) < 4) return false;
    out = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    p += 4;
    return true;
}

bool readI32(const u8*& p, const u8* end, i32& out) {
    u32 v = 0;
    if (!readU32(p, end, v)) return false;
    out = (i32)v;
    return true;
}

bool readF32(const u8*& p, const u8* end, f32& out) {
    u32 v = 0;
    if (!readU32(p, end, v)) return false;
    // memcpy, not a pointer cast: the cast is a strict-aliasing violation that
    // -O2 is entitled to miscompile, and this is the one place a wrong answer
    // would look like a fader jumping for no reason.
    std::memcpy(&out, &v, 4);
    return true;
}

// An OSC-string: NUL-terminated, then padded with NULs to a multiple of four.
// The NUL must be INSIDE the buffer — a non-terminated string is the classic
// way to walk an OSC parser off the end of a datagram, and it is rejected here
// rather than clamped, because a truncated address is not an address.
bool readString(const u8*& p, const u8* end, std::string& out, size_t maxLen) {
    const u8* q = p;
    while (q < end && *q != 0) ++q;
    if (q >= end) return false;                    // no terminator in the packet
    const size_t len = (size_t)(q - p);
    if (len > maxLen) return false;
    const size_t adv = pad4(len + 1);
    if ((size_t)(end - p) < adv) return false;     // padding runs past the end
    // The pad bytes must be NUL. Non-zero padding means the sender is not
    // speaking OSC 1.0 and the next field is not where we think it is.
    for (size_t i = len; i < adv; ++i) if (p[i] != 0) return false;
    out.assign((const char*)p, len);
    p += adv;
    return true;
}

bool parseMessageBody(const u8* p, const u8* end, OscMessage& out) {
    out.path.clear();
    out.args.clear();
    if (!readString(p, end, out.path, 255)) return false;
    if (out.path.empty() || out.path[0] != '/') return false;
    // Printable ASCII only, which is both the spec's intent and what keeps a
    // path safe to hand to the address grammar and to a log line.
    for (unsigned char c : out.path) if (c <= 0x20 || c >= 0x7f) return false;

    std::string tags;
    if (!readString(p, end, tags, (size_t)kOscMaxArgs + 1)) return false;
    if (tags.empty() || tags[0] != ',') return false;

    for (size_t i = 1; i < tags.size(); ++i) {
        if ((int)out.args.size() >= kOscMaxArgs) return false;
        OscArg a;
        a.type = tags[i];
        switch (a.type) {
        case 'i': if (!readI32(p, end, a.i)) return false; break;
        case 'f': if (!readF32(p, end, a.f)) return false; break;
        case 's': if (!readString(p, end, a.s, 255)) return false; break;
        // REJECTED, not skipped: 'b' blobs, 'T'/'F'/'N'/'I' (which carry no
        // bytes and would be easy to accept) and everything else. A sender
        // using them wants semantics this receiver does not implement, and
        // guessing is how a "true" becomes a 0.
        default: return false;
        }
        out.args.push_back(std::move(a));
    }
    // Trailing bytes after the last argument mean the typetag and the payload
    // disagree, which is exactly the shape of a truncation or an injection.
    return p == end;
}

bool parsePacket(const u8* data, size_t len, std::vector<OscMessage>& out, int depth) {
    if (depth > kOscMaxDepth) return false;
    if (len == 0 || len > (size_t)kOscMaxPacket) return false;
    if (len % 4 != 0) return false;                // OSC is 4-byte aligned throughout
    if ((int)out.size() > kOscMaxArgs * 4) return false;

    const u8* p = data;
    const u8* end = data + len;

    if (len >= 8 && std::memcmp(data, "#bundle\0", 8) == 0) {
        p += 8;
        u32 hi = 0, lo = 0;
        if (!readU32(p, end, hi) || !readU32(p, end, lo)) return false;   // timetag
        (void)hi; (void)lo;                        // no scheduler; see osc.h
        while (p < end) {
            i32 sz = 0;
            if (!readI32(p, end, sz)) return false;
            if (sz < 0 || (size_t)sz > (size_t)(end - p)) return false;
            if (!parsePacket(p, (size_t)sz, out, depth + 1)) return false;
            p += (size_t)sz;
        }
        return p == end;
    }

    OscMessage m;
    if (!parseMessageBody(p, end, m)) return false;
    out.push_back(std::move(m));
    return true;
}

void copyTrunc(char* dst, size_t cap, const std::string& src) {
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

} // namespace

bool oscParsePacket(const u8* data, size_t len, std::vector<OscMessage>& out) {
    out.clear();
    if (!data) return false;
    if (!parsePacket(data, len, out, 0)) { out.clear(); return false; }
    return !out.empty();
}

bool oscParseMessage(const u8* data, size_t len, OscMessage& out) {
    std::vector<OscMessage> v;
    if (!oscParsePacket(data, len, v) || v.size() != 1) return false;
    out = std::move(v[0]);
    return true;
}

bool oscPathToAddress(const std::string& path, std::string& address) {
    const size_t n = std::strlen(kOscPrefix);
    if (path.size() <= n || path.compare(0, n, kOscPrefix) != 0) return false;
    address = path.substr(n);
    return !address.empty();
}

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

OscServer::Config OscServer::configFromEnv(const char* value) {
    Config c;
    if (!value || !*value) return c;
    std::string v = value;
    // Trim, so a .desktop file's stray space does not silently disable it.
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\n')) v.pop_back();
    if (v.empty() || v == "0" || v == "off" || v == "no" || v == "false") return c;

    c.enabled = true;
    if (v == "1" || v == "on" || v == "yes" || v == "true") return c;   // defaults

    const size_t colon = v.find_last_of(':');
    std::string host, portStr;
    if (colon == std::string::npos) portStr = v;
    else { host = v.substr(0, colon); portStr = v.substr(colon + 1); }

    char* end = nullptr;
    const long p = std::strtol(portStr.c_str(), &end, 10);
    if (!end || *end != '\0' || p < 1 || p > 65535) {
        LOGW("NXTAKT_OSC: '%s' is not a port; using %d", value, c.port);
    } else {
        c.port = (int)p;
    }
    if (!host.empty()) c.addr = host;
    c.wide = !(c.addr == "127.0.0.1" || c.addr == "localhost" || c.addr == "::1");
    return c;
}

OscServer::Config OscServer::configFromEnvironment() {
    return configFromEnv(env("OSC"));
}

// ---------------------------------------------------------------------------
// the server
// ---------------------------------------------------------------------------

bool OscServer::start(const Config& cfg, std::string* err) {
    if (running()) return true;
    if (!cfg.enabled) return false;

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) { if (err) *err = "socket() failed"; return false; }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u16)cfg.port);
    std::string host = cfg.addr;
    if (host == "localhost") host = "127.0.0.1";
    if (::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        ::close(fd_); fd_ = -1;
        if (err) *err = "not an IPv4 address: " + cfg.addr;
        return false;
    }
    if (::bind(fd_, (sockaddr*)&sa, sizeof sa) != 0) {
        ::close(fd_); fd_ = -1;
        if (err) *err = "could not bind " + cfg.addr + ":" + std::to_string(cfg.port);
        return false;
    }

    addr_ = cfg.addr;
    port_ = cfg.port;
    wide_ = cfg.wide;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&OscServer::run, this);

    if (wide_) {
        // Loud, once, and in the log rather than only in a tooltip: this is a
        // remote-control surface with no authentication on a routable address.
        LOGW("osc: listening on %s:%d — UNAUTHENTICATED and reachable from the "
             "network; anything that can send a UDP packet here can drive this set",
             addr_.c_str(), port_);
    } else {
        LOGI("osc: listening on %s:%d (localhost only)", addr_.c_str(), port_);
    }
    return true;
}

void OscServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        return;
    }
    // The reader polls with a timeout rather than blocking in recvfrom, so it
    // notices the flag within one tick and no wakeup datagram is needed.
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    port_ = 0;
}

void OscServer::run() {
    std::vector<OscMessage> msgs;
    msgs.reserve(8);
    u8 buf[kOscMaxPacket];

    while (running_.load(std::memory_order_relaxed)) {
        pollfd pfd{fd_, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 100);
        if (ready <= 0) continue;

        for (;;) {
            const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, MSG_DONTWAIT, nullptr, nullptr);
            if (n <= 0) break;
            // A datagram that filled the buffer exactly may have been truncated
            // by the kernel, and a truncated OSC packet is not an OSC packet.
            if ((size_t)n >= sizeof buf) { rejected_.fetch_add(1, std::memory_order_relaxed); continue; }

            if (!oscParsePacket(buf, (size_t)n, msgs)) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            for (const OscMessage& m : msgs) {
                OscHit h;
                copyTrunc(h.path, sizeof h.path, m.path);
                if (!m.args.empty()) {
                    const OscArg& a = m.args[0];
                    h.type = a.type;
                    h.value = a.asFloat();
                    if (a.type == 's') copyTrunc(h.text, sizeof h.text, a.s);
                }
                if (ring_.push(h)) received_.fetch_add(1, std::memory_order_relaxed);
                else               dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

bool OscServer::poll(OscHit& out) { return ring_.pop(out); }

} // namespace ctl
} // namespace lat
