// Lock-free single-producer / single-consumer ring buffer for POD messages.
// One instance carries GUI -> audio commands, another carries audio -> GUI
// events. Neither side ever blocks or allocates.
#pragma once
#include <atomic>
#include "common.h"

namespace lat {

template <typename T, int N>
class Ring {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");
public:
    // Producer side.
    bool push(const T& v) {
        const u32 w = w_.load(std::memory_order_relaxed);
        const u32 next = (w + 1) & (N - 1);
        if (next == r_.load(std::memory_order_acquire)) return false; // full
        buf_[w] = v;
        w_.store(next, std::memory_order_release);
        return true;
    }
    // Consumer side.
    bool pop(T& out) {
        const u32 r = r_.load(std::memory_order_relaxed);
        if (r == w_.load(std::memory_order_acquire)) return false; // empty
        out = buf_[r];
        r_.store((r + 1) & (N - 1), std::memory_order_release);
        return true;
    }
    bool empty() const {
        return r_.load(std::memory_order_acquire) == w_.load(std::memory_order_acquire);
    }
private:
    T buf_[N]{};
    std::atomic<u32> w_{0};
    std::atomic<u32> r_{0};
};

} // namespace lat
