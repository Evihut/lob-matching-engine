// Google Benchmark micro-benchmarks, each run against both book implementations.

#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"

using namespace lob;

namespace {

constexpr BookConfig kCfg{1, 20'000, 1 << 16};
constexpr Price kMid = 10'000;

// Pre-populate `levels` price levels per side with `per_level` orders each.
template <class Book>
OrderId seed_book(Book& book, int levels, int per_level) {
    OrderId id = 1;
    for (int l = 1; l <= levels; ++l)
        for (int k = 0; k < per_level; ++k) {
            book.add_limit(id++, Side::Buy, kMid - l, 100);
            book.add_limit(id++, Side::Sell, kMid + l, 100);
        }
    return id;
}

// Add a passive order then cancel it: the dominant message pair in real feeds.
template <class Book>
void BM_AddCancel(benchmark::State& state) {
    Book book(kCfg);
    OrderId id = seed_book(book, 50, 10);
    std::mt19937_64 rng(1);
    for (auto _ : state) {
        const Price px = kMid - 1 - static_cast<Price>(rng() % 50);
        book.add_limit(id, Side::Buy, px, 100);
        book.cancel(id);
        ++id;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// Marketable order that fills exactly one resting order, which is then replaced
// so the book stays in steady state.
template <class Book>
void BM_MatchOneOrder(benchmark::State& state) {
    Book book(kCfg);
    OrderId id = seed_book(book, 50, 10);
    for (auto _ : state) {
        book.add_market(id++, Side::Buy, 100);
        book.add_limit(id++, Side::Sell, kMid + 1, 100);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// Market order that sweeps `range(0)` full price levels, followed by the
// passive orders that refill them (timing both avoids PauseTiming overhead,
// which is larger than the work being measured). Items = messages processed.
template <class Book>
void BM_SweepRefill(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    Book book(kCfg);
    OrderId id = seed_book(book, depth, 4);
    for (auto _ : state) {
        book.add_market(id++, Side::Buy, 400 * depth);
        for (int l = 1; l <= depth; ++l)
            for (int k = 0; k < 4; ++k) book.add_limit(id++, Side::Sell, kMid + l, 100);
    }
    state.SetItemsProcessed(state.iterations() * (1 + 4 * depth));
}

// Cancel a random order out of a crowded book (40k resting orders across 1000
// levels), then add a replacement at a random level so depth stays constant.
// Exercises id lookup and mid-queue unlink with a cold-ish working set.
template <class Book>
void BM_CancelReplaceDeepBook(benchmark::State& state) {
    Book book(kCfg);
    std::vector<OrderId> live;
    OrderId id = 1;
    std::mt19937_64 rng(2);
    for (int i = 0; i < 40'000; ++i) {
        const bool buy = i & 1;
        book.add_limit(id, buy ? Side::Buy : Side::Sell, kMid + (buy ? -1 : 1) * (1 + static_cast<Price>(rng() % 500)), 100);
        live.push_back(id++);
    }
    for (auto _ : state) {
        const std::size_t slot = rng() % live.size();
        book.cancel(live[slot]);
        const bool buy = rng() & 1;
        book.add_limit(id, buy ? Side::Buy : Side::Sell, kMid + (buy ? -1 : 1) * (1 + static_cast<Price>(rng() % 500)), 100);
        live[slot] = id++;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

}  // namespace

BENCHMARK_TEMPLATE(BM_AddCancel, OrderBook<>)->Name("AddCancel/ladder");
BENCHMARK_TEMPLATE(BM_AddCancel, MapOrderBook<>)->Name("AddCancel/map");
BENCHMARK_TEMPLATE(BM_MatchOneOrder, OrderBook<>)->Name("MatchOne/ladder");
BENCHMARK_TEMPLATE(BM_MatchOneOrder, MapOrderBook<>)->Name("MatchOne/map");
BENCHMARK_TEMPLATE(BM_SweepRefill, OrderBook<>)->Name("SweepRefill/ladder")->Arg(1)->Arg(10)->Arg(50);
BENCHMARK_TEMPLATE(BM_SweepRefill, MapOrderBook<>)->Name("SweepRefill/map")->Arg(1)->Arg(10)->Arg(50);
BENCHMARK_TEMPLATE(BM_CancelReplaceDeepBook, OrderBook<>)->Name("CancelReplaceDeep/ladder");
BENCHMARK_TEMPLATE(BM_CancelReplaceDeepBook, MapOrderBook<>)->Name("CancelReplaceDeep/map");

BENCHMARK_MAIN();
