// Lattice IPC — POSIX shared memory transport for the engine/GUI process split.
//
// This header is the wire layer and nothing else: it knows how to hand a block
// of memory to two processes and how to move POD messages across it in one
// direction without locks. It knows nothing about clips, tracks or plugins.
// docs/PROCESS-SPLIT.md describes what gets carried over it.
//
// Three pieces:
//
//   ShmRegion     shm_open + mmap with a validated header. One process is the
//                 creator (sizes the region, initialises it, owns the unlink),
//                 every other process is an attacher (read/write, never
//                 unlinks).
//   ShmSpscRing   the same contract as lat::Ring (src/core/ring.h) but the
//                 buffer and both indices live inside a ShmRegion, so producer
//                 and consumer can be in different processes.
//   SharedStateT  the polled scalar block — the cross-process form of the
//                 std::atomic members the GUI reads off Engine every frame.
//
// Header-only on purpose. The engine daemon, the GUI and the tests all want
// this and none of them should have to agree on a link order to get it; it
// also keeps src/ipc out of the app's `find src -name '*.cpp'` build.
//
// Everything here is Linux-specific (shm_open, /proc for liveness). That is
// fine: Lattice is a native Linux DAW. The Windows port in backend_win32.cpp
// would need a CreateFileMapping twin, not a portability shim here.
#pragma once
#include "../core/common.h"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace lat::ipc {

// ---------------------------------------------------------------------------
// Constants and small helpers
// ---------------------------------------------------------------------------

// "LTC_SHM1" — a byte pattern that is not plausibly the start of anything else
// somebody might leave in /dev/shm under a colliding name.
inline constexpr u64 kShmMagic = 0x4C54435F53484D31ull;

// Bump on ANY change to ShmHeader, ShmSpscRing or SharedStateT layout, or to
// the meaning of a field. A mismatched attacher must fail rather than
// misinterpret; that is the whole point of the field.
//
//   v2 — SharedStateT gained recState[]/recSlotIdx[] so the block mirrors
//        Engine's published atomics exactly (wave 2, the engine daemon).
inline constexpr u32 kShmVersion = 2;

// Indices and payload sit on separate lines so the producer's write index does
// not invalidate the consumer's cache line on every push. Cross-process this
// matters more than in-process: the two sides are on unrelated cores with no
// scheduler affinity between them.
inline constexpr size_t kCacheLine = 64;

// The payload starts here, past the header. Fixed rather than sizeof-derived so
// that adding a reserved field to ShmHeader does not silently move every
// payload offset out from under an older peer — such a change must go through
// kShmVersion instead.
inline constexpr size_t kPayloadOffset = 256;

inline constexpr size_t alignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// FNV-1a. Callers fold a description of their payload layout (struct sizes,
// ring capacities, a literal name) into a single u32 that goes in the header,
// so two builds that disagree about where the rings live refuse to talk even
// when kShmVersion happens to match. Cheap insurance against the "I forgot to
// bump the version" failure, which is the one that actually happens.
constexpr u32 fnv1a(const char* s, u32 h = 2166136261u) {
    return *s ? fnv1a(s + 1, (u32)((h ^ (u32)(u8)(*s)) * 16777619ull)) : h;
}
constexpr u32 hashMix(u32 h, u64 v) {
    for (int i = 0; i < 8; ++i) h = (u32)((h ^ (u32)((v >> (i * 8)) & 0xffu)) * 16777619ull);
    return h;
}

inline u64 monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Creator liveness — the crash-orphan story
// ---------------------------------------------------------------------------
//
// A POSIX shm object outlives every process that mapped it; only shm_unlink
// removes the name. So the discipline is:
//
//   * the creator unlinks in close()/its destructor. After shm_unlink the name
//     is gone but existing mappings keep working, so an attacher mid-session is
//     not yanked out from under — it just cannot be re-attached to. That is
//     exactly the semantics we want for "engine is going away".
//   * attachers never unlink. Two attachers racing to unlink would let a third
//     process create a *different* region under the same name while the second
//     still thinks it is talking to the first.
//   * if the creator dies without unlinking (SIGKILL, segfault, OOM) the region
//     is orphaned and /dev/shm keeps the pages alive forever. The next creator
//     hits EEXIST. It must not blindly unlink — a live engine may legitimately
//     own that name — so it asks whether the recorded creator is still alive.
//
// Liveness is pid + start-time, never pid alone: pids are recycled, and
// "unlink the region because pid 4711 is gone" is catastrophic if 4711 is now
// somebody else's engine. If /proc is unreadable we deliberately conclude
// "alive" and refuse to reclaim, because leaking a region is survivable and
// stealing a live one is not.

// Field 22 of /proc/<pid>/stat, in clock ticks since boot. 0 if unavailable.
inline u64 procStartTicks(i32 pid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    std::FILE* f = std::fopen(path, "re");
    if (!f) return 0;
    char buf[1024];
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';
    // comm (field 2) is parenthesised and may itself contain spaces and
    // parens, so tokenising has to start after the *last* ')'.
    const char* p = std::strrchr(buf, ')');
    if (!p) return 0;
    ++p;
    int field = 3;                              // first token after comm
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (field == 22) return std::strtoull(p, nullptr, 10);
        while (*p && *p != ' ') ++p;
        ++field;
    }
    return 0;
}

inline bool processAlive(i32 pid, u64 startTicks) {
    if (pid <= 0) return false;
    if (::kill((pid_t)pid, 0) != 0 && errno != EPERM) return false;
    const u64 now = procStartTicks(pid);
    // Unknown start time on either side -> cannot prove reuse -> assume alive.
    return now == 0 || startTicks == 0 || now == startTicks;
}

// ---------------------------------------------------------------------------
// Region header
// ---------------------------------------------------------------------------

struct ShmHeader {
    u64 magic;                  // kShmMagic
    u32 version;                // kShmVersion of the creator
    u32 headerBytes;            // == kPayloadOffset
    u64 totalBytes;             // whole mapping, header included
    u32 layoutHash;             // caller-supplied payload layout fingerprint
    i32 creatorPid;
    u64 creatorStartTicks;      // guards against pid reuse
    std::atomic<u32> ready;     // 0 until the creator has initialised the payload
    std::atomic<u32> attached;  // informational: successful attach() count
    u32 reserved[8];
};
static_assert(sizeof(ShmHeader) <= kPayloadOffset, "header must fit before the payload");
static_assert(sizeof(std::atomic<u32>) == 4, "shared atomics must have the obvious layout");
static_assert(std::atomic<u32>::is_always_lock_free,
              "a shared atomic backed by a lock table would deadlock across processes");
static_assert(std::atomic<u64>::is_always_lock_free, "64-bit shared atomics must be lock-free");

// ---------------------------------------------------------------------------
// ShmRegion
// ---------------------------------------------------------------------------
//
// Creator sequence:
//     ShmRegion r;
//     r.create("/lattice-engine-1000", bytes, kLayoutHash);
//     ...build the rings and state block inside r...
//     r.publishReady();          // release barrier: attachers may now look
//
// Attacher sequence:
//     ShmRegion r;
//     r.attach("/lattice-engine-1000", kLayoutHash, kShmVersion, /*timeoutMs*/2000);
//     ...map the same offsets...
//
// The two-step create/publishReady exists because an attacher that mapped a
// half-initialised region would read garbage ring indices. ready is the only
// synchronisation point in the whole protocol.
class ShmRegion {
public:
    ShmRegion() = default;
    ~ShmRegion() { close(); }
    ShmRegion(const ShmRegion&)            = delete;
    ShmRegion& operator=(const ShmRegion&) = delete;
    ShmRegion(ShmRegion&& o) noexcept { moveFrom(o); }
    ShmRegion& operator=(ShmRegion&& o) noexcept {
        if (this != &o) { close(); moveFrom(o); }
        return *this;
    }

    // Creator. `payloadBytes` is what you need past the header; the region is
    // rounded up to a page. Fails if a live process already owns the name.
    bool create(const char* name, size_t payloadBytes, u32 layoutHash, u32 version = kShmVersion) {
        close();
        if (!setName(name)) return false;

        const long pg = ::sysconf(_SC_PAGESIZE);
        const size_t page  = pg > 0 ? (size_t)pg : 4096;
        const size_t total = alignUp(kPayloadOffset + payloadBytes, page);

        int fd = ::shm_open(name_, O_CREAT | O_EXCL | O_RDWR, 0600);  // 0600: a session
        if (fd < 0 && errno == EEXIST) {                              // is nobody else's business
            // Left over from a crash? Reclaim it. Owned by a live process?
            // reapIfStale says no and we fail loudly rather than gatecrash.
            if (reapIfStale(name_)) fd = ::shm_open(name_, O_CREAT | O_EXCL | O_RDWR, 0600);
        }
        if (fd < 0) {
            setErr("shm_open(%s, O_CREAT|O_EXCL): %s", name_, std::strerror(errno));
            name_[0] = '\0';
            return false;
        }
        if (::ftruncate(fd, (off_t)total) != 0) {
            setErr("ftruncate(%s, %zu): %s", name_, total, std::strerror(errno));
            ::close(fd); ::shm_unlink(name_); name_[0] = '\0';
            return false;
        }
        void* p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);                     // the mapping keeps the object alive
        if (p == MAP_FAILED) {
            setErr("mmap(%s, %zu): %s", name_, total, std::strerror(errno));
            ::shm_unlink(name_); name_[0] = '\0';
            return false;
        }

        base_   = p;
        bytes_  = total;
        unlink_ = true;                  // creator owns the name

        // The kernel zero-fills a fresh shm object, so the payload needs no
        // clearing here — which matters once the payload is a multi-megabyte
        // sample pool. Only the header is written explicitly.
        std::memset(base_, 0, kPayloadOffset);
        ShmHeader* h = new (base_) ShmHeader();
        h->magic             = kShmMagic;
        h->version           = version;
        h->headerBytes       = (u32)kPayloadOffset;
        h->totalBytes        = (u64)total;
        h->layoutHash        = layoutHash;
        h->creatorPid        = (i32)::getpid();
        h->creatorStartTicks = procStartTicks((i32)::getpid());
        h->ready.store(0, std::memory_order_relaxed);
        err_[0] = '\0';
        return true;
    }

    // Creator: everything in the payload is initialised, attachers may proceed.
    void publishReady() {
        if (base_) header()->ready.store(1, std::memory_order_release);
    }

    // Attacher. Retries while the region is absent or not yet ready, up to
    // timeoutMs (0 = single attempt). A region that exists and is ready but
    // disagrees about magic/version/layout/size fails immediately — retrying a
    // mismatch would just spin until the timeout and report the wrong reason.
    bool attach(const char* name, u32 layoutHash, u32 version = kShmVersion, int timeoutMs = 0) {
        close();
        if (!setName(name)) return false;

        const u64 deadline = monotonicNs() + (u64)(timeoutMs > 0 ? timeoutMs : 0) * 1000000ull;
        for (;;) {
            int fd = ::shm_open(name_, O_RDWR, 0);
            if (fd >= 0) {
                struct stat st{};
                if (::fstat(fd, &st) == 0 && (size_t)st.st_size >= kPayloadOffset) {
                    const size_t total = (size_t)st.st_size;
                    void* p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    ::close(fd);
                    if (p == MAP_FAILED) {
                        setErr("mmap(%s, %zu): %s", name_, total, std::strerror(errno));
                        name_[0] = '\0';
                        return false;
                    }
                    ShmHeader* h = (ShmHeader*)p;
                    if (h->ready.load(std::memory_order_acquire) == 1) {
                        if (!validate(h, total, layoutHash, version)) {
                            ::munmap(p, total);
                            // Keep name_ so error() reads sensibly; the region
                            // is not ours and must not be unlinked.
                            return false;
                        }
                        base_   = p;
                        bytes_  = total;
                        unlink_ = false;                 // attachers never unlink
                        h->attached.fetch_add(1, std::memory_order_relaxed);
                        err_[0] = '\0';
                        return true;
                    }
                    ::munmap(p, total);                  // creator still filling it in
                } else {
                    ::close(fd);                         // created but not yet sized
                }
            }
            if (monotonicNs() >= deadline) {
                setErr("attach(%s): timed out after %d ms waiting for a ready region",
                       name_, timeoutMs);
                name_[0] = '\0';
                return false;
            }
            timespec ts{0, 200000};                      // 0.2 ms; startup path only
            nanosleep(&ts, nullptr);
        }
    }

    void close() {
        if (base_) { ::munmap(base_, bytes_); base_ = nullptr; }
        if (unlink_ && name_[0]) ::shm_unlink(name_);
        unlink_  = false;
        bytes_   = 0;
        name_[0] = '\0';
    }

    // Stale-region cleanup hook. Returns true if `name` named an orphan and it
    // was removed. Safe to call at daemon startup, from a crash handler, or
    // from a "lattice --clean-shm" maintenance path.
    static bool reapIfStale(const char* name) {
        char nm[kNameMax];
        if (!normalize(name, nm, sizeof nm)) return false;
        int fd = ::shm_open(nm, O_RDONLY, 0);
        if (fd < 0) return false;                        // nothing there
        struct stat st{};
        if (::fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(ShmHeader)) {
            // Created but never sized: the creator died between shm_open and
            // ftruncate. Nobody can be using this.
            ::close(fd);
            ::shm_unlink(nm);
            return true;
        }
        void* p = ::mmap(nullptr, sizeof(ShmHeader), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return false;
        const ShmHeader* h = (const ShmHeader*)p;
        // A torn or foreign header reads as garbage; a garbage pid is
        // overwhelmingly likely to be dead, and if it is not we keep the
        // region. Either way we never remove a region a live peer owns.
        const bool stale = h->magic != kShmMagic ||
                           !processAlive(h->creatorPid, h->creatorStartTicks);
        ::munmap(p, sizeof(ShmHeader));
        if (stale) ::shm_unlink(nm);
        return stale;
    }

    // Unlink without regard for liveness. Only for a process that knows it is
    // the owner (e.g. a fatal-signal handler in the daemon).
    static void forceUnlink(const char* name) {
        char nm[kNameMax];
        if (normalize(name, nm, sizeof nm)) ::shm_unlink(nm);
    }

    bool        valid() const  { return base_ != nullptr; }
    const char* name() const   { return name_; }
    const char* error() const  { return err_; }
    bool        isCreator() const { return unlink_; }
    size_t      totalBytes() const { return bytes_; }
    size_t      payloadBytes() const { return bytes_ ? bytes_ - kPayloadOffset : 0; }

    ShmHeader*       header()       { return (ShmHeader*)base_; }
    const ShmHeader* header() const { return (const ShmHeader*)base_; }
    u8*              payload()      { return (u8*)base_ + kPayloadOffset; }

    // Bounds- and alignment-checked view of an object placed at `off` in the
    // payload. Returns null rather than trapping, so a layout mistake surfaces
    // as a startup failure instead of a SIGSEGV on the audio thread.
    template <typename T>
    T* at(size_t off) {
        if (!base_) return nullptr;
        if (off % alignof(T) != 0) return nullptr;
        if (off > payloadBytes() || sizeof(T) > payloadBytes() - off) return nullptr;
        return (T*)(payload() + off);
    }

private:
    static constexpr size_t kNameMax = 96;   // POSIX shm names are short

    void moveFrom(ShmRegion& o) {
        base_ = o.base_; bytes_ = o.bytes_; unlink_ = o.unlink_;
        std::memcpy(name_, o.name_, sizeof name_);
        std::memcpy(err_,  o.err_,  sizeof err_);
        o.base_ = nullptr; o.bytes_ = 0; o.unlink_ = false; o.name_[0] = '\0';
    }

    // POSIX requires a leading slash and no others; accept both spellings from
    // callers so config files can say "lattice-engine" or "/lattice-engine".
    static bool normalize(const char* name, char* out, size_t cap) {
        if (!name || !*name) return false;
        const char* body = (*name == '/') ? name + 1 : name;
        if (!*body || std::strchr(body, '/')) return false;
        if (std::strlen(body) + 2 > cap) return false;
        out[0] = '/';
        std::strcpy(out + 1, body);
        return true;
    }
    bool setName(const char* name) {
        if (!normalize(name, name_, sizeof name_)) {
            setErr("invalid shm name '%s' (need a single path component)", name ? name : "(null)");
            name_[0] = '\0';
            return false;
        }
        return true;
    }

    bool validate(const ShmHeader* h, size_t total, u32 layoutHash, u32 version) {
        if (h->magic != kShmMagic) {
            setErr("%s: bad magic 0x%016llx (not a Lattice region)",
                   name_, (unsigned long long)h->magic);
            return false;
        }
        if (h->version != version) {
            setErr("%s: protocol version mismatch (region %u, expected %u)",
                   name_, h->version, version);
            return false;
        }
        if (h->layoutHash != layoutHash) {
            setErr("%s: layout mismatch (region 0x%08x, expected 0x%08x)",
                   name_, h->layoutHash, layoutHash);
            return false;
        }
        if (h->headerBytes != (u32)kPayloadOffset || h->totalBytes != (u64)total) {
            setErr("%s: size mismatch (header %u/%llu, mapped %zu)",
                   name_, h->headerBytes, (unsigned long long)h->totalBytes, total);
            return false;
        }
        return true;
    }

    void setErr(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(err_, sizeof err_, fmt, ap);
        va_end(ap);
    }

    void*  base_   = nullptr;
    size_t bytes_  = 0;
    bool   unlink_ = false;
    char   name_[kNameMax] = {};
    char   err_[192]       = {};
};

// ---------------------------------------------------------------------------
// ShmSpscRing
// ---------------------------------------------------------------------------
//
// Same contract as lat::Ring: one producer, one consumer, no blocking, no
// allocation, capacity N-1 (one slot is burned so full and empty are
// distinguishable). The differences are all consequences of living in shared
// memory:
//
//   * no default member initialisers and no constructor, so a mapped region can
//     be adopted by an attacher without running anything;
//   * T must be trivially copyable and pointer-free — an address is meaningless
//     in the peer's address space (see docs/PROCESS-SPLIT.md on the two places
//     the current protocol smuggles pointers);
//   * indices are masked on load. In-process we trust the peer; across a
//     process boundary a crashed or wild peer can leave an out-of-range index
//     in shared memory, and masking turns "read past the end of the mapping"
//     into "read the wrong slot". Costs one AND per operation.
//
// WHY THERE IS NO DOORBELL
// ------------------------
// No eventfd, no futex, no signal. The architecture being split is already
// poll-based on both ends: the GUI drains events and reads the atomics block
// once per rendered frame, and the engine drains commands once per audio block
// at the top of process(). Neither side ever waits on the other, so a wakeup
// primitive would add a syscall per message and change nothing about latency —
// worst case a command sits for one audio block, exactly as it does today.
// Adding a doorbell would also drag a blocking wait into the audio callback,
// which is the one thing that must never happen.
//
// The future case is a low-power idle mode: GUI hidden or minimised, transport
// stopped, nothing to poll. Then a per-ring eventfd (write 1 on push into an
// empty ring, poll() on the consumer) or a futex on the write index lets the
// GUI sleep indefinitely instead of waking at frame rate. That is a strict
// addition — the flags would live in a reserved header field and both sides
// keep working if only one supports it — so it is deliberately out of scope
// here. The engine side must stay poll-only regardless.
template <typename T, u32 N>
class ShmSpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two >= 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "shared-memory messages must be trivially copyable");
    static_assert(std::atomic<u32>::is_always_lock_free,
                  "ring indices must be lock-free: a lock table is per-process "
                  "and would not synchronise anything across the boundary");
public:
    // Creator side: adopt the memory at `off` and reset it. Must run before
    // ShmRegion::publishReady(). Returns null if the offset does not fit.
    static ShmSpscRing* createAt(ShmRegion& r, size_t off) {
        ShmSpscRing* p = r.at<ShmSpscRing>(off);
        if (p) p->init();
        return p;
    }
    // Attacher side: adopt the memory, touching nothing.
    static ShmSpscRing* attachAt(ShmRegion& r, size_t off) { return r.at<ShmSpscRing>(off); }

    void init() {
        w_.store(0, std::memory_order_relaxed);
        r_.store(0, std::memory_order_relaxed);
    }

    // Producer side.
    bool push(const T& v) {
        const u32 w    = w_.load(std::memory_order_relaxed) & kMask;
        const u32 next = (w + 1) & kMask;
        if (next == (r_.load(std::memory_order_acquire) & kMask)) return false;  // full
        buf_[w] = v;
        w_.store(next, std::memory_order_release);
        return true;
    }
    // Consumer side.
    bool pop(T& out) {
        const u32 r = r_.load(std::memory_order_relaxed) & kMask;
        if (r == (w_.load(std::memory_order_acquire) & kMask)) return false;     // empty
        out = buf_[r];
        r_.store((r + 1) & kMask, std::memory_order_release);
        return true;
    }
    bool empty() const {
        return (r_.load(std::memory_order_acquire) & kMask) ==
               (w_.load(std::memory_order_acquire) & kMask);
    }
    // Approximate; the peer moves under you. Fine for meters and diagnostics.
    u32 size() const {
        const u32 w = w_.load(std::memory_order_acquire) & kMask;
        const u32 r = r_.load(std::memory_order_acquire) & kMask;
        return (w - r) & kMask;
    }

    static constexpr u32    capacity() { return N - 1; }
    static constexpr size_t bytes()    { return sizeof(ShmSpscRing); }

private:
    static constexpr u32 kMask = N - 1;

    alignas(kCacheLine) std::atomic<u32> w_;
    alignas(kCacheLine) std::atomic<u32> r_;
    alignas(kCacheLine) T buf_[N];
};

// ---------------------------------------------------------------------------
// SharedStateT — the polled scalar block
// ---------------------------------------------------------------------------
//
// The cross-process form of the std::atomic members on Engine. The engine
// writes it once per block from publish(); the GUI reads it once per frame.
//
// Relaxed on both sides, exactly as today. There is no seqlock and no
// generation gate around the reads because no two fields have an invariant
// between them: a meter one block stale next to a playhead one block fresh is
// indistinguishable from the ~11 ms of latency the display already has. The
// generation counter below is for *liveness*, not consistency — a reader that
// ever needs a coherent multi-field snapshot (say, a screenshot-accurate
// mixer capture) should get a dedicated seqlock rather than turning every
// meter store into a fence on the audio thread.
//
// Types are fixed-width and there is no std::atomic<bool>: bool's size is
// implementation-defined and this struct is parsed by two separately compiled
// binaries.
template <int NTracks>
struct SharedStateT {
    // --- liveness ------------------------------------------------------
    std::atomic<u64> generation;    // +1 per publish; frozen => engine wedged
    std::atomic<u64> heartbeatNs;   // CLOCK_MONOTONIC at last publish
    std::atomic<i32> enginePid;
    std::atomic<u32> engineState;   // EngineState below
    std::atomic<u64> blocksRendered;
    std::atomic<u64> xruns;

    // --- transport (mirrors Engine::publish) ---------------------------
    std::atomic<f64> beat;
    std::atomic<f64> tempo;
    std::atomic<u32> playing;       // 0/1 — not atomic<bool>, see above
    std::atomic<f32> cpu;
    std::atomic<f64> sampleRate;
    std::atomic<u32> blockSize;

    // --- per-track ------------------------------------------------------
    std::atomic<i32> slotState[NTracks];
    std::atomic<i32> activeSlot[NTracks];
    std::atomic<i32> pendingSlot[NTracks];
    std::atomic<f64> clipPhase[NTracks];
    std::atomic<f32> meterL[NTracks];
    std::atomic<f32> meterR[NTracks];
    std::atomic<f32> masterMeterL;
    std::atomic<f32> masterMeterR;

    // Recording, mirroring Engine::recState/recSlotIdx: 0 idle, 1 queued,
    // 2 recording (a take with a stop already queued still reads 2), and the
    // slot the take is aimed at, -1 when idle. Here because the daemon mirrors
    // the *whole* published block or the GUI would have to keep a second,
    // in-process source of truth for two of its indicators.
    std::atomic<i32> recState[NTracks];
    std::atomic<i32> recSlotIdx[NTracks];

    enum : u32 { StateBooting = 0, StateRunning = 1, StateDraining = 2, StateStopping = 3 };

    // Creator only, before publishReady(). Defaults match Engine's, including
    // the sentinels: activeSlot -1 = nothing playing, pendingSlot -2 = nothing
    // queued (-1 means a queued *stop*).
    void init(f64 sr = 48000.0, u32 block = 0) {
        generation.store(0, std::memory_order_relaxed);
        heartbeatNs.store(monotonicNs(), std::memory_order_relaxed);
        enginePid.store((i32)::getpid(), std::memory_order_relaxed);
        engineState.store(StateBooting, std::memory_order_relaxed);
        blocksRendered.store(0, std::memory_order_relaxed);
        xruns.store(0, std::memory_order_relaxed);
        beat.store(0.0, std::memory_order_relaxed);
        tempo.store(120.0, std::memory_order_relaxed);
        playing.store(0, std::memory_order_relaxed);
        cpu.store(0.f, std::memory_order_relaxed);
        sampleRate.store(sr, std::memory_order_relaxed);
        blockSize.store(block, std::memory_order_relaxed);
        for (int i = 0; i < NTracks; ++i) {
            slotState[i].store(0, std::memory_order_relaxed);
            activeSlot[i].store(-1, std::memory_order_relaxed);
            pendingSlot[i].store(-2, std::memory_order_relaxed);
            clipPhase[i].store(0.0, std::memory_order_relaxed);
            meterL[i].store(0.f, std::memory_order_relaxed);
            meterR[i].store(0.f, std::memory_order_relaxed);
            recState[i].store(0, std::memory_order_relaxed);
            recSlotIdx[i].store(-1, std::memory_order_relaxed);
        }
        masterMeterL.store(0.f, std::memory_order_relaxed);
        masterMeterR.store(0.f, std::memory_order_relaxed);
    }

    // Engine, last statement of publish(): stamps the block so the GUI can tell
    // "engine is idle" from "engine is dead".
    void stampHeartbeat() {
        blocksRendered.fetch_add(1, std::memory_order_relaxed);
        heartbeatNs.store(monotonicNs(), std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_relaxed);
    }

    // GUI. True if the engine has not published inside `toleranceNs`. The
    // tolerance must be generous — several hundred ms — because a laptop
    // resuming from suspend or a stalled JACK server is not a dead engine, and
    // respawning under a live one is the worst possible outcome.
    bool stale(u64 toleranceNs) const {
        const u64 last = heartbeatNs.load(std::memory_order_relaxed);
        const u64 now  = monotonicNs();
        return now > last && (now - last) > toleranceNs;
    }
};

using SharedState = SharedStateT<kMaxTracks>;

static_assert(std::atomic<f64>::is_always_lock_free, "f64 state must be lock-free");
static_assert(std::atomic<f32>::is_always_lock_free, "f32 state must be lock-free");
static_assert(std::atomic<i32>::is_always_lock_free, "i32 state must be lock-free");

} // namespace lat::ipc
