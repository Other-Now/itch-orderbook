// Standalone x86 benchmark (no Google Benchmark needed). Two parts:
//  1. Full-book steady-state churn for the three book models (map/flat/mbo) --
//     the number the README table reports.
//  2. Allocator-isolation: real MapLevels / FlatLevels vs a pool-backed
//     std::pmr::map (same tree + comparisons, only the node allocator differs),
//     under alloc-heavy churn and alloc-free stable regimes. If map and pmr::map
//     tie under churn, the system allocator was never the bottleneck.

#include <version>  // populate feature-test macros before we test them

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <random>
#include <vector>

#include "itch/book/mbo_book.hpp"
#include "itch/book/order_book.hpp"

#if defined(__cpp_lib_memory_resource)
#include <map>
#include <memory_resource>
#endif

using namespace itch;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kWindow = 50;
constexpr std::uint32_t kBig = 1'000'000;
constexpr std::uint32_t kDeep = 1'000'000'000u;  // stable-regime resting; never erased

std::int64_t g_sink = 0;

// Median + coefficient of variation over `reps` runs; first run discarded (warmup).
template <class F>
void report(const char* name, F fn, long ops, int reps) {
    std::vector<double> v;
    fn(ops);
    for (int r = 0; r < reps; ++r) v.push_back(fn(ops));
    std::sort(v.begin(), v.end());
    const double median = v[v.size() / 2];
    double mean = 0; for (double x : v) mean += x; mean /= v.size();
    double var = 0; for (double x : v) var += (x - mean) * (x - mean); var /= v.size();
    std::printf("  %-22s %8.1f ns   (cv %4.1f%%)\n", name, median,
                std::sqrt(var) / mean * 100.0);
}

// --- Part 1: full-book churn ------------------------------------------------

struct AddBuf {  // a 36-byte Add Order body; only book-read fields are set
    unsigned char b[36]{};
    AddBuf(std::uint16_t loc, std::uint64_t ref, char side, std::uint32_t shares,
           std::int32_t price) {
        b[0] = 'A';
        b[1] = loc >> 8; b[2] = loc & 0xff;
        for (int i = 0; i < 8; ++i) b[11 + i] = (ref >> ((7 - i) * 8)) & 0xff;
        b[19] = static_cast<unsigned char>(side);
        b[20] = shares >> 24; b[21] = shares >> 16; b[22] = shares >> 8; b[23] = shares;
        std::uint32_t p = static_cast<std::uint32_t>(price);
        b[32] = p >> 24; b[33] = p >> 16; b[34] = p >> 8; b[35] = p;
    }
};

template <class Set>
double book_churn_ns(long ops) {
    Set set;
    std::mt19937_64 rng(2);
    std::uniform_int_distribution<int> tick(-25, 25);
    auto price = [&](std::mt19937_64& r) { return 1000000 + 100 * tick(r); };
    std::uint64_t ref = 1;
    for (int i = 0; i < 4096; ++i) {
        AddBuf a(1, ref, (i & 1) ? 'B' : 'S', 100, price(rng));
        set.add_order(AddOrder{a.b}); ++ref;
    }
    std::uint64_t del_ref = 1;
    std::int64_t acc = 0;
    const auto t0 = Clock::now();
    for (long k = 0; k < ops; ++k) {
        AddBuf a(1, ref, (ref & 1) ? 'B' : 'S', 100, price(rng));
        set.add_order(AddOrder{a.b});
        set.del(del_ref);
        ++ref; ++del_ref;
        acc += static_cast<std::int64_t>(set.live_orders());
    }
    const auto t1 = Clock::now();
    g_sink += acc;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / ops;
}

// --- Part 2: level-container isolation --------------------------------------

template <class Levels>
double churn_ns(long ops) {
    Levels lv;
    for (int i = 0; i < kWindow; ++i) lv.add(i, kBig);
    std::int32_t next = kWindow, oldest = 0;
    std::int64_t acc = 0;
    const auto t0 = Clock::now();
    for (long k = 0; k < ops; ++k) {
        lv.add(next, kBig);
        lv.reduce(oldest, kBig);
        ++next; ++oldest;
        if (auto b = lv.best()) acc += *b;
    }
    const auto t1 = Clock::now();
    g_sink += acc;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / ops;
}

template <class Levels>
double stable_ns(long ops) {
    Levels lv;
    for (int i = 0; i < kWindow; ++i) lv.add(i, kDeep);
    std::int64_t acc = 0;
    const auto t0 = Clock::now();
    for (long k = 0; k < ops; ++k) {
        const std::int32_t p = static_cast<std::int32_t>(k % kWindow);
        lv.add(p, 10);
        lv.reduce(p, 10);
        if (auto b = lv.best()) acc += *b;
    }
    const auto t1 = Clock::now();
    g_sink += acc;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / ops;
}

#if defined(__cpp_lib_memory_resource)
template <class Compare>
class PmrMapLevels {
public:
    void add(std::int32_t price, std::uint32_t shares) { lv_[price] += shares; }
    void reduce(std::int32_t price, std::uint32_t shares) {
        auto it = lv_.find(price);
        if (it == lv_.end()) return;
        if (shares >= it->second) lv_.erase(it);
        else it->second -= shares;
    }
    std::optional<std::int32_t> best() const {
        if (lv_.empty()) return std::nullopt;
        return lv_.begin()->first;
    }
private:
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::map<std::int32_t, std::uint64_t, Compare> lv_{&pool_};
};
#endif

}  // namespace

int main() {
    using Cmp = std::less<std::int32_t>;
    const int reps = 11;

    std::printf("FULL-BOOK CHURN (add+delete, ~50 levels/side)\n");
    report("MBP map", book_churn_ns<OrderBookSet>, 1'000'000, reps);
    report("MBP flat", book_churn_ns<FlatOrderBookSet>, 1'000'000, reps);
    report("MBO price-time", book_churn_ns<MboOrderBookSet>, 1'000'000, reps);

    std::printf("\nLEVEL CHURN (insert+erase a level every op)\n");
    report("map (glibc malloc)", churn_ns<MapLevels<Cmp>>, 1'000'000, reps);
#if defined(__cpp_lib_memory_resource)
    report("pmr::map (pool)", churn_ns<PmrMapLevels<Cmp>>, 1'000'000, reps);
#else
    std::printf("  (pmr::map skipped: no <memory_resource> pool support)\n");
#endif
    report("flat vector", churn_ns<FlatLevels<Cmp>>, 1'000'000, reps);

    std::printf("\nLEVEL STABLE (touch existing levels only, no allocation)\n");
    report("map (glibc malloc)", stable_ns<MapLevels<Cmp>>, 5'000'000, reps);
#if defined(__cpp_lib_memory_resource)
    report("pmr::map (pool)", stable_ns<PmrMapLevels<Cmp>>, 5'000'000, reps);
#endif
    report("flat vector", stable_ns<FlatLevels<Cmp>>, 5'000'000, reps);

    std::fprintf(stderr, "%s", g_sink == 42 ? "x" : "");
    return 0;
}
