#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"

using namespace lob;

namespace {

struct Recorder {
    std::vector<Trade> trades;
    void on_trade(const Trade& t) { trades.push_back(t); }
};

template <class Book>
class OrderBookTest : public ::testing::Test {
protected:
    Book book{BookConfig{1, 1000, 64}};
    std::vector<Trade>& trades() { return book.listener().trades; }
};

using Books = ::testing::Types<OrderBook<Recorder>, MapOrderBook<Recorder>>;

class BookNames {
public:
    template <class T>
    static std::string GetName(int) {
        return std::is_same_v<T, OrderBook<Recorder>> ? "Ladder" : "Map";
    }
};

TYPED_TEST_SUITE(OrderBookTest, Books, BookNames);

constexpr auto B = Side::Buy;
constexpr auto S = Side::Sell;

}  // namespace

TYPED_TEST(OrderBookTest, EmptyBookHasNoPrices) {
    EXPECT_FALSE(this->book.best_bid());
    EXPECT_FALSE(this->book.best_ask());
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, NonCrossingOrdersRest) {
    EXPECT_EQ(this->book.add_limit(1, B, 99, 10), (AddResult{Status::Resting, 0, 10}));
    EXPECT_EQ(this->book.add_limit(2, S, 101, 5), (AddResult{Status::Resting, 0, 5}));
    EXPECT_EQ(this->book.add_limit(3, B, 100, 7).status, Status::Resting);
    EXPECT_EQ(*this->book.best_bid(), 100);
    EXPECT_EQ(*this->book.best_ask(), 101);
    EXPECT_EQ(this->book.order_count(), 3u);
    EXPECT_TRUE(this->trades().empty());
}

TYPED_TEST(OrderBookTest, CrossTradesAtMakerPrice) {
    this->book.add_limit(1, S, 100, 10);
    const auto r = this->book.add_limit(2, B, 105, 4);
    EXPECT_EQ(r, (AddResult{Status::Filled, 4, 0}));
    ASSERT_EQ(this->trades().size(), 1u);
    EXPECT_EQ(this->trades()[0], (Trade{2, 1, B, 100, 4}));
    EXPECT_EQ(*this->book.order_qty(1), 6);
}

TYPED_TEST(OrderBookTest, PricePriorityBeatsTimePriority) {
    this->book.add_limit(1, S, 102, 5);
    this->book.add_limit(2, S, 101, 5);  // later but better price
    this->book.add_market(3, B, 5);
    ASSERT_EQ(this->trades().size(), 1u);
    EXPECT_EQ(this->trades()[0].maker_id, 2u);
    EXPECT_EQ(this->trades()[0].price, 101);
}

TYPED_TEST(OrderBookTest, TimePriorityWithinLevel) {
    this->book.add_limit(1, B, 100, 5);
    this->book.add_limit(2, B, 100, 5);
    this->book.add_limit(3, B, 100, 5);
    this->book.add_market(4, S, 12);
    ASSERT_EQ(this->trades().size(), 3u);
    EXPECT_EQ(this->trades()[0], (Trade{4, 1, S, 100, 5}));
    EXPECT_EQ(this->trades()[1], (Trade{4, 2, S, 100, 5}));
    EXPECT_EQ(this->trades()[2], (Trade{4, 3, S, 100, 2}));
    EXPECT_EQ(*this->book.order_qty(3), 3);
    EXPECT_FALSE(this->book.order_qty(1));
}

TYPED_TEST(OrderBookTest, LimitSweepsUpToPriceThenRestsRemainder) {
    this->book.add_limit(1, S, 100, 3);
    this->book.add_limit(2, S, 101, 3);
    this->book.add_limit(3, S, 103, 3);
    const auto r = this->book.add_limit(4, B, 101, 10);
    EXPECT_EQ(r, (AddResult{Status::Resting, 6, 4}));
    EXPECT_EQ(this->trades().size(), 2u);
    EXPECT_EQ(*this->book.best_bid(), 101);
    EXPECT_EQ(*this->book.best_ask(), 103);
    EXPECT_EQ(this->book.volume_at(101), 4);
}

TYPED_TEST(OrderBookTest, MarketOrderSweepsAndExpiresRemainder) {
    this->book.add_limit(1, S, 100, 3);
    this->book.add_limit(2, S, 110, 3);
    const auto r = this->book.add_market(3, B, 10);
    EXPECT_EQ(r, (AddResult{Status::Expired, 6, 4}));
    EXPECT_FALSE(this->book.best_ask());
    EXPECT_FALSE(this->book.best_bid());  // market remainder never rests
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, MarketOrderOnEmptyBookExpires) {
    EXPECT_EQ(this->book.add_market(1, S, 5), (AddResult{Status::Expired, 0, 5}));
    EXPECT_TRUE(this->trades().empty());
}

TYPED_TEST(OrderBookTest, IocLimitDoesNotRest) {
    this->book.add_limit(1, S, 100, 3);
    EXPECT_EQ(this->book.add_limit(2, B, 100, 5, TimeInForce::IOC), (AddResult{Status::Expired, 3, 2}));
    EXPECT_FALSE(this->book.best_bid());
    EXPECT_EQ(this->book.add_limit(3, B, 99, 5, TimeInForce::IOC), (AddResult{Status::Expired, 0, 5}));
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, CancelRemovesOrderAndUpdatesBest) {
    this->book.add_limit(1, B, 100, 5);
    this->book.add_limit(2, B, 98, 5);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_EQ(*this->book.best_bid(), 98);
    EXPECT_FALSE(this->book.cancel(1));  // already gone
    EXPECT_FALSE(this->book.cancel(42)); // never existed
    EXPECT_TRUE(this->book.cancel(2));
    EXPECT_FALSE(this->book.best_bid());
}

TYPED_TEST(OrderBookTest, CancelMiddleOfQueueKeepsOthersInOrder) {
    this->book.add_limit(1, S, 100, 1);
    this->book.add_limit(2, S, 100, 1);
    this->book.add_limit(3, S, 100, 1);
    EXPECT_TRUE(this->book.cancel(2));
    this->book.add_market(4, B, 2);
    ASSERT_EQ(this->trades().size(), 2u);
    EXPECT_EQ(this->trades()[0].maker_id, 1u);
    EXPECT_EQ(this->trades()[1].maker_id, 3u);
}

TYPED_TEST(OrderBookTest, ReduceKeepsQueuePriority) {
    this->book.add_limit(1, S, 100, 10);
    this->book.add_limit(2, S, 100, 10);
    EXPECT_TRUE(this->book.reduce(1, 7));
    EXPECT_EQ(*this->book.order_qty(1), 3);
    EXPECT_EQ(this->book.volume_at(100), 13);
    this->book.add_market(3, B, 3);
    EXPECT_EQ(this->trades().back().maker_id, 1u);
}

TYPED_TEST(OrderBookTest, ReduceByFullQtyCancels) {
    this->book.add_limit(1, S, 100, 10);
    EXPECT_TRUE(this->book.reduce(1, 10));
    EXPECT_FALSE(this->book.best_ask());
    EXPECT_FALSE(this->book.reduce(1, 1));
    EXPECT_FALSE(this->book.reduce(99, 1));
}

TYPED_TEST(OrderBookTest, RejectsInvalidInput) {
    EXPECT_EQ(this->book.add_limit(1, B, 100, 0).status, Status::RejectedInvalidQty);
    EXPECT_EQ(this->book.add_limit(1, B, 100, -5).status, Status::RejectedInvalidQty);
    EXPECT_EQ(this->book.add_limit(1, B, 0, 5).status, Status::RejectedInvalidPrice);
    EXPECT_EQ(this->book.add_limit(1, B, 1001, 5).status, Status::RejectedInvalidPrice);
    EXPECT_EQ(this->book.add_market(1, B, 0).status, Status::RejectedInvalidQty);
    EXPECT_EQ(this->book.add_limit(1, B, 100, 5).status, Status::Resting);
    EXPECT_EQ(this->book.add_limit(1, S, 200, 5).status, Status::RejectedDuplicateId);
    EXPECT_EQ(this->book.order_count(), 1u);
}

TYPED_TEST(OrderBookTest, IdCanBeReusedAfterOrderLeavesBook) {
    this->book.add_limit(1, B, 100, 5);
    this->book.add_market(2, S, 5);
    EXPECT_EQ(this->book.add_limit(1, B, 100, 5).status, Status::Resting);
}

TYPED_TEST(OrderBookTest, BandEdgesAreUsable) {
    EXPECT_EQ(this->book.add_limit(1, B, 1, 5).status, Status::Resting);
    EXPECT_EQ(this->book.add_limit(2, S, 1000, 5).status, Status::Resting);
    EXPECT_EQ(*this->book.best_bid(), 1);
    EXPECT_EQ(*this->book.best_ask(), 1000);
    this->book.add_market(3, S, 5);
    this->book.add_market(4, B, 5);
    EXPECT_FALSE(this->book.best_bid());
    EXPECT_FALSE(this->book.best_ask());
}

TYPED_TEST(OrderBookTest, BestPriceRecoversAcrossBitmapWords) {
    // Levels 64+ ticks apart live in different bitmap words.
    this->book.add_limit(1, S, 900, 1);
    this->book.add_limit(2, S, 130, 1);
    this->book.add_limit(3, B, 5, 1);
    this->book.add_limit(4, B, 70, 1);
    EXPECT_TRUE(this->book.cancel(2));
    EXPECT_EQ(*this->book.best_ask(), 900);
    EXPECT_TRUE(this->book.cancel(4));
    EXPECT_EQ(*this->book.best_bid(), 5);
}

TEST(LadderBitmap, BestPriceAcrossSummaryWordsInWideBand) {
    // A 1M-tick band spans many summary words; exercise far jumps both ways.
    OrderBook<> book(BookConfig{1, 1'000'000, 16});
    book.add_limit(1, Side::Sell, 999'999, 1);
    book.add_limit(2, Side::Sell, 5'000, 1);
    book.add_limit(3, Side::Buy, 2, 1);
    book.add_limit(4, Side::Buy, 4'999, 1);
    EXPECT_TRUE(book.cancel(2));
    EXPECT_EQ(*book.best_ask(), 999'999);
    EXPECT_TRUE(book.cancel(4));
    EXPECT_EQ(*book.best_bid(), 2);
    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.best_ask());
    EXPECT_TRUE(book.cancel(3));
    EXPECT_FALSE(book.best_bid());
}

// Regression: kReservedOrderId (UINT64_MAX) is IdMap's empty-slot marker. Before
// it was rejected, cancel() on an empty book "found" an empty slot and indexed a
// node that did not exist (out-of-bounds read, caught by ASan / hardened libc++).
TYPED_TEST(OrderBookTest, ReservedOrderIdIsRejectedEverywhere) {
    EXPECT_FALSE(this->book.cancel(kReservedOrderId));  // empty book: the original crash
    EXPECT_FALSE(this->book.reduce(kReservedOrderId, 1));
    EXPECT_FALSE(this->book.order_qty(kReservedOrderId));
    EXPECT_EQ(this->book.add_limit(kReservedOrderId, B, 100, 5).status, Status::RejectedInvalidId);
    EXPECT_EQ(this->book.add_market(kReservedOrderId, S, 5).status, Status::RejectedInvalidId);

    this->book.add_limit(1, B, 100, 5);
    this->book.add_limit(2, S, 101, 5);
    EXPECT_FALSE(this->book.cancel(kReservedOrderId));  // non-empty book
    EXPECT_EQ(this->book.add_limit(kReservedOrderId, S, 100, 5).status, Status::RejectedInvalidId);
    EXPECT_TRUE(this->trades().empty());  // the rejected crossing order must not trade
    EXPECT_EQ(this->book.order_count(), 2u);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_TRUE(this->book.cancel(2));
}

TYPED_TEST(OrderBookTest, LargestNonReservedIdWorks) {
    const OrderId id = kReservedOrderId - 1;
    EXPECT_EQ(this->book.add_limit(id, B, 100, 5).status, Status::Resting);
    EXPECT_EQ(*this->book.order_qty(id), 5);
    EXPECT_TRUE(this->book.cancel(id));
}

TYPED_TEST(OrderBookTest, InvalidConfigThrowsInEveryBuildType) {
    using Book = TypeParam;
    EXPECT_THROW(Book(BookConfig{10, 9, 16}), std::invalid_argument);
    EXPECT_THROW(Book(BookConfig{0, BookConfig::kMaxLevels, 16}), std::invalid_argument);  // 2^31 + 1 levels
    EXPECT_THROW(Book(BookConfig{INT64_MIN, INT64_MAX, 16}), std::invalid_argument);       // no overflow
    EXPECT_NO_THROW(Book(BookConfig{5, 5, 16}));                                           // single level
    EXPECT_NO_THROW(Book(BookConfig{-100, 100, 16}));                                      // negative ticks allowed
}
