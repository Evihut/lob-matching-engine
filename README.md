# lob-matching-engine

A header-only C++20 limit order book and matching engine with price-time priority,
built for low and predictable latency, plus a replay harness that reports per-message
latency percentiles and sustained throughput.

- **Order types:** limit (GTC / IOC), market, cancel, and reduce (partial cancel that keeps queue priority)
- **Matching:** strict price-time priority; trades print at the resting order's price
- **Correctness:** 62 GoogleTest cases, including a differential fuzz test of 1.6M random messages against a
  textbook `std::map` reference book (identical trade streams and top of book after every message);
  clean under AddressSanitizer + UBSan
- **Performance (single thread):** **91 M msgs/s** on a 5M-message replay on an Apple M4, **5.7× the `std::map`
  baseline**; add+cancel in **7.9 ns**, single-order match in **8.3 ns**. With `RDTSC` on x86 (GitHub Actions runner):
  per-message **p50 64 ns / p99 139 ns / p99.9 209 ns** including ~20 ns of timer overhead

## Design

```
                     price level array (index = price - min_price)
 bitmap summary ─┐     ┌──────┬──────┬──────┬──────┬──────┬──────┐
 (1 bit / word)  └──►  │ bid  │ bid  │      │      │ ask  │ ask  │  ...  Level{head, tail, total}
 bitmap          ──►   │  1   │  1   │  0   │  0   │  1   │  1   │
 (1 bit / level)       └──┬───┴──────┴──────┴──────┴──┬───┴──────┘
                          │ best_bid                  │ best_ask
                          ▼                           ▼
                       [o7]⇄[o9]⇄[o12]            [o3]⇄[o8]           FIFO per level: intrusive doubly
                                                                     linked list of 32-bit node indices
  OrderId ──► IdMap (open addressing, linear probing, backward-shift delete) ──► node index
```

| Choice | Why |
|---|---|
| Flat array of price levels over a configured price band | Level lookup is one subtraction, no tree walk or pointer chasing. |
| Two-level bitmap of non-empty levels | When the best level empties, the next one is found with `ctz`/`clz` over at most a few words, even if the book is sparse or a side is empty (this cut the "sweep empties a side" case from 70 ns to 29 ns). |
| Order nodes in a pooled `std::vector` with a free list, linked by 32-bit indices | No heap allocation on the hot path once warm; 32-byte nodes; indices instead of pointers survive vector growth. |
| Custom `IdMap` instead of `std::unordered_map` | Flat, cache-friendly, no per-node allocation; backward-shift deletion avoids tombstone build-up under heavy cancel churn. |
| Listener as a template parameter | Execution callbacks are inlined; a no-op listener costs nothing. No virtual dispatch. |
| Integer tick prices | No floating-point comparison or rounding in matching. |

Invariant used throughout: the book is never crossed, so any non-empty level above the best bid is an ask and any
non-empty level below the best ask is a bid. That lets one array and one bitmap serve both sides.

`MapOrderBook` (`std::map<Price, std::list<Order>>` + `std::unordered_map`) has the same interface and semantics.
It is the correctness oracle for differential testing and the baseline for every benchmark.

## Results

Machine: Apple M4 (10 cores), macOS, Apple clang 21, `-O3` Release. Single thread; macOS does not support pinning
threads to cores, so numbers include normal OS scheduling noise.

### Market-data replay

5,000,000 messages from the synthetic generator (`lob_gen`, seed 42), book held at ~9–10k resting orders.
Message mix: 54% limit (incl. 4% marketable, some IOC), 37% cancel, 5.5% reduce, 4% market; 806,903 trades.

| Book | Throughput (median of 7) | Mean per msg | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| **ladder (this engine)** | **91.3 M msgs/s** | **11.0 ns** | < 42 ns | ≤ 42 ns | 125 ns |
| std::map baseline | 16.1 M msgs/s | 62.1 ns | 42 ns | 125 ns | 292 ns |

> **Measurement caveat.** On Apple Silicon the finest user-space clock ticks every 41.67 ns (24 MHz), so a single
> message that takes ~10 ns usually measures as 0 or 42 ns. The percentiles above are therefore quantized to that
> tick and include the timer's own overhead: read "p99 ≤ 42 ns" as "99% of messages finished within one timer tick".
> Throughput is measured untimed over the whole replay and is not affected. For real per-message percentiles see the
> x86 table below.

#### x86-64 with RDTSC (GitHub Actions `ubuntu-latest`, 4 vCPU @ 3.1 GHz, GCC, shared VM)

Same generator settings (the flow differs slightly because libstdc++ and libc++ implement the random distributions
differently). `RDTSC` ticks every 0.435 ns here; each sample is bracketed by `LFENCE; RDTSC; LFENCE`, whose
back-to-back overhead is **~20 ns** and is included in every number below (not subtracted). Shared CI VMs are noisy,
so treat tails as indicative. Reproduced on every push by the `replay-benchmark` CI job.

| Book | Throughput | Mean per msg | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|---:|
| **ladder (this engine)** | **50.7 M msgs/s** | **19.7 ns** | **64 ns** | **85 ns** | **139 ns** | **209 ns** |
| std::map baseline | 14.0 M msgs/s | 71.4 ns | 98 ns | 147 ns | 223 ns | 400 ns |

Per message type (ladder): limit p50/p99 63/144 ns, market 61/142 ns, cancel 71/125 ns, reduce 69/130 ns.

### Micro-benchmarks (Google Benchmark, Apple M4, median of 5 repetitions, CV < 3%)

| Benchmark | ladder | std::map | Speed-up |
|---|---:|---:|---:|
| Add passive order + cancel it (per message) | 7.9 ns | 70.6 ns | 8.9× |
| Market order filling one resting order + replenish (per message) | 8.3 ns | 49.6 ns | 6.0× |
| Cancel random order in a 40k-order book + replace (per message) | 33.5 ns | 187 ns | 5.6× |
| Sweep 1 level + refill (per iteration, 5 msgs) | 28.9 ns | 211 ns | 7.3× |
| Sweep 10 levels + refill (per iteration, 41 msgs) | 297 ns | 2,231 ns | 7.5× |
| Sweep 50 levels + refill (per iteration, 201 msgs) | 1,458 ns | 11,548 ns | 7.9× |

On the x86 CI runner the same benchmarks show 2.5–5× speed-ups (e.g. add+cancel 19.6 vs 87.0 ns, deep-book
cancel/replace 67 vs 333 ns).

The deep-book cancel case is dominated by cache misses on random order lookups (40k orders × 32 B nodes plus the id
table exceed L1), which is why it is the slowest per message for both books.

## Build and run

Requires CMake ≥ 3.25 and a C++20 compiler. GoogleTest and Google Benchmark are fetched automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure          # unit + differential tests

./build/lob_gen data/flow.csv 5000000 42 10000      # messages, seed, target book depth
./build/lob_replay data/flow.csv --runs 7           # latency percentiles + throughput, both books
./build/lob_bench                                   # micro-benchmarks

# sanitizers
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZE=ON -DLOB_BUILD_BENCH=OFF
cmake --build build-asan -j && ./build-asan/lob_tests
```

### Replaying real exchange data (LOBSTER)

`lob_replay` also reads [LOBSTER](https://lobsterdata.com) NASDAQ message files (free samples are on their site):

```bash
./build/lob_replay AAPL_2012-06-21_34200000_57600000_message_10.csv --format lobster --tick 100
```

Mapping: submissions → limit orders, partial cancels → `reduce`, deletions → `cancel`, visible executions → a market
order of the opposite side sized to the fill. For each execution the harness checks that the engine filled **the same
resting order id the exchange did** and prints the agreement rate, which is a direct check of price-time priority
against real data. Orders resting before the file starts are unknown to the engine, so messages that refer to them
are no-ops and executions against them can disagree; expect agreement below 100% for that reason.

## API

```cpp
#include "lob/order_book.hpp"

struct Printer { void on_trade(const lob::Trade& t) { /* taker_id, maker_id, price, qty */ } };

lob::OrderBook<Printer> book(lob::BookConfig{.min_price = 1, .max_price = 200'000});
book.add_limit(1, lob::Side::Sell, 10'050, 300);                          // rests
auto r = book.add_limit(2, lob::Side::Buy, 10'050, 500);                  // fills 300, rests 200
book.add_limit(3, lob::Side::Buy, 10'040, 100, lob::TimeInForce::IOC);    // no fill -> Expired
book.add_market(4, lob::Side::Sell, 150);                                 // hits order 2
book.reduce(2, 20);                                                       // keeps queue position
book.cancel(2);
```

Every add returns `AddResult{status, filled, remaining}` where status is one of `Resting`, `Filled`, `Expired`,
`RejectedDuplicateId`, `RejectedInvalidId`, `RejectedInvalidQty`, `RejectedInvalidPrice`.

Order id `lob::kReservedOrderId` (`UINT64_MAX`) is reserved: the id map uses it as its empty-slot marker, so every
entry point rejects it (`RejectedInvalidId`, or `false` from `cancel` / `reduce`). An invalid `BookConfig`
(`min_price > max_price`, or a band wider than 2³¹ ticks) throws `std::invalid_argument` from the constructor in
every build type.

## Layout

```
include/lob/
  types.hpp            Order/Trade/Status types, BookConfig, NullListener
  order_book.hpp       optimized book (price ladder + two-level bitmap + pooled nodes)
  map_order_book.hpp   std::map reference book (oracle + baseline)
  id_map.hpp           open-addressing OrderId -> node index map
  messages.hpp         native CSV + LOBSTER parsers, apply()
  timer.hpp            RDTSC / CNTVCT / steady_clock tick counter
tools/  gen_flow.cpp (synthetic order flow), replay.cpp (latency/throughput harness)
tests/  GoogleTest: behaviour, differential fuzzing, IdMap, parsers
bench/  Google Benchmark micro-benchmarks
```

## Limitations and next steps

- Single instrument, single thread; no network/gateway layer. A realistic next step is an SPSC ring buffer between
  a feed-handler thread and the matching thread, with latency measured end-to-end.
- Prices must fall inside the configured band (rejected otherwise). A production book would recentre or fall back to
  a sparse structure for far-away prices.
- No self-trade prevention, stop orders, iceberg/hidden quantity, or order modify-with-price-change (cancel + new).
