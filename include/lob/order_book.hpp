#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <vector>

#include "lob/id_map.hpp"
#include "lob/types.hpp"

namespace lob {

// Price-time priority limit order book tuned for low latency.
//
// Layout:
//   * Price levels live in a flat array indexed by (price - min_price), so
//     finding a level is one subtraction instead of a tree walk.
//   * A two-level bitmap marks non-empty levels (one bit per level, plus one
//     summary bit per 64-bit word), so the next best price after a level
//     empties is found with a few count-leading/trailing-zeros instructions
//     even when the book is sparse or one side is empty.
//   * Orders are nodes in a pooled vector linked into per-level FIFO queues by
//     32-bit indices, so the hot path does no heap allocation once warm.
//   * OrderId -> node lookup uses an open-addressing hash map (IdMap).
//
// A level only ever holds orders of one side: the book is never crossed, so
// every non-empty level above the best bid is an ask and every non-empty level
// below the best ask is a bid.
template <class Listener = NullListener>
class OrderBook {
public:
    // Throws std::invalid_argument if the config is invalid (see BookConfig::validate).
    explicit OrderBook(BookConfig cfg = {}, Listener listener = {})
        : min_price_((cfg.validate(), cfg.min_price)),
          max_price_(cfg.max_price),
          levels_(static_cast<std::size_t>(cfg.max_price - cfg.min_price + 1)),
          bitmap_((levels_.size() + 63) / 64, 0),
          summary_((bitmap_.size() + 63) / 64, 0),
          ids_(cfg.expected_orders),
          best_bid_(kNoBid),
          best_ask_(static_cast<std::int64_t>(levels_.size())),
          listener_(std::move(listener)) {
        nodes_.reserve(cfg.expected_orders);
        free_.reserve(cfg.expected_orders);
    }

    AddResult add_limit(OrderId id, Side side, Price price, Qty qty,
                        TimeInForce tif = TimeInForce::GTC) {
        if (id == kReservedOrderId) return {Status::RejectedInvalidId, 0, 0};
        if (qty <= 0) return {Status::RejectedInvalidQty, 0, 0};
        if (price < min_price_ || price > max_price_) return {Status::RejectedInvalidPrice, 0, 0};
        if (ids_.find(id) != IdMap::kNotFound) return {Status::RejectedDuplicateId, 0, 0};

        const auto idx = static_cast<std::int64_t>(price - min_price_);
        const Qty filled = side == Side::Buy ? match_buy(id, qty, idx) : match_sell(id, qty, idx);
        const Qty left = qty - filled;
        if (left == 0) return {Status::Filled, filled, 0};
        if (tif == TimeInForce::IOC) return {Status::Expired, filled, left};
        rest(id, side, idx, left);
        return {Status::Resting, filled, left};
    }

    // Market orders sweep the opposite side and never rest.
    AddResult add_market(OrderId id, Side side, Qty qty) {
        if (id == kReservedOrderId) return {Status::RejectedInvalidId, 0, 0};
        if (qty <= 0) return {Status::RejectedInvalidQty, 0, 0};
        const Qty filled = side == Side::Buy
                               ? match_buy(id, qty, static_cast<std::int64_t>(levels_.size()) - 1)
                               : match_sell(id, qty, 0);
        const Qty left = qty - filled;
        return left == 0 ? AddResult{Status::Filled, filled, 0} : AddResult{Status::Expired, filled, left};
    }

    bool cancel(OrderId id) {
        const std::uint32_t n = ids_.find(id);
        if (n == IdMap::kNotFound) return false;
        remove_node(n);
        return true;
    }

    // Reduce a resting order's quantity in place, keeping its queue priority.
    // Reducing by the full remaining quantity or more cancels the order.
    bool reduce(OrderId id, Qty by) {
        if (by <= 0) return false;
        const std::uint32_t n = ids_.find(id);
        if (n == IdMap::kNotFound) return false;
        Node& node = nodes_[n];
        if (by >= node.qty) {
            remove_node(n);
        } else {
            node.qty -= by;
            levels_[node.level].total -= by;
        }
        return true;
    }

    std::optional<Price> best_bid() const noexcept {
        if (best_bid_ == kNoBid) return std::nullopt;
        return min_price_ + best_bid_;
    }

    std::optional<Price> best_ask() const noexcept {
        if (best_ask_ == static_cast<std::int64_t>(levels_.size())) return std::nullopt;
        return min_price_ + best_ask_;
    }

    // Total resting quantity at a price (either side; a level holds one side).
    Qty volume_at(Price price) const noexcept {
        if (price < min_price_ || price > max_price_) return 0;
        return levels_[static_cast<std::size_t>(price - min_price_)].total;
    }

    std::size_t order_count() const noexcept { return ids_.size(); }

    std::optional<Qty> order_qty(OrderId id) const noexcept {
        const std::uint32_t n = ids_.find(id);
        if (n == IdMap::kNotFound) return std::nullopt;
        return nodes_[n].qty;
    }

    Listener& listener() noexcept { return listener_; }

private:
    static constexpr std::uint32_t kNil = 0xFFFFFFFFu;
    static constexpr std::int64_t kNoBid = -1;

    struct Node {
        OrderId id;
        Qty qty;
        std::uint32_t level;
        std::uint32_t prev;
        std::uint32_t next;
        Side side;
    };

    struct Level {
        std::uint32_t head = kNil;
        std::uint32_t tail = kNil;
        Qty total = 0;
    };

    // ------------------------------------------------------------- matching

    Qty match_buy(OrderId taker, Qty qty, std::int64_t limit_idx) {
        Qty filled = 0;
        while (filled < qty && best_ask_ <= limit_idx) {
            filled += consume_level(taker, Side::Buy, best_ask_, qty - filled);
            if (levels_[best_ask_].head == kNil) {
                clear_bit(best_ask_);
                best_ask_ = next_set_above(best_ask_ + 1);
            }
        }
        return filled;
    }

    Qty match_sell(OrderId taker, Qty qty, std::int64_t limit_idx) {
        Qty filled = 0;
        while (filled < qty && best_bid_ >= limit_idx && best_bid_ != kNoBid) {
            filled += consume_level(taker, Side::Sell, best_bid_, qty - filled);
            if (levels_[best_bid_].head == kNil) {
                clear_bit(best_bid_);
                best_bid_ = prev_set_below(best_bid_ - 1);
            }
        }
        return filled;
    }

    // Fill up to `want` against one level in FIFO order; returns qty filled.
    Qty consume_level(OrderId taker, Side taker_side, std::int64_t idx, Qty want) {
        Level& lvl = levels_[static_cast<std::size_t>(idx)];
        const Price px = min_price_ + idx;
        Qty got = 0;
        while (got < want && lvl.head != kNil) {
            const std::uint32_t n = lvl.head;
            Node& maker = nodes_[n];
            const Qty fill = maker.qty < want - got ? maker.qty : want - got;
            maker.qty -= fill;
            lvl.total -= fill;
            got += fill;
            listener_.on_trade(Trade{taker, maker.id, taker_side, px, fill});
            if (maker.qty == 0) {
                lvl.head = maker.next;
                if (lvl.head == kNil) lvl.tail = kNil;
                else nodes_[lvl.head].prev = kNil;
                ids_.erase(maker.id);
                free_.push_back(n);
            }
        }
        return got;
    }

    // --------------------------------------------------------- book upkeep

    void rest(OrderId id, Side side, std::int64_t idx, Qty qty) {
        std::uint32_t n;
        const auto level = static_cast<std::uint32_t>(idx);
        if (!free_.empty()) {
            n = free_.back();
            free_.pop_back();
            nodes_[n] = Node{id, qty, level, kNil, kNil, side};
        } else {
            n = static_cast<std::uint32_t>(nodes_.size());
            nodes_.push_back(Node{id, qty, level, kNil, kNil, side});
        }
        ids_.insert(id, n);

        Level& lvl = levels_[level];
        if (lvl.tail == kNil) {
            lvl.head = lvl.tail = n;
            set_bit(idx);
            if (side == Side::Buy) {
                if (idx > best_bid_) best_bid_ = idx;
            } else if (idx < best_ask_) {
                best_ask_ = idx;
            }
        } else {
            nodes_[lvl.tail].next = n;
            nodes_[n].prev = lvl.tail;
            lvl.tail = n;
        }
        lvl.total += qty;
    }

    void remove_node(std::uint32_t n) {
        Node& node = nodes_[n];
        Level& lvl = levels_[node.level];
        if (node.prev != kNil) nodes_[node.prev].next = node.next;
        else lvl.head = node.next;
        if (node.next != kNil) nodes_[node.next].prev = node.prev;
        else lvl.tail = node.prev;
        lvl.total -= node.qty;
        ids_.erase(node.id);
        free_.push_back(n);

        if (lvl.head == kNil) {
            const std::int64_t idx = node.level;
            clear_bit(idx);
            if (node.side == Side::Buy) {
                if (idx == best_bid_) best_bid_ = prev_set_below(idx - 1);
            } else if (idx == best_ask_) {
                best_ask_ = next_set_above(idx + 1);
            }
        }
    }

    // ------------------------------------------------------------- bitmap

    void set_bit(std::int64_t i) noexcept {
        const std::size_t w = static_cast<std::size_t>(i) >> 6;
        bitmap_[w] |= 1ull << (i & 63);
        summary_[w >> 6] |= 1ull << (w & 63);
    }

    void clear_bit(std::int64_t i) noexcept {
        const std::size_t w = static_cast<std::size_t>(i) >> 6;
        bitmap_[w] &= ~(1ull << (i & 63));
        if (bitmap_[w] == 0) summary_[w >> 6] &= ~(1ull << (w & 63));
    }

    static constexpr std::uint64_t mask_from(unsigned b) noexcept { return ~0ull << b; }  // bits >= b
    static constexpr std::uint64_t mask_upto(unsigned b) noexcept {                       // bits <= b
        return b == 63 ? ~0ull : (1ull << (b + 1)) - 1;
    }

    // Lowest set index >= i, or levels_.size() when there is none.
    std::int64_t next_set_above(std::int64_t i) const noexcept {
        const auto end = static_cast<std::int64_t>(levels_.size());
        if (i >= end) return end;
        std::size_t w = static_cast<std::size_t>(i) >> 6;
        if (const std::uint64_t bits = bitmap_[w] & mask_from(static_cast<unsigned>(i & 63)))
            return static_cast<std::int64_t>(w * 64 + static_cast<std::size_t>(std::countr_zero(bits)));
        // Find the next non-empty word via the summary level.
        ++w;
        if (w >= bitmap_.size()) return end;
        std::size_t s = w >> 6;
        std::uint64_t sbits = summary_[s] & mask_from(static_cast<unsigned>(w & 63));
        while (sbits == 0) {
            if (++s == summary_.size()) return end;
            sbits = summary_[s];
        }
        w = s * 64 + static_cast<std::size_t>(std::countr_zero(sbits));
        return static_cast<std::int64_t>(w * 64 + static_cast<std::size_t>(std::countr_zero(bitmap_[w])));
    }

    // Highest set index <= i, or kNoBid when there is none.
    std::int64_t prev_set_below(std::int64_t i) const noexcept {
        if (i < 0) return kNoBid;
        std::size_t w = static_cast<std::size_t>(i) >> 6;
        if (const std::uint64_t bits = bitmap_[w] & mask_upto(static_cast<unsigned>(i & 63)))
            return static_cast<std::int64_t>(w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(bits)));
        if (w == 0) return kNoBid;
        --w;
        std::size_t s = w >> 6;
        std::uint64_t sbits = summary_[s] & mask_upto(static_cast<unsigned>(w & 63));
        while (sbits == 0) {
            if (s == 0) return kNoBid;
            sbits = summary_[--s];
        }
        w = s * 64 + 63 - static_cast<std::size_t>(std::countl_zero(sbits));
        return static_cast<std::int64_t>(w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(bitmap_[w])));
    }

    Price min_price_;
    Price max_price_;
    std::vector<Level> levels_;
    std::vector<std::uint64_t> bitmap_;   // bit per price level
    std::vector<std::uint64_t> summary_;  // bit per non-zero bitmap_ word
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> free_;
    IdMap ids_;
    std::int64_t best_bid_;
    std::int64_t best_ask_;
    Listener listener_;
};

}  // namespace lob
