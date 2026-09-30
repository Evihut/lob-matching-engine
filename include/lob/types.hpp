#pragma once

#include <cstdint>
#include <limits>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // integer ticks; never a floating point price
using Qty = std::int64_t;

// The one order id callers may not use: IdMap stores it as its empty-slot marker.
// Every entry point rejects it explicitly.
inline constexpr OrderId kReservedOrderId = std::numeric_limits<OrderId>::max();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class TimeInForce : std::uint8_t {
    GTC,  // rest any unfilled remainder
    IOC,  // cancel any unfilled remainder
};

enum class Status : std::uint8_t {
    Resting,             // remainder rests on the book (it may have partially filled first)
    Filled,              // fully filled on arrival
    Expired,             // IOC / market remainder cancelled (it may have partially filled first)
    RejectedDuplicateId, // a resting order already uses this id
    RejectedInvalidId,   // id == kReservedOrderId
    RejectedInvalidQty,  // qty <= 0
    RejectedInvalidPrice // limit price outside the configured band
};

struct AddResult {
    Status status;
    Qty filled;
    Qty remaining;  // resting qty for Resting, cancelled qty for Expired, 0 otherwise

    friend bool operator==(const AddResult&, const AddResult&) = default;
};

struct Trade {
    OrderId taker_id;
    OrderId maker_id;
    Side taker_side;
    Price price;  // always the resting (maker) order's price
    Qty qty;

    friend bool operator==(const Trade&, const Trade&) = default;
};

// Inclusive price band. Prices outside it are rejected, which lets the
// optimized book index price levels directly in a flat array.
struct BookConfig {
    Price min_price = 1;
    Price max_price = 1 << 20;
    std::uint32_t expected_orders = 1 << 16;

};

// Receives execution events. Books take the listener as a template parameter,
// so a listener with empty inline methods costs nothing at runtime.
struct NullListener {
    void on_trade(const Trade&) noexcept {}
};

}  // namespace lob
