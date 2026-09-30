// Tests for RingBuffer. The drop accounting is what stands between a slow
// encoder and a silently incomplete recording, so it gets proven, not assumed.

#include "core/RingBuffer.h"

#include <atomic>
#include <cstdio>
#include <thread>

using namespace campy;

namespace {

int failures = 0;

void Check(bool cond, const char* what) {
    if (!cond) { std::printf("  FAIL: %s\n", what); ++failures; }
}

void TestFillAndDrain() {
    RingBuffer<int> rb(4);
    for (int i = 0; i < 4; ++i) Check(rb.Push(i), "push into space");

    Check(!rb.Push(99), "push into full buffer must fail");
    Check(rb.Stats().dropped == 1, "overflow is counted");
    Check(rb.Stats().peak == 4, "peak tracks high-water mark");

    for (int i = 0; i < 4; ++i) {
        auto v = rb.Pop(std::chrono::milliseconds(10));
        Check(v.has_value() && *v == i, "FIFO order preserved");
    }
    Check(!rb.Pop(std::chrono::milliseconds(1)).has_value(), "empty pop times out");
}

void TestCloseDrainsRatherThanDiscards() {
    // This is the Stop-button guarantee: closing must let queued frames still
    // reach the encoder, or pressing Stop would truncate the recording.
    RingBuffer<int> rb(8);
    for (int i = 0; i < 5; ++i) rb.Push(i);
    rb.Close();

    Check(!rb.Push(6), "push after close is refused");
    Check(!rb.IsDrained(), "not drained while items remain");

    int drained = 0;
    while (auto v = rb.Pop(std::chrono::milliseconds(10))) ++drained;
    Check(drained == 5, "every queued item survives Close()");
    Check(rb.IsDrained(), "drained once empty and closed");
}

void TestConcurrentProducerConsumer() {
    RingBuffer<int> rb(64);
    const int total = 20000;
    std::atomic<int> consumed{0};
    std::atomic<uint64_t> sum{0};

    std::thread consumer([&] {
        while (true) {
            auto v = rb.Pop(std::chrono::milliseconds(100));
            if (v) { sum += static_cast<uint64_t>(*v); ++consumed; }
            else if (rb.IsDrained()) break;
        }
    });

    uint64_t pushedSum = 0;
    uint64_t dropped = 0;
    for (int i = 0; i < total; ++i) {
        if (rb.Push(i)) pushedSum += static_cast<uint64_t>(i);
        else ++dropped;
    }
    rb.Close();
    consumer.join();

    const RingStats s = rb.Stats();
    Check(s.pushed + dropped == static_cast<uint64_t>(total),
          "every item is either pushed or counted as dropped");
    Check(s.dropped == dropped, "reported drops match observed drops");
    Check(consumed == static_cast<int>(s.pushed), "consumer sees every push");
    Check(sum == pushedSum, "no corruption across threads");
    std::printf("  (concurrent: %llu pushed, %llu dropped, peak %zu/%zu)\n",
                static_cast<unsigned long long>(s.pushed),
                static_cast<unsigned long long>(s.dropped),
                s.peak, s.capacity);
}

} // namespace

int main() {
    std::printf("RingBuffer tests\n");
    TestFillAndDrain();
    TestCloseDrainsRatherThanDiscards();
    TestConcurrentProducerConsumer();

    if (failures == 0) { std::printf("PASS\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", failures);
    return 1;
}
