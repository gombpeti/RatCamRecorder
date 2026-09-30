#pragma once

// Bounded producer/consumer queue between a camera grab thread and its encoder.
//
// Bounded is the whole point. campy uses `writeQueue = deque()`, which is
// unbounded: when the encoder falls behind, it consumes RAM until the machine
// dies rather than admitting it is behind. Here the capacity is fixed, an
// overflow drops the frame, and every drop is counted and surfaced. For
// behavioural work a gap nobody noticed is worse than a visible failure.
//
// A mutex is plenty at 30 Hz per camera; lock-free would add risk for no
// measurable gain at these rates.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace campy {

struct RingStats {
    uint64_t pushed = 0;
    uint64_t popped = 0;
    uint64_t dropped = 0;    // overflow: encoder could not keep up
    size_t   size = 0;       // current occupancy
    size_t   peak = 0;       // high-water mark, for sizing the buffer
    size_t   capacity = 0;
};

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : slots_(capacity), capacity_(capacity) {}

    /// Copying overload, for callers holding an lvalue.
    bool Push(const T& item) {
        T copy = item;
        return Push(std::move(copy));
    }

    /// Returns false if the buffer was full; the item is dropped and counted.
    /// Never blocks -- a grab thread must not stall waiting on the encoder,
    /// because a missed USB transfer window cannot be recovered.
    bool Push(T&& item) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (closed_) return false;
            if (size_ == capacity_) {
                ++stats_.dropped;
                return false;
            }
            slots_[tail_] = std::move(item);
            tail_ = (tail_ + 1) % capacity_;
            ++size_;
            ++stats_.pushed;
            if (size_ > stats_.peak) stats_.peak = size_;
        }
        cv_.notify_one();
        return true;
    }

    /// Blocks until an item is available, the timeout expires, or the buffer is
    /// closed and drained. An empty return with IsDrained() means "finished".
    std::optional<T> Pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_);
        if (!cv_.wait_for(lock, timeout, [this] { return size_ > 0 || closed_; }))
            return std::nullopt;
        if (size_ == 0) return std::nullopt;   // closed and empty

        T item = std::move(slots_[head_]);
        head_ = (head_ + 1) % capacity_;
        --size_;
        ++stats_.popped;
        return item;
    }

    /// Stop accepting pushes and wake any waiting consumer. Items already
    /// queued stay readable, so a Stop drains rather than discards -- this is
    /// what lets every buffered frame still reach the file.
    void Close() {
        {
            std::lock_guard<std::mutex> lock(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(m_);
        return closed_;
    }

    bool IsDrained() const {
        std::lock_guard<std::mutex> lock(m_);
        return closed_ && size_ == 0;
    }

    RingStats Stats() const {
        std::lock_guard<std::mutex> lock(m_);
        RingStats s = stats_;
        s.size = size_;
        s.capacity = capacity_;
        return s;
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<T> slots_;
    size_t capacity_ = 0;
    size_t head_ = 0, tail_ = 0, size_ = 0;
    bool closed_ = false;
    RingStats stats_;
};

} // namespace campy
