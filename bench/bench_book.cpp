#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <vector>

#include "itch/book/mbo_book.hpp"
#include "itch/book/order_book.hpp"

using namespace itch;

namespace {

// An Add Order body in a stable buffer, so its pointer can be handed to AddOrder.
// Only the fields the book reads are filled.
struct AddBuf {
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

}  // namespace

// Add N orders spread across price levels, same workload for every book set.
template <class Set>
static void BM_Add(benchmark::State& state) {
    std::mt19937_64 rng(1);
    // Penny ticks in a tight band around the inside, ~50 active levels per side.
    std::uniform_int_distribution<int> tick(-25, 25);
    auto price = [&](std::mt19937_64& r) { return 1000000 + 100 * tick(r); };
    for (auto _ : state) {
        Set set;
        std::uint64_t ref = 1;
        for (int i = 0; i < state.range(0); ++i) {
            AddBuf a(1, ref, (i & 1) ? 'B' : 'S', 100, price(rng));
            set.add_order(AddOrder{a.b});
            ++ref;
        }
        benchmark::DoNotOptimize(set.live_orders());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Add<OrderBookSet>)->Arg(1 << 16)->Name("Add/map");
BENCHMARK(BM_Add<FlatOrderBookSet>)->Arg(1 << 16)->Name("Add/flat");
BENCHMARK(BM_Add<MboOrderBookSet>)->Arg(1 << 16)->Name("Add/mbo");

// Steady-state churn: each iteration adds an order then deletes an old one, so
// the live set and level count stay roughly constant.
template <class Set>
static void BM_AddDeleteChurn(benchmark::State& state) {
    Set set;
    std::mt19937_64 rng(2);
    std::uniform_int_distribution<int> tick(-25, 25);
    auto price = [&](std::mt19937_64& r) { return 1000000 + 100 * tick(r); };
    std::uint64_t ref = 1;
    for (int i = 0; i < 4096; ++i) {  // preload so deletes always hit a live order
        AddBuf a(1, ref, (i & 1) ? 'B' : 'S', 100, price(rng));
        set.add_order(AddOrder{a.b});
        ++ref;
    }
    std::uint64_t del_ref = 1;
    for (auto _ : state) {
        AddBuf a(1, ref, (ref & 1) ? 'B' : 'S', 100, price(rng));
        set.add_order(AddOrder{a.b});
        set.del(del_ref);
        ++ref;
        ++del_ref;
        benchmark::DoNotOptimize(set.live_orders());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddDeleteChurn<OrderBookSet>)->Name("Churn/map");
BENCHMARK(BM_AddDeleteChurn<FlatOrderBookSet>)->Name("Churn/flat");
BENCHMARK(BM_AddDeleteChurn<MboOrderBookSet>)->Name("Churn/mbo");

BENCHMARK_MAIN();
