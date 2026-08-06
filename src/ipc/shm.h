// NxTakt IPC — POSIX shared memory transport for the engine/GUI process split.
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
// fine: NxTakt is a native Linux DAW. The Windows port in backend_win32.cpp
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
//
// Left as "LTC_" through the NxTakt rename (as were kPoolMagic and
// kPoolBlockMagic in pool.h). It is eight raw bytes chosen to be improbable,
// not a name anyone reads: it is never printed except as hex, never parsed,
// and never written to disk. Changing it would buy nothing and would only make
// the constant disagree with every comment and hexdump that quotes it.
inline constexpr u64 kShmMagic = 0x4C54435F53484D31ull;

// Bump on ANY change to ShmHeader, ShmSpscRing or SharedStateT layout, or to
// the meaning of a field. A mismatched attacher must fail rather than
// misinterpret; that is the whole point of the field.
//
//   v2 — SharedStateT gained recState[]/recSlotIdx[] so the block mirrors
//        Engine's published atomics exactly (wave 2, the engine daemon).
//   v3 — SharedStateT gained arrOverride and journalDropped (wave 8g, the
//        arrangement across the boundary: ARRANGEMENT.md §9.1). The block still
//        mirrors Engine's published atomics exactly, which is the rule that
//        decides what belongs here — and both of those are now among them.
//   v4 — SharedStateT gained returnMeterL/R[] and latencyFrames (wave 9 step 0,
//        docs/GUI-ON-DAEMON.md §1.2/§5). Both landed on Engine in wave 6 (the
//        return buses, plugin delay compensation) after this block was written
//        in wave 1, so they were the two published atomics the GUI reads every
//        frame that the wire could not carry — the first concrete regression a
//        daemon-backed GUI would have shipped with. Same rule as v3: they are
//        Engine's own published atomics, so they belong here.
//   v5 — no field moved: `generation` changed MEANING (wave 9 step 2). It was a
//        liveness counter bumped once, last, per publish; it is now a seqlock
//        sequence bumped twice — odd while a publish is in flight, even when the
//        block is quiescent — so a reader can take a genuinely coherent
//        multi-field snapshot. A v4 writer bumps by one and would leave the
//        parity wrong half the time, which a v5 reader would read as "a publish
//        is permanently in flight". That is exactly the disagreement a version
//        number exists to refuse, so it is refused.
//   v6 — SharedStateT gained posBar/posBeat/posSixteenth and posSigNum/posSigDen
//        (wave 9, the time-signature transport readout). Engine's own published
//        atomics again, and the same rule as v3/v4 decides it: this block mirrors
//        what Engine publishes, and it publishes these. The alternative — a
//        client deriving bars from `beat` and its own copy of the signature map —
//        is wrong precisely when a map was REFUSED, which is the one case where
//        being wrong is silent.
inline constexpr u32 kShmVersion = 6;

// How many return buses SharedStateT carries meters for. This is kMaxReturns
// from audio/engine.h, written out rather than included: the wire layer knows
// nothing about the engine (see the file header), and kMaxTracks is only
// reachable because it lives in core/common.h. control.h has both headers and
// static_asserts that the two numbers agree, so the duplication cannot drift
// without failing a build.
inline constexpr int kShmReturns = 4;

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
//     r.create("/nxtakt-engine-1000", bytes, kLayoutHash);
//     ...build the rings and state block inside r...
//     r.publishReady();          // release barrier: attachers may now look
//
// Attacher sequence:
//     ShmRegion r;
//     r.attach("/nxtakt-engine-1000", kLayoutHash, kShmVersion, /*timeoutMs*/2000);
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
    //
    // `seal` asks the kernel to make the object un-shrinkable before the fd is
    // dropped (§3.5: a peer that ftruncates a mapped region smaller hands the
    // audio thread a SIGBUS on its next read). It is best-effort by design —
    // see trySeal() — and sealed() reports what actually happened, because a
    // pool that could not be sealed is still a usable pool, just one whose
    // shrink-safety rests on nobody having a writable fd rather than on the
    // kernel.
    bool create(const char* name, size_t payloadBytes, u32 layoutHash,
                u32 version = kShmVersion, bool seal = false) {
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
        sealed_ = seal && trySeal(fd);
        void* p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);                     // the mapping keeps the object alive
        if (p == MAP_FAILED) {
            setErr("mmap(%s, %zu): %s", name_, total, std::strerror(errno));
            ::shm_unlink(name_); name_[0] = '\0';
            return false;
        }

        base_     = p;
        bytes_    = total;
        unlink_   = true;                // creator owns the name
        readOnly_ = false;

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
    //
    // `readOnly` maps PROT_READ and opens O_RDONLY. It exists for the sample
    // pool (§3.5): the GUI owns the allocator and the engine only ever reads
    // block extents, so mapping it read-only in the daemon turns "the engine
    // must not write the pool" from a comment into a page permission. It also
    // means the attach counter cannot be bumped — the counter is informational
    // and a write, and a write is exactly what we just gave up.
    bool attach(const char* name, u32 layoutHash, u32 version = kShmVersion, int timeoutMs = 0,
                bool readOnly = false) {
        close();
        if (!setName(name)) return false;

        const u64 deadline = monotonicNs() + (u64)(timeoutMs > 0 ? timeoutMs : 0) * 1000000ull;
        for (;;) {
            int fd = ::shm_open(name_, readOnly ? O_RDONLY : O_RDWR, 0);
            if (fd >= 0) {
                struct stat st{};
                if (::fstat(fd, &st) == 0 && (size_t)st.st_size >= kPayloadOffset) {
                    const size_t total = (size_t)st.st_size;
                    void* p = ::mmap(nullptr, total,
                                     readOnly ? PROT_READ : (PROT_READ | PROT_WRITE),
                                     MAP_SHARED, fd, 0);
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
                        base_     = p;
                        bytes_    = total;
                        unlink_   = false;               // attachers never unlink
                        readOnly_ = readOnly;
                        if (!readOnly) h->attached.fetch_add(1, std::memory_order_relaxed);
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
        unlink_   = false;
        readOnly_ = false;
        sealed_   = false;
        bytes_    = 0;
        name_[0]  = '\0';
    }

    // Detach without unlinking, whatever this handle's role is. The one place
    // a creator wants this is a hand-off: the session region must outlive the
    // process that made it (§4.3), so "I am going away but the region is not"
    // has to be expressible. Ordinary shutdown still goes through close().
    void release() {
        unlink_ = false;
        close();
    }

    // Best-effort shrink protection. Sealing is a memfd feature: a plain
    // shm_open() object lives on tmpfs but its inode is not created sealable,
    // so F_ADD_SEALS answers EINVAL and there is nothing to be done about it
    // short of the memfd + SCM_RIGHTS path §3.2 needs a socket for. We ask
    // anyway, because the answer is free and it becomes yes the moment the fd
    // arrives from memfd_create() instead — and because a silent "we meant to
    // seal this" is how the SIGBUS in §5 gets shipped.
    static bool trySeal(int fd) {
#if defined(F_ADD_SEALS) && defined(F_SEAL_SHRINK)
        return ::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0;
#else
        (void)fd;
        return false;
#endif
    }

    // Stale-region cleanup hook. Returns true if `name` named an orphan and it
    // was removed. Safe to call at daemon startup, from a crash handler, or
    // from a "nxtakt --clean-shm" maintenance path.
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
    bool        isReadOnly() const { return readOnly_; }

    // Re-stamp the liveness identity to the calling process. A region that is
    // designed to outlive its creator and be adopted by a replacement (the
    // sample pool, §4.3) keys reapIfStale() on ShmHeader::creatorPid — but that
    // still named the ORIGINAL, now-dead creator, so a live adopted region read
    // as stale and could be unlinked out from under its new owner (F8b). An
    // adopter calls this after a successful writable attach() so the liveness
    // key follows ownership. No-op on a read-only mapping (the daemon, which
    // must not write the pool) and before any mapping exists.
    void adoptCreator() {
        if (!base_ || readOnly_) return;
        ShmHeader* h = header();
        h->creatorPid        = (i32)::getpid();
        h->creatorStartTicks = procStartTicks((i32)::getpid());
    }
    bool        sealed() const    { return sealed_; }
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
        readOnly_ = o.readOnly_; sealed_ = o.sealed_;
        std::memcpy(name_, o.name_, sizeof name_);
        std::memcpy(err_,  o.err_,  sizeof err_);
        o.base_ = nullptr; o.bytes_ = 0; o.unlink_ = false;
        o.readOnly_ = false; o.sealed_ = false; o.name_[0] = '\0';
    }

    // POSIX requires a leading slash and no others; accept both spellings from
    // callers so config files can say "nxtakt-engine" or "/nxtakt-engine".
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
            setErr("%s: bad magic 0x%016llx (not an NxTakt region)",
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

    void*  base_     = nullptr;
    size_t bytes_    = 0;
    bool   unlink_   = false;
    bool   readOnly_ = false;
    bool   sealed_   = false;
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
// Every field is a relaxed atomic and may still be read on its own — a meter one
// block stale beside a playhead one block fresh is indistinguishable from the
// ~11 ms of latency the display already has, and that is all most readers want.
//
// THE BLOCK IS ALSO A SEQLOCK (v5)
// --------------------------------
// What v4 could not offer is a *coherent multi-field snapshot*, and the GUI
// turns out to need one: docs/GUI-ON-DAEMON.md §2.1 draws one clip slot from
// slotState, activeSlot, pendingSlot and clipPhase together, and a copy that
// straddles a publish can show a slot Playing with activeSlot == -1.
//
// `generation` is therefore a seqlock sequence rather than a plain counter:
// **odd while a publish is in flight, even when the block is quiescent**, bumped
// twice per publish instead of once (publishBegin/publishEnd below, readCoherent
// on the reading side). Liveness is unchanged — it still advances whenever the
// writer is alive and freezes when it wedges — so every reader that only asked
// "is this number moving?" keeps working.
//
// The parity is the point, and it is why §2.1's own recipe ("read generation,
// copy, re-read, retry on change") is not sufficient by itself: against a writer
// that bumps only at the END of a publish, a reader that samples entirely
// *inside* one publish sees the same generation either side of a copy it has
// already torn. One counter bumped twice costs the writer two relaxed increments
// and two fences per publish, on a non-realtime thread, and turns the retry loop
// into an actual proof.
//
// Types are fixed-width and there is no std::atomic<bool>: bool's size is
// implementation-defined and this struct is parsed by two separately compiled
// binaries.
template <int NTracks>
struct SharedStateT {
    // --- liveness and the seqlock sequence ------------------------------
    // +2 per publish, odd while one is in flight; frozen => engine wedged.
    std::atomic<u64> generation;
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
    // Engine::latencyFrames — the total plugin delay compensation the engine is
    // applying, which the status bar prints. An engine scalar like blockSize and
    // published for the same reason: the GUI cannot compute it, because it is
    // the engine that knows which chains are running and what each of them
    // reported.
    std::atomic<i32> latencyFrames;

    // The playhead as a musician reads it, ONE-BASED, plus the signature in
    // force AT THE PLAYHEAD. Engine's own published atomics (posBar/posBeat/
    // posSixteenth/posSigNum/posSigDen), recomputed once per block from the
    // signature map, which is why they belong on the wire rather than being
    // recomputed on the far side: the map lives in the engine, and a client that
    // divided `beat` by a single numerator would be wrong from the first
    // signature change on — and wrong DIFFERENTLY from the daemon, which is the
    // failure that matters. A refused signature map leaves the engine in 4/4
    // while the client's own copy of the set still says 7/8; carrying these is
    // what stops a GUI from confidently drawing the wrong bar number.
    std::atomic<i32> posBar, posBeat, posSixteenth;
    std::atomic<i32> posSigNum, posSigDen;

    // --- per-track ------------------------------------------------------
    std::atomic<i32> slotState[NTracks];
    std::atomic<i32> activeSlot[NTracks];
    std::atomic<i32> pendingSlot[NTracks];
    std::atomic<f64> clipPhase[NTracks];
    std::atomic<f32> meterL[NTracks];
    std::atomic<f32> meterR[NTracks];
    std::atomic<f32> masterMeterL;
    std::atomic<f32> masterMeterR;

    // --- the return buses ----------------------------------------------
    // Engine::returnMeterL/R. The mixer's A-D strips read these every frame
    // exactly as they read meterL/meterR, so leaving them off the wire would
    // have given a daemon-backed mixer four permanently dead meters.
    std::atomic<f32> returnMeterL[kShmReturns];
    std::atomic<f32> returnMeterR[kShmReturns];

    // Recording, mirroring Engine::recState/recSlotIdx: 0 idle, 1 queued,
    // 2 recording (a take with a stop already queued still reads 2), and the
    // slot the take is aimed at, -1 when idle. Here because the daemon mirrors
    // the *whole* published block or the GUI would have to keep a second,
    // in-process source of truth for two of its indicators.
    std::atomic<i32> recState[NTracks];
    std::atomic<i32> recSlotIdx[NTracks];

    // --- the arrangement (docs/ARRANGEMENT.md §4.2, §5.3) -------------------
    //
    // Bit i set == track i's arrangement lane is suspended because a session
    // clip was launched on it, until Cmd::BackToArrangement. ENGINE-owned and
    // set at the quantized launch the engine computes, which is precisely why
    // it has to be published rather than inferred: the GUI knows when it asked
    // for a launch, not which bar line the engine put it on.
    //
    // journalDropped is the ENGINE's refused-push count for the record journal.
    // It is mirrored here rather than left in the daemon's header because it is
    // one of Engine's published atomics and this block is what mirrors those;
    // the daemon's own second-hop drop count lives in ControlHeader beside it
    // (§9.6 — there are two hops, and a take has to be able to see both).
    std::atomic<u32> arrOverride;
    std::atomic<u32> journalDropped;

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
        latencyFrames.store(0, std::memory_order_relaxed);
        // One-based, matching Engine's own defaults. A zeroed bar number in a
        // one-based readout looks like a rendering quirk rather than a missing
        // publish, which is exactly the failure this whole block's init() exists
        // to make impossible.
        posBar.store(1, std::memory_order_relaxed);
        posBeat.store(1, std::memory_order_relaxed);
        posSixteenth.store(1, std::memory_order_relaxed);
        posSigNum.store(4, std::memory_order_relaxed);
        posSigDen.store(4, std::memory_order_relaxed);
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
        for (int i = 0; i < kShmReturns; ++i) {
            returnMeterL[i].store(0.f, std::memory_order_relaxed);
            returnMeterR[i].store(0.f, std::memory_order_relaxed);
        }
        arrOverride.store(0, std::memory_order_relaxed);
        journalDropped.store(0, std::memory_order_relaxed);
    }

    // -- the seqlock, writer side ----------------------------------------
    //
    // Bracket EVERY publish pass with these two. Between them the sequence is
    // odd and readCoherent() will refuse to hand a reader the block; outside
    // them it is even and a snapshot taken across a matching pair of even reads
    // provably did not straddle a publish.
    //
    // The fences are load-bearing and are not decoration on the relaxed
    // increments: without the first, the compiler or the CPU may sink the "I am
    // writing" store below the field stores it is supposed to announce; without
    // the second, it may hoist the "I have finished" store above them. Either
    // reordering hands a reader a torn copy that its own retry loop believes.
    void publishBegin() {
        generation.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }
    void publishEnd() {
        heartbeatNs.store(monotonicNs(), std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        generation.fetch_add(1, std::memory_order_relaxed);
    }

    // Engine, last statement of publish(): stamps the block so the GUI can tell
    // "engine is idle" from "engine is dead". A degenerate publish pass — it
    // brackets nothing but the block counter — so that a writer with a single
    // field to move still leaves the sequence even and still advances it.
    void stampHeartbeat() {
        publishBegin();
        blocksRendered.fetch_add(1, std::memory_order_relaxed);
        publishEnd();
    }

    // -- the seqlock, reader side ----------------------------------------
    //
    // Runs `copy` (which must read only relaxed loads out of this block, and
    // must be safe to run more than once) and returns true when the values it
    // produced provably came from one publish.
    //
    // Bounded, and that bound is a deliberate policy rather than an oversight:
    // a writer SIGSTOPped mid-publish leaves the sequence odd forever, and a UI
    // that spins until it goes even would hang on a debugger breakpoint in the
    // daemon. After `tries` attempts the last copy is handed back anyway, with
    // `false`, so the caller can draw *something* and say the engine is not
    // answering — which is what the lifecycle banner is for. Eight tries against
    // a 4 ms publish cadence is several milliseconds of headroom over a publish
    // that takes microseconds; reaching the bound means the writer is stopped,
    // not busy.
    template <class Fn>
    bool readCoherent(Fn&& copy, int tries = 8) const {
        for (int i = 0; i < tries; ++i) {
            const u64 g0 = generation.load(std::memory_order_acquire);
            if (!(g0 & 1ull)) {                         // no publish in flight
                copy();
                std::atomic_thread_fence(std::memory_order_acquire);
                if (generation.load(std::memory_order_relaxed) == g0) return true;
            }
            // Yield rather than spin. A publish is a few microseconds of stores
            // once every few milliseconds, so eight back-to-back reloads inside
            // one nanosecond would all land inside the same window and the retry
            // budget would be spent without ever giving the writer time to leave
            // it. Sleeping is what turns "retry" into "wait for the writer".
            timespec ts{0, 20000};                      // 20 us
            nanosleep(&ts, nullptr);
        }
        copy();
        return false;
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
