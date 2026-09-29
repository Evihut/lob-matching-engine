#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "lob/types.hpp"

namespace lob {

// Straightforward reference implementation: std::map of price levels, each a
// std::list FIFO, plus an unordered_map for O(1) cancel.
//
// It is the textbook design and serves two purposes: a correctness oracle for
// differential tests against OrderBook, and a performance baseline in the
// benchmarks. Its public interface and semantics match OrderBook exactly.
template <class Listener = NullListener>
class MapOrderBook {
public:
    explicit MapOrderBook(BookConfig cfg = {}, Listener listener = {})
        : min_price_(cfg.min_price), max_price_(cfg.max_price), listener_(std::move(listener)) {
        index_.reserve(cfg.expected_orders);
    }

    AddResult add_limit(OrderId id, Side side, Price price, Qty qty,
                        TimeInForce tif = TimeInForce::GTC) {
        if (qty <= 0) return {Status::RejectedInvalidQty, 0, 0};
        if (price < min_price_ || price > max_price_) return {Status::RejectedInvalidPrice, 0, 0};
        if (index_.contains(id)) return {Status::RejectedDuplicateId, 0, 0};

        const Qty filled = side == Side::Buy ? match(asks_, id, side, qty, price)
                                             : match(bids_, id, side, qty, price);
        const Qty left = qty - filled;
        if (left == 0) return {Status::Filled, filled, 0};
        if (tif == TimeInForce::IOC) return {Status::Expired, filled, left};

        auto& queue = side == Side::Buy ? bids_[price] : asks_[price];
        queue.push_back(Order{id, left, price, side});
        index_.emplace(id, std::prev(queue.end()));
        return {Status::Resting, filled, left};
    }

    AddResult add_market(OrderId id, Side side, Qty qty) {
        if (qty <= 0) return {Status::RejectedInvalidQty, 0, 0};
        const Qty filled = side == Side::Buy ? match(asks_, id, side, qty, max_price_)
                                             : match(bids_, id, side, qty, min_price_);
        const Qty left = qty - filled;
        return left == 0 ? AddResult{Status::Filled, filled, 0} : AddResult{Status::Expired, filled, left};
    }

    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        erase(it);
        return true;
    }

    bool reduce(OrderId id, Qty by) {
        if (by <= 0) return false;
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        if (by >= it->second->qty) erase(it);
        else it->second->qty -= by;
        return true;
    }

    std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }

    std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    Qty volume_at(Price price) const {
        Qty total = 0;
        if (auto it = bids_.find(price); it != bids_.end())
            for (const Order& o : it->second) total += o.qty;
        if (auto it = asks_.find(price); it != asks_.end())
            for (const Order& o : it->second) total += o.qty;
        return total;
    }

    std::size_t order_count() const noexcept { return index_.size(); }

    std::optional<Qty> order_qty(OrderId id) const {
        auto it = index_.find(id);
        if (it == index_.end()) return std::nullopt;
        return it->second->qty;
    }

    Listener& listener() noexcept { return listener_; }

private:
    struct Order {
        OrderId id;
        Qty qty;
        Price price;
        Side side;
    };
    using Queue = std::list<Order>;

    // Match against the opposite side's map (best level first) up to `limit`.
    template <class Levels>
    Qty match(Levels& levels, OrderId taker, Side taker_side, Qty qty, Price limit) {
        Qty filled = 0;
        while (filled < qty && !levels.empty()) {
            auto lvl = levels.begin();
            const Price px = lvl->first;
            if (taker_side == Side::Buy ? px > limit : px < limit) break;
            Queue& q = lvl->second;
            while (filled < qty && !q.empty()) {
                Order& maker = q.front();
                const Qty fill = std::min(maker.qty, qty - filled);
                maker.qty -= fill;
                filled += fill;
                listener_.on_trade(Trade{taker, maker.id, taker_side, px, fill});
                if (maker.qty == 0) {
                    index_.erase(maker.id);
                    q.pop_front();
                }
            }
            if (q.empty()) levels.erase(lvl);
        }
        return filled;
    }

    void erase(typename std::unordered_map<OrderId, typename Queue::iterator>::iterator it) {
        const Order o = *it->second;
        if (o.side == Side::Buy) erase_from(bids_, o.price, it->second);
        else erase_from(asks_, o.price, it->second);
        index_.erase(it);
    }

    template <class Levels>
    static void erase_from(Levels& levels, Price price, typename Queue::iterator pos) {
        auto lvl = levels.find(price);
        lvl->second.erase(pos);
        if (lvl->second.empty()) levels.erase(lvl);
    }

    Price min_price_;
    Price max_price_;
    std::map<Price, Queue, std::greater<>> bids_;
    std::map<Price, Queue> asks_;
    std::unordered_map<OrderId, typename Queue::iterator> index_;
    Listener listener_;
};

}  // namespace lob
