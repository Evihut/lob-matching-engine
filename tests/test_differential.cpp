// Differential fuzz test: drive the optimized OrderBook and the reference
// MapOrderBook with the same random order flow and require identical results,
// identical trade streams, and identical top-of-book after every message.

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/messages.hpp"
#include "lob/order_book.hpp"

using namespace lob;

namespace {

struct Recorder {
    std::vector<Trade> trades;
    void on_trade(const Trade& t) { trades.push_back(t); }
};

std::vector<Msg> random_flow(std::size_t n, std::uint64_t seed, Price lo, Price hi, Price spread = 40) {
    std::mt19937_64 rng(seed);
    std::vector<Msg> out;
    std::vector<OrderId> issued;
    OrderId next = 1;
    const Price mid = (lo + hi) / 2;
    for (std::size_t i = 0; i < n; ++i) {
        const auto r = rng() % 100;
        const Side side = rng() & 1 ? Side::Buy : Side::Sell;
        const Qty qty = 1 + static_cast<Qty>(rng() % 50);
        if (r < 50 || issued.empty()) {
            // Wide price range around mid so books both cross and build depth,
            // occasionally outside the band to exercise rejection.
            const Price px = mid + static_cast<Price>(rng() % static_cast<std::uint64_t>(2 * spread + 1)) - spread +
                             (rng() % 200 == 0 ? hi : 0);
            const auto tif = rng() % 10 == 0 ? TimeInForce::IOC : TimeInForce::GTC;
            // 2% of adds reuse an id (duplicate rejection), 0.5% use the reserved id.
            const auto pick = rng() % 200;
            const OrderId id = pick < 4 && !issued.empty() ? issued[rng() % issued.size()]
                               : pick == 4                 ? kReservedOrderId
                                                           : next++;
            out.push_back({MsgType::AddLimit, side, tif, id, px, qty, 0});
            issued.push_back(id);
        } else if (r < 80) {
            out.push_back({MsgType::Cancel, side, TimeInForce::GTC, issued[rng() % issued.size()], 0, 0, 0});
        } else if (r < 90) {
            out.push_back({MsgType::Reduce, side, TimeInForce::GTC, issued[rng() % issued.size()], 0, qty, 0});
        } else {
            out.push_back({MsgType::AddMarket, side, TimeInForce::GTC, next++, 0, qty * 3, 0});
        }
    }
    return out;
}

template <class Book>
auto step(Book& b, const Msg& m) {
    struct R {
        AddResult add{};
        bool ok = false;
    } r;
    switch (m.type) {
        case MsgType::AddLimit: r.add = b.add_limit(m.id, m.side, m.price, m.qty, m.tif); break;
        case MsgType::AddMarket: r.add = b.add_market(m.id, m.side, m.qty); break;
        case MsgType::Cancel: r.ok = b.cancel(m.id); break;
        case MsgType::Reduce: r.ok = b.reduce(m.id, m.qty); break;
    }
    return r;
}

}  // namespace

class Differential : public ::testing::TestWithParam<std::uint64_t> {};

void run_differential(const BookConfig& cfg, std::uint64_t seed, Price spread) {
    OrderBook<Recorder> fast(cfg);
    MapOrderBook<Recorder> ref(cfg);
    const auto flow = random_flow(100'000, seed, cfg.min_price, cfg.max_price, spread);
    for (std::size_t i = 0; i < flow.size(); ++i) {
        const auto a = step(fast, flow[i]);
        const auto b = step(ref, flow[i]);
        ASSERT_EQ(a.add, b.add) << "message " << i;
        ASSERT_EQ(a.ok, b.ok) << "message " << i;
        ASSERT_EQ(fast.best_bid(), ref.best_bid()) << "message " << i;
        ASSERT_EQ(fast.best_ask(), ref.best_ask()) << "message " << i;
        ASSERT_EQ(fast.order_count(), ref.order_count()) << "message " << i;
        if (fast.best_bid() && fast.best_ask()) ASSERT_LT(*fast.best_bid(), *fast.best_ask()) << "crossed book";
    }
    ASSERT_EQ(fast.listener().trades, ref.listener().trades);
    EXPECT_GT(fast.listener().trades.size(), 1000u);  // the flow really exercised matching
    for (Price p = cfg.min_price; p <= cfg.max_price; ++p) ASSERT_EQ(fast.volume_at(p), ref.volume_at(p)) << p;
}

// Dense book: narrow band, prices within +/-40 ticks of mid.
TEST_P(Differential, DenseBookMatchesReference) { run_differential(BookConfig{900, 1100, 128}, GetParam(), 40); }

// Sparse book: prices spread over +/-150k ticks of a 300k-tick band, so level
// searches cross many bitmap and summary words.
TEST_P(Differential, SparseBookMatchesReference) {
    run_differential(BookConfig{1, 300'000, 128}, GetParam(), 150'000);
}

INSTANTIATE_TEST_SUITE_P(Seeds, Differential, ::testing::Values(1, 2, 3, 4, 5, 6, 7, 8));
