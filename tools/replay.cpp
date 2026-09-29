// Replay an order-flow file through a book and report per-message latency
// percentiles and sustained throughput.
//
// Usage:
//   lob_replay <file> [--format native|lobster] [--tick 100] [--book ladder|map|both] [--runs 5]
//
// Methodology
//   * The whole file is parsed into memory before any timing.
//   * Latency pass: each message is timed individually with the platform's
//     cheapest tick counter (see lob/timer.hpp). The timer's own overhead and
//     resolution are measured and printed; overhead is NOT subtracted, so the
//     numbers are an upper bound on engine time.
//   * Throughput pass: the full stream is replayed untimed, `runs` times on a
//     fresh book each run; the median run is reported.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/messages.hpp"
#include "lob/order_book.hpp"
#include "lob/timer.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

struct Counter {
    std::size_t trades = 0;
    Qty volume = 0;
    OrderId first_maker = 0;  // first maker hit by the current message
    void on_trade(const Trade& t) noexcept {
        ++trades;
        volume += t.qty;
        if (first_maker == 0) first_maker = t.maker_id;
    }
};

struct Options {
    std::string path;
    std::string format = "native";
    std::string book = "both";
    Price tick = 100;
    int runs = 5;
};

std::int64_t ns(Clock::duration d) { return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count(); }

// Percentile of tick samples, returned in nanoseconds.
double pct(std::vector<std::int64_t>& v, double p) {
    if (v.empty()) return 0.0;
    const auto k = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return static_cast<double>(v[k]) * timer::ns_per_tick();
}

void timer_overhead() {
    std::vector<std::int64_t> d(1'000'000);
    for (auto& x : d) {
        const auto a = timer::now();
        const auto b = timer::now();
        x = static_cast<std::int64_t>(b - a);
    }
    std::int64_t min_nonzero = 0;
    for (auto x : d)
        if (x > 0 && (min_nonzero == 0 || x < min_nonzero)) min_nonzero = x;
    std::printf("timer: %s, %.3f ns/tick; back-to-back overhead p50=%.1f ns p99=%.1f ns; "
                "smallest non-zero delta=%.1f ns\n\n",
                timer::name(), timer::ns_per_tick(), pct(d, 0.50), pct(d, 0.99),
                static_cast<double>(min_nonzero) * timer::ns_per_tick());
}

BookConfig config_for(const std::vector<Msg>& msgs) {
    Price lo = INT64_MAX, hi = INT64_MIN;
    for (const Msg& m : msgs)
        if (m.type == MsgType::AddLimit) {
            lo = std::min(lo, m.price);
            hi = std::max(hi, m.price);
        }
    if (lo > hi) lo = hi = 1;
    return BookConfig{std::max<Price>(1, lo - 16), hi + 16, 1 << 16};
}

template <template <class> class Book>
void run(const char* name, const std::vector<Msg>& msgs, const Options& opt) {
    const BookConfig cfg = config_for(msgs);

    // ---- latency pass
    std::array<std::vector<std::int64_t>, 4> by_type;
    for (auto& v : by_type) v.reserve(msgs.size());
    std::size_t maker_checked = 0, maker_agree = 0;
    Book<Counter> book(cfg);
    for (const Msg& m : msgs) {
        book.listener().first_maker = 0;
        const auto t0 = timer::now();
        apply(book, m);
        const auto t1 = timer::now();
        by_type[static_cast<std::size_t>(m.type)].push_back(static_cast<std::int64_t>(t1 - t0));
        if (m.expected_maker != 0) {
            ++maker_checked;
            maker_agree += book.listener().first_maker == m.expected_maker;
        }
    }
    const std::size_t trades = book.listener().trades;

    // ---- throughput pass
    std::vector<double> rates;
    for (int r = 0; r < opt.runs; ++r) {
        Book<Counter> fresh(cfg);
        const auto t0 = Clock::now();
        for (const Msg& m : msgs) apply(fresh, m);
        const auto t1 = Clock::now();
        rates.push_back(static_cast<double>(msgs.size()) / (static_cast<double>(ns(t1 - t0)) * 1e-9));
    }
    std::sort(rates.begin(), rates.end());
    const double rate = rates[rates.size() / 2];

    std::printf("## %s book\n\n", name);
    std::printf("messages=%zu  trades=%zu  resting_at_end=%zu\n", msgs.size(), trades, book.order_count());
    if (maker_checked)
        std::printf("LOBSTER executions whose maker matched the exchange: %zu / %zu (%.2f%%)\n", maker_agree,
                    maker_checked, 100.0 * static_cast<double>(maker_agree) / static_cast<double>(maker_checked));
    std::printf("throughput (median of %d runs): %.2f M msgs/s  (%.1f ns/msg mean)\n\n", opt.runs, rate / 1e6,
                1e9 / rate);
    std::printf("| type | count | p50 ns | p90 ns | p99 ns | p99.9 ns | max ns |\n");
    std::printf("|---|---:|---:|---:|---:|---:|---:|\n");
    std::vector<std::int64_t> all;
    for (std::size_t t = 0; t < by_type.size(); ++t) {
        auto& v = by_type[t];
        all.insert(all.end(), v.begin(), v.end());
        if (v.empty()) continue;
        std::printf("| %s | %zu | %.0f | %.0f | %.0f | %.0f | %.0f |\n", to_string(static_cast<MsgType>(t)),
                    v.size(), pct(v, 0.5), pct(v, 0.9), pct(v, 0.99), pct(v, 0.999), pct(v, 1.0));
    }
    std::printf("| **all** | %zu | %.0f | %.0f | %.0f | %.0f | %.0f |\n\n", all.size(), pct(all, 0.5),
                pct(all, 0.9), pct(all, 0.99), pct(all, 0.999), pct(all, 1.0));
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (!std::strcmp(argv[i], "--format")) opt.format = next();
        else if (!std::strcmp(argv[i], "--book")) opt.book = next();
        else if (!std::strcmp(argv[i], "--tick")) opt.tick = std::stoll(next());
        else if (!std::strcmp(argv[i], "--runs")) opt.runs = std::stoi(next());
        else opt.path = argv[i];
    }
    if (opt.path.empty()) {
        std::fprintf(stderr, "usage: %s <file> [--format native|lobster] [--tick N] [--book ladder|map|both] [--runs N]\n",
                     argv[0]);
        return 2;
    }

    std::vector<Msg> msgs;
    if (opt.format == "lobster") {
        LobsterStats st;
        msgs = load_lobster(opt.path, opt.tick, &st);
        std::printf("LOBSTER: submits=%zu partial_cancels=%zu deletes=%zu executions=%zu skipped=%zu\n", st.submits,
                    st.partial_cancels, st.deletes, st.executions, st.skipped);
    } else {
        msgs = load_native(opt.path);
    }
    std::printf("loaded %zu messages from %s\n", msgs.size(), opt.path.c_str());
    timer_overhead();

    if (opt.book == "ladder" || opt.book == "both") run<OrderBook>("ladder (optimized)", msgs, opt);
    if (opt.book == "map" || opt.book == "both") run<MapOrderBook>("map (baseline)", msgs, opt);
    return 0;
}
