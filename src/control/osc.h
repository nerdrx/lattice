// OSC receive: a hand-rolled UDP server for the subset of Open Sound Control
// that a control surface actually sends.
//
// NO liblo. The dependency would be a shared library, a build-system branch and
// a packaging line for about two hundred lines of byte-swapping — and liblo's
// own parser has had CVEs in exactly the code this file replaces. What is
// implemented is the subset and nothing else:
//
//   * messages: an OSC-string address, an OSC-string typetag starting with ',',
//     then the arguments. Padded to four bytes, as the spec requires.
//   * typetags: 'i' int32, 'f' float32, 's' string. Anything else is REJECTED,
//     not skipped: a packet we only half understand is a packet from something
//     that expects behaviour we do not have.
//   * bundles: '#bundle', a timetag (parsed, then ignored — there is no
//     scheduler here and a late-executed fader move is worse than a prompt one),
//     then length-prefixed elements, recursively, to a bounded depth.
//
// Everything is bounds-checked against the length the socket reported, never
// against a length the packet claims. The parser is pure and takes (data, len),
// which is what makes it testable against hostile input with no socket at all.
//
// ---------------------------------------------------------------------------
// ADDRESSING. OSC paths map onto the SAME canonical space as MIDI-learn and
// automation (docs/PARAM-ADDRESS.md), under one prefix:
//
//     /nxtakt/t:7/vol            0.8     track 7's fader
//     /nxtakt/t:7/dev:12/p:3     0.25    param 3 of device 12 on track 7
//     /nxtakt/t:7/mute           1       (int or float; >= 0.5 is "on")
//     /nxtakt/s:4/launch                 no argument needed; a trigger
//
// Strip "/nxtakt/" and what is left IS the address, verbatim — which is only
// possible because the grammar was specified to be OSC-safe ("`/`-separated,
// ASCII, no spaces", PARAM-ADDRESS.md).
//
// VALUES ARE NORMALISED 0..1, exactly as the MIDI path's are, and for the same
// reason: a phone running TouchOSC cannot know that a plugin's cutoff runs
// 20..20000, and a remote that had to would break the moment the plugin was
// swapped. The GUI maps 0..1 onto whatever the address resolves to today.
//
// ---------------------------------------------------------------------------
// THREADING. The socket has its own thread. It never touches model state: it
// parses into a POD OscHit and pushes it into a lock-free SPSC ring that the
// GUI drains once a frame. One producer, one consumer, no locks, no allocation
// on the receive path, and a full ring drops. That is the whole contract.
//
// ---------------------------------------------------------------------------
// SECURITY. OSC over UDP has NO authentication, NO integrity check and trivially
// spoofable source addresses. This server therefore binds 127.0.0.1 by default,
// where the only thing that can reach it is a process already running as the
// user. It is OFF unless NXTAKT_OSC is set (see configFromEnv).
//
// BINDING WIDER IS A DELIBERATE ACT WITH REAL CONSEQUENCES. NXTAKT_OSC=0.0.0.0:9000
// means anyone who can route a UDP packet to the machine — a guest on the
// venue's wifi, anything on a shared studio LAN — can move any fader, mute any
// track, launch any scene and drive any plugin parameter, with no credential and
// no trace beyond the log line this prints. There is no rate limit that helps
// and no allow-list here to configure. Bind wide only on a network you control,
// for the length of the show, and prefer an SSH tunnel or a WireGuard address
// over 0.0.0.0 whenever the remote is not on the same machine.
#pragma once
#include "../core/common.h"
#include "../core/ring.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace lat {
namespace ctl {

// One parsed argument. `s` is only meaningful for type 's'.
struct OscArg {
    char type = 0;                 // 'i', 'f' or 's'
    i32  i = 0;
    f32  f = 0.f;
    std::string s;
    // Numeric reading of whatever came in, for a receiver that does not care
    // which of the two number types the sender chose.
    f32 asFloat() const { return type == 'f' ? f : (f32)i; }
};

struct OscMessage {
    std::string path;
    std::vector<OscArg> args;
};

inline constexpr int kOscMaxArgs   = 8;      // more than any surface sends
inline constexpr int kOscMaxPacket = 4096;   // a datagram larger than this is not ours
inline constexpr int kOscMaxDepth  = 4;      // nested bundles

// Parses ONE OSC packet — a message or a bundle. Every message found is
// appended to `out` (a bundle yields several). False means the packet was
// malformed or used something outside the subset, and `out` is then empty.
//
// This function must never read outside [data, data + len). That is the whole
// security surface of the feature and it is what the harness hammers.
bool oscParsePacket(const u8* data, size_t len, std::vector<OscMessage>& out);
// The single-message convenience the tests and the ring path use.
bool oscParseMessage(const u8* data, size_t len, OscMessage& out);

// What crosses the ring: fixed size, trivially copyable, no allocation. A
// string argument is truncated rather than dropped — nothing in the mapped
// address space takes a string, so it exists for diagnostics only.
struct OscHit {
    char path[112] = {};
    char type = 0;                 // 0 = no arguments at all (a trigger)
    f32  value = 0.f;              // numeric reading of the first argument
    char text[24] = {};
};

class OscServer {
public:
    ~OscServer() { stop(); }

    // How NXTAKT_OSC is read. Kept static and pure so it is testable:
    //   unset / "" / "0" / "off" / "no" / "false"  -> disabled
    //   "1" / "on" / "yes" / "true"                -> 127.0.0.1:9000
    //   "9001"                                     -> 127.0.0.1:9001
    //   "127.0.0.1:9001" / "0.0.0.0:9000"          -> that address and port
    struct Config {
        bool enabled = false;
        std::string addr = "127.0.0.1";
        int port = 9000;
        bool wide = false;         // not a loopback address; the UI says so
    };
    static Config configFromEnv(const char* value);
    // The project's env convention (NXTAKT_OSC, falling back to LATTICE_OSC).
    static Config configFromEnvironment();

    bool start(const Config& cfg, std::string* err = nullptr);
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }
    int  port() const { return port_; }
    const std::string& addr() const { return addr_; }
    bool wide() const { return wide_; }

    // GUI thread. Drains one queued hit.
    bool poll(OscHit& out);

    u64 received() const { return received_.load(std::memory_order_relaxed); }
    u64 rejected() const { return rejected_.load(std::memory_order_relaxed); }
    u64 dropped()  const { return dropped_.load(std::memory_order_relaxed); }

private:
    void run();

    int  fd_ = -1;
    int  port_ = 0;
    std::string addr_;
    bool wide_ = false;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<u64> received_{0}, rejected_{0}, dropped_{0};
    Ring<OscHit, 256> ring_;
};

// "/nxtakt/t:7/vol" -> "t:7/vol". False when the path is not under our prefix.
bool oscPathToAddress(const std::string& path, std::string& address);
inline constexpr const char* kOscPrefix = "/nxtakt/";

} // namespace ctl
} // namespace lat
