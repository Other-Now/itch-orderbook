// Standalone steady-state churn benchmark (no Google Benchmark needed), so the
// README's cross-platform numbers reproduce anywhere with just a C++20 compiler:
//   g++ -O3 -std=c++20 -Iinclude bench/bench_churn.cpp -o bench_churn
// Each op adds an order and deletes an old one, holding ~50 price levels/side --
// the realistic regime -- for each of the three book models.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "itch/book/mbo_book.hpp"
#include "itch/book/order_book.hpp"

using namespace itch;
using Clock = std::chrono::steady_clock;

namespace {

std::int64_t g_sink = 0;

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
    for (int i = 0; i < 4096; ++i) {  // preload so deletes always hit a live order
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
    std::printf("  %-16s %7.1f ns   (cv %4.1f%%)\n", name, median,
                std::sqrt(var) / mean * 100.0);
}

}  // namespace

int main() {
    const long ops = 1'000'000;
    const int reps = 11;
    std::printf("steady-state add+delete churn (~50 levels/side), median of %d:\n", reps);
    report("MBP map", book_churn_ns<OrderBookSet>, ops, reps);
    report("MBP flat", book_churn_ns<FlatOrderBookSet>, ops, reps);
    report("MBO price-time", book_churn_ns<MboOrderBookSet>, ops, reps);
    std::fprintf(stderr, "%s", g_sink == 42 ? "x" : "");
    return 0;
}
