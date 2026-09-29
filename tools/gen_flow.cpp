// Synthetic order-flow generator.
//
// Produces a self-consistent event stream in the native CSV format: cancels and
// reduces only target orders that are still resting, and aggressive orders are
// priced off the live book. The event mix loosely follows published message
// statistics for liquid NASDAQ names (most traffic is passive adds and cancels,
// a few percent are executions).
//
// Cancel and partial-cancel rates scale with book depth relative to a target,
// so the book settles around `target_depth` resting orders instead of draining
// or growing without bound. At the target the mix is 46% passive adds, 40%
// cancels, 6% partial cancels, 4% market orders, 4% marketable limits.
//
// Usage: lob_gen <out.csv> [n_messages=1000000] [seed=42] [target_depth=10000]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

// Tracks remaining quantity of every resting order so the generator can pick
// valid cancel targets in O(1).
struct Tracker {
    std::unordered_map<OrderId, std::pair<Qty, std::size_t>> live;  // id -> (qty, slot in ids)
    std::vector<OrderId> ids;

    void add(OrderId id, Qty q) {
        live[id] = {q, ids.size()};
        ids.push_back(id);
    }
    void remove(OrderId id) {
        auto it = live.find(id);
        if (it == live.end()) return;
        const std::size_t slot = it->second.second;
        ids[slot] = ids.back();
        live[ids[slot]].second = slot;
        ids.pop_back();
        live.erase(it);
    }
    void on_trade(const Trade& t) {
        auto it = live.find(t.maker_id);
        if (it == live.end()) return;
        it->second.first -= t.qty;
        if (it->second.first == 0) remove(t.maker_id);
    }
};

struct Fwd {
    Tracker* t;
    void on_trade(const Trade& tr) { t->on_trade(tr); }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out.csv> [n_messages] [seed] [target_depth]\n", argv[0]);
        return 2;
    }
    const std::string out_path = argv[1];
    const std::size_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1'000'000;
    const unsigned seed = argc > 3 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)) : 42;
    const double target_depth = argc > 4 ? std::strtod(argv[4], nullptr) : 10'000.0;

    constexpr Price kMin = 1, kMax = 200'000, kStartMid = 100'000;
    Tracker tracker;
    OrderBook<Fwd> book(BookConfig{kMin, kMax, 1 << 18}, Fwd{&tracker});
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::geometric_distribution<int> depth(0.1);   // passive distance from the touch, in ticks
    std::lognormal_distribution<double> lot_dist(0.7, 0.8);

    auto size = [&] { return static_cast<Qty>(100 * std::max(1.0, std::round(lot_dist(rng)))); };

    std::FILE* out = std::fopen(out_path.c_str(), "w");
    if (!out) {
        std::perror("fopen");
        return 1;
    }
    std::fprintf(out, "# synthetic order flow: n=%zu seed=%u target_depth=%.0f\n", n, seed, target_depth);

    OrderId next_id = 1;
    std::size_t counts[5] = {};
    for (std::size_t i = 0; i < n; ++i) {
        const Price bid = book.best_bid().value_or(book.best_ask().value_or(kStartMid) - 1);
        const Price ask = book.best_ask().value_or(bid + 1);
        const double fill = std::min(1.5, static_cast<double>(tracker.ids.size()) / target_depth);
        const double p_cancel = 0.40 * fill, p_reduce = 0.06 * fill;
        const double p_passive = 1.0 - p_cancel - p_reduce - 0.08;
        const double r = u(rng);
        // Keep a few hundred orders resting before allowing cancels/aggression.
        const bool thin = tracker.ids.size() < 200;

        if (thin || r < p_passive) {  // passive limit order
            const Side side = u(rng) < 0.5 ? Side::Buy : Side::Sell;
            Price px = side == Side::Buy ? bid - depth(rng) : ask + depth(rng);
            if (ask - bid > 1 && u(rng) < 0.3) px = side == Side::Buy ? bid + 1 : ask - 1;  // improve the touch
            px = std::clamp(px, kMin, kMax);
            const Qty q = size();
            const OrderId id = next_id++;
            if (book.add_limit(id, side, px, q).status == Status::Resting) tracker.add(id, *book.order_qty(id));
            std::fprintf(out, "A,%llu,%c,%lld,%lld\n", (unsigned long long)id, side == Side::Buy ? 'B' : 'S',
                         (long long)px, (long long)q);
            ++counts[0];
        } else if (r < p_passive + p_cancel) {  // full cancel
            const OrderId id = tracker.ids[rng() % tracker.ids.size()];
            book.cancel(id);
            tracker.remove(id);
            std::fprintf(out, "C,%llu\n", (unsigned long long)id);
            ++counts[1];
        } else if (r < p_passive + p_cancel + p_reduce) {  // partial cancel
            const OrderId id = tracker.ids[rng() % tracker.ids.size()];
            const Qty have = tracker.live[id].first;
            const Qty lots = have / 100;  // sizes and fills are whole 100-share lots
            const Qty by = lots > 1 ? 100 * static_cast<Qty>(1 + rng() % static_cast<std::uint64_t>(lots - 1)) : have;
            book.reduce(id, by);
            if (by >= have) tracker.remove(id);
            else tracker.live[id].first -= by;
            std::fprintf(out, "R,%llu,%lld\n", (unsigned long long)id, (long long)by);
            ++counts[2];
        } else if (r < p_passive + p_cancel + p_reduce + 0.04) {  // market order
            const Side side = u(rng) < 0.5 ? Side::Buy : Side::Sell;
            const Qty q = size();
            const OrderId id = next_id++;
            book.add_market(id, side, q);
            std::fprintf(out, "M,%llu,%c,%lld\n", (unsigned long long)id, side == Side::Buy ? 'B' : 'S', (long long)q);
            ++counts[3];
        } else {  // marketable limit that may walk a few levels and rest the remainder
            const Side side = u(rng) < 0.5 ? Side::Buy : Side::Sell;
            const Price px = side == Side::Buy ? ask + static_cast<Price>(rng() % 3) : bid - static_cast<Price>(rng() % 3);
            const Qty q = size() * 2;
            const OrderId id = next_id++;
            const bool ioc = u(rng) < 0.3;
            const auto res = book.add_limit(id, side, std::clamp(px, kMin, kMax), q, ioc ? TimeInForce::IOC : TimeInForce::GTC);
            if (res.status == Status::Resting) tracker.add(id, res.remaining);
            std::fprintf(out, "A,%llu,%c,%lld,%lld%s\n", (unsigned long long)id, side == Side::Buy ? 'B' : 'S',
                         (long long)std::clamp(px, kMin, kMax), (long long)q, ioc ? ",I" : "");
            ++counts[4];
        }
    }
    std::fclose(out);
    std::fprintf(stderr,
                 "wrote %zu messages: passive=%zu cancel=%zu reduce=%zu market=%zu aggressive_limit=%zu; "
                 "resting at end=%zu\n",
                 n, counts[0], counts[1], counts[2], counts[3], counts[4], book.order_count());
    return 0;
}
