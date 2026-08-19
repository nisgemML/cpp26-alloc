// bench/bench.cpp — Latency and throughput benchmark.
// Real numbers on Intel(R) Xeon(R) @ 2.10GHz, GCC 14, -O3 -march=native.

#include "alloc/pool.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <immintrin.h>
#include <latch>
#include <print>
#include <thread>
#include <x86intrin.h>

struct alignas(64) OrderNode {
    std::uint64_t order_id; double price;
    std::int32_t qty; std::int32_t side; std::uint8_t pad[40];
};
static_assert(sizeof(OrderNode) == 64uz);

// ── RDTSC calibration ─────────────────────────────────────────────────────────
static double g_ns_per_cycle = 0.0;

static void calibrate_tsc() {
    auto t0 = std::chrono::steady_clock::now();
    std::uint64_t c0 = __rdtsc();
    // Busy-wait 50ms for accurate calibration.
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds{50}) {}
    std::uint64_t c1 = __rdtsc();
    auto t1 = std::chrono::steady_clock::now();
    double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());
    g_ns_per_cycle = ns / double(c1 - c0);
    std::print("TSC: {:.4f} ns/cycle  (CPU ~{:.0f} MHz)\n\n",
               g_ns_per_cycle, 1000.0/g_ns_per_cycle);
}

static double cycles_to_ns(std::uint64_t c) noexcept {
    return double(c) * g_ns_per_cycle;
}

// ── HDR histogram ─────────────────────────────────────────────────────────────
struct Histogram {
    static constexpr std::size_t N = 4096uz;
    std::array<std::uint64_t, N> buckets{};
    std::uint64_t count{0uz}, sum_ns{0uz}, max_ns{0uz};

    void record(std::uint64_t cycles) noexcept {
        std::uint64_t ns = std::uint64_t(cycles_to_ns(cycles));
        ++buckets[std::min(ns, N - 1uz)];
        ++count; sum_ns += ns;
        if (ns > max_ns) max_ns = ns;
    }

    [[nodiscard]] double percentile(double pct) const noexcept {
        std::uint64_t target = std::uint64_t(double(count) * pct / 100.0);
        std::uint64_t cumul = 0uz;
        for (std::size_t i = 0uz; i < N; ++i) {
            cumul += buckets[i];
            if (cumul >= target) return double(i);
        }
        return double(max_ns);
    }

    void print(std::string_view label) const {
        std::print("  {:<38} p50={:6.1f}  p90={:6.1f}  p99={:6.1f}"
                   "  p99.9={:7.1f}  max={:8.1f}  (ns)\n",
            label,
            percentile(50.0), percentile(90.0), percentile(99.0),
            percentile(99.9), double(max_ns));
    }
};

// ── Single-threaded latency benchmarks ────────────────────────────────────────

static constexpr int N_WARMUP  =   5'000;
static constexpr int N_SAMPLES = 500'000;

static void bench_slab_alloc() {
    alloc::SlabAllocator<OrderNode, 4096uz, 8uz> alloc;
    Histogram h;

    for (int i = 0; i < N_WARMUP; ++i) {
        auto r = alloc.alloc();
        if (r) (void)alloc.free(*r);
    }
    for (int i = 0; i < N_SAMPLES; ++i) {
        _mm_lfence();
        std::uint64_t t0 = __rdtsc();
        auto r = alloc.alloc();
        _mm_lfence();
        std::uint64_t t1 = __rdtsc();
        h.record(t1 - t0);
        if (r) (void)alloc.free(*r);
    }
    h.print("SlabAllocator::alloc()");
}

static void bench_slab_free() {
    constexpr int BATCH = 4096;
    alloc::SlabAllocator<OrderNode, BATCH, 2uz> alloc;
    Histogram h;

    for (int round = 0; round < N_SAMPLES / BATCH; ++round) {
        std::array<OrderNode*, BATCH> ptrs;
        for (auto& p : ptrs) { auto r = alloc.alloc(); p = r ? *r : nullptr; }
        for (auto* p : ptrs) {
            if (!p) continue;
            _mm_lfence();
            std::uint64_t t0 = __rdtsc();
            (void)alloc.free(p);
            _mm_lfence();
            std::uint64_t t1 = __rdtsc();
            h.record(t1 - t0);
        }
    }
    h.print("SlabAllocator::free()");
}

static void bench_malloc() {
    Histogram h;
    for (int i = 0; i < N_WARMUP; ++i) { auto* p = std::malloc(64); std::free(p); }
    for (int i = 0; i < N_SAMPLES; ++i) {
        _mm_lfence();
        std::uint64_t t0 = __rdtsc();
        void* p = std::malloc(64);
        _mm_lfence();
        std::uint64_t t1 = __rdtsc();
        h.record(t1 - t0);
        std::free(p);
    }
    h.print("::malloc(64)");
}

static void bench_new() {
    Histogram h;
    for (int i = 0; i < N_WARMUP; ++i) { delete new OrderNode{}; }
    for (int i = 0; i < N_SAMPLES; ++i) {
        _mm_lfence();
        std::uint64_t t0 = __rdtsc();
        auto* p = new OrderNode{};
        _mm_lfence();
        std::uint64_t t1 = __rdtsc();
        h.record(t1 - t0);
        delete p;
    }
    h.print("::new OrderNode");
}

// ── Multi-threaded throughput (std::latch for precise start) ──────────────────

static std::uint64_t bench_slab_mt(int n_threads, std::chrono::milliseconds dur) {
    alloc::SlabAllocator<OrderNode, 4096uz, 32uz> alloc;
    std::atomic<std::uint64_t> total{0uz};
    std::latch start{n_threads + 1};  // C++20 std::latch
    std::atomic<bool> stop{false};

    auto worker = [&] {
        start.arrive_and_wait();
        std::uint64_t ops = 0uz;
        while (!stop.load(std::memory_order_relaxed)) {
            auto r = alloc.alloc();
            if (r) { (void)alloc.free(*r); ++ops; }
        }
        total.fetch_add(ops, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int i = 0; i < n_threads; ++i) threads.emplace_back(worker);
    start.arrive_and_wait();  // release all workers simultaneously
    std::this_thread::sleep_for(dur);
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();
    return total.load();
}

static std::uint64_t bench_malloc_mt(int n_threads, std::chrono::milliseconds dur) {
    std::atomic<std::uint64_t> total{0uz};
    std::latch start{n_threads + 1};
    std::atomic<bool> stop{false};

    auto worker = [&] {
        start.arrive_and_wait();
        std::uint64_t ops = 0uz;
        while (!stop.load(std::memory_order_relaxed)) {
            void* p = std::malloc(sizeof(OrderNode));
            if (p) { std::free(p); ++ops; }
        }
        total.fetch_add(ops, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int i = 0; i < n_threads; ++i) threads.emplace_back(worker);
    start.arrive_and_wait();
    std::this_thread::sleep_for(dur);
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();
    return total.load();
}

int main() {
    std::print("cpp26-alloc benchmark\n");
    std::print("═══════════════════════════════════════════════════════════════\n");
    calibrate_tsc();

    std::print("Single-threaded latency ({} samples)\n", N_SAMPLES);
    std::print("───────────────────────────────────────────────────────────────\n");
    bench_slab_alloc();
    bench_slab_free();
    bench_malloc();
    bench_new();

    std::print("\nMulti-threaded throughput (500ms window, alloc+free pairs)\n");
    std::print("───────────────────────────────────────────────────────────────\n");
    std::print("  {:<10} {:>18} {:>18} {:>10}\n",
               "Threads", "SlabAlloc (M/s)", "malloc (M/s)", "Speedup");

    auto dur = std::chrono::milliseconds{500};
    for (int n : {1, 2, 4}) {
        auto slab = bench_slab_mt(n, dur);
        auto mal  = bench_malloc_mt(n, dur);
        double secs   = double(dur.count()) / 1000.0;
        double slab_m = double(slab) / 1e6 / secs;
        double mal_m  = double(mal)  / 1e6 / secs;
        std::print("  {:<10} {:>18.2f} {:>18.2f} {:>9.2f}x\n",
                   n, slab_m, mal_m, slab_m / mal_m);
    }

    std::print("\n═══════════════════════════════════════════════════════════════\n");
    std::print("Note: container environment (shared Xeon). On an isolated core\n");
    std::print("(isolcpus + SCHED_FIFO), expect p50 alloc 4-8ns vs malloc 8-15ns.\n");
    std::print("See BENCHMARK_RESULTS.md for full analysis.\n");
    return 0;
}
