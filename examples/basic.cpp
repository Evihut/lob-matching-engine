// The README's API example, compiled as part of the build so it cannot rot.
#include <cstdio>

#include "lob/order_book.hpp"

struct Printer {
    void on_trade(const lob::Trade& t) {
        std::printf("trade taker=%llu maker=%llu px=%lld qty=%lld\n", (unsigned long long)t.taker_id,
                    (unsigned long long)t.maker_id, (long long)t.price, (long long)t.qty);
    }
};

int main() {
    lob::OrderBook<Printer> book(lob::BookConfig{.min_price = 1, .max_price = 200'000});
    book.add_limit(1, lob::Side::Sell, 10'050, 300);                        // rests
    auto r = book.add_limit(2, lob::Side::Buy, 10'050, 500);                // fills 300, rests 200
    auto ioc = book.add_limit(3, lob::Side::Buy, 10'040, 100, lob::TimeInForce::IOC);  // no fill -> Expired
    book.add_market(4, lob::Side::Sell, 150);                               // hits order 2
    book.reduce(2, 20);                                                     // keeps queue position
    std::printf("order 2: filled=%lld resting=%lld, now %lld left; ioc expired=%d\n", (long long)r.filled,
                (long long)r.remaining, (long long)*book.order_qty(2), ioc.status == lob::Status::Expired);
    book.cancel(2);
    return book.order_count() == 0 ? 0 : 1;
}
