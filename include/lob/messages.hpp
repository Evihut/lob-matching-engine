#pragma once

#include <charconv>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

enum class MsgType : std::uint8_t { AddLimit, AddMarket, Cancel, Reduce };

// One order-entry event. Replays are fully parsed into a vector<Msg> before
// timing starts so that parsing never shows up in latency numbers.
struct Msg {
    MsgType type;
    Side side;
    TimeInForce tif;
    OrderId id;
    Price price;
    Qty qty;
    OrderId expected_maker;  // LOBSTER executions: the resting order the exchange filled (0 = unknown)
};

template <class Book>
inline void apply(Book& book, const Msg& m) {
    switch (m.type) {
        case MsgType::AddLimit: book.add_limit(m.id, m.side, m.price, m.qty, m.tif); break;
        case MsgType::AddMarket: book.add_market(m.id, m.side, m.qty); break;
        case MsgType::Cancel: book.cancel(m.id); break;
        case MsgType::Reduce: book.reduce(m.id, m.qty); break;
    }
}

inline const char* to_string(MsgType t) {
    switch (t) {
        case MsgType::AddLimit: return "limit";
        case MsgType::AddMarket: return "market";
        case MsgType::Cancel: return "cancel";
        case MsgType::Reduce: return "reduce";
    }
    return "?";
}

namespace detail {

inline std::vector<std::string_view> split(std::string_view line, char sep = ',') {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(sep, start);
        out.push_back(line.substr(start, pos - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

template <class T>
T to_int(std::string_view s) {
    T v{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{}) throw std::runtime_error("bad integer: " + std::string(s));
    return v;
}

inline Side parse_side(std::string_view s) {
    if (s == "B") return Side::Buy;
    if (s == "S") return Side::Sell;
    throw std::runtime_error("bad side: " + std::string(s));
}

}  // namespace detail

// Native CSV format, one event per line:
//   A,<id>,<B|S>,<price>,<qty>[,I]   limit order (optional I = immediate-or-cancel)
//   M,<id>,<B|S>,<qty>               market order
//   C,<id>                           cancel
//   R,<id>,<qty>                     reduce by qty, keeping priority
// Lines starting with '#' are comments.
inline Msg parse_native_line(std::string_view line) {
    using namespace detail;
    const auto f = split(line);
    Msg m{};
    switch (f.at(0).empty() ? '?' : f[0][0]) {
        case 'A':
            m.type = MsgType::AddLimit;
            m.id = to_int<OrderId>(f.at(1));
            m.side = parse_side(f.at(2));
            m.price = to_int<Price>(f.at(3));
            m.qty = to_int<Qty>(f.at(4));
            m.tif = f.size() > 5 && f[5] == "I" ? TimeInForce::IOC : TimeInForce::GTC;
            break;
        case 'M':
            m.type = MsgType::AddMarket;
            m.id = to_int<OrderId>(f.at(1));
            m.side = parse_side(f.at(2));
            m.qty = to_int<Qty>(f.at(3));
            break;
        case 'C':
            m.type = MsgType::Cancel;
            m.id = to_int<OrderId>(f.at(1));
            break;
        case 'R':
            m.type = MsgType::Reduce;
            m.id = to_int<OrderId>(f.at(1));
            m.qty = to_int<Qty>(f.at(2));
            break;
        default: throw std::runtime_error("bad message: " + std::string(line));
    }
    return m;
}

inline std::vector<Msg> load_native(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<Msg> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        out.push_back(parse_native_line(line));
    }
    return out;
}

struct LobsterStats {
    std::size_t submits = 0, partial_cancels = 0, deletes = 0, executions = 0, skipped = 0;
};

// LOBSTER message files (https://lobsterdata.com) have columns
//   time, type, order_id, size, price (USD * 10000), direction (1 buy / -1 sell)
// Event types are mapped onto engine operations:
//   1 submission          -> AddLimit
//   2 partial cancel      -> Reduce
//   3 deletion            -> Cancel
//   4 visible execution   -> AddMarket on the opposite side, sized to the fill;
//                            expected_maker records which order the exchange hit
//   5 hidden execution, 6 cross trade, 7 halt -> skipped (no visible book effect)
// `tick` converts LOBSTER prices into integer ticks (100 = one cent).
inline std::vector<Msg> load_lobster(const std::string& path, Price tick, LobsterStats* stats = nullptr) {
    using namespace detail;
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<Msg> out;
    LobsterStats st;
    OrderId next_taker = 1ull << 62;  // synthetic ids for the aggressor side of executions
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto f = split(line);
        const int type = to_int<int>(f.at(1));
        const OrderId id = to_int<OrderId>(f.at(2));
        const Qty size = to_int<Qty>(f.at(3));
        const Price price = to_int<Price>(f.at(4)) / tick;
        const Side side = to_int<int>(f.at(5)) == 1 ? Side::Buy : Side::Sell;
        switch (type) {
            case 1: out.push_back({MsgType::AddLimit, side, TimeInForce::GTC, id, price, size, 0}); ++st.submits; break;
            case 2: out.push_back({MsgType::Reduce, side, TimeInForce::GTC, id, price, size, 0}); ++st.partial_cancels; break;
            case 3: out.push_back({MsgType::Cancel, side, TimeInForce::GTC, id, price, size, 0}); ++st.deletes; break;
            case 4:
                out.push_back({MsgType::AddMarket, opposite(side), TimeInForce::GTC, next_taker++, price, size, id});
                ++st.executions;
                break;
            default: ++st.skipped; break;
        }
    }
    if (stats) *stats = st;
    return out;
}

}  // namespace lob
