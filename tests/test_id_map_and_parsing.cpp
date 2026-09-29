#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <random>
#include <unordered_map>

#include "lob/id_map.hpp"
#include "lob/messages.hpp"
#include "lob/order_book.hpp"

using namespace lob;

TEST(IdMap, InsertFindErase) {
    IdMap m(4);
    EXPECT_TRUE(m.insert(10, 1));
    EXPECT_FALSE(m.insert(10, 2));
    EXPECT_EQ(m.find(10), 1u);
    EXPECT_EQ(m.find(11), IdMap::kNotFound);
    EXPECT_TRUE(m.erase(10));
    EXPECT_FALSE(m.erase(10));
    EXPECT_EQ(m.find(10), IdMap::kNotFound);
}

TEST(IdMap, MatchesUnorderedMapUnderChurnAndGrowth) {
    IdMap m(8);  // deliberately small so it rehashes many times
    std::unordered_map<OrderId, std::uint32_t> ref;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 300'000; ++i) {
        const OrderId k = rng() % 5000;  // small key space forces long probe runs and collisions
        switch (rng() % 3) {
            case 0: {
                const auto v = static_cast<std::uint32_t>(rng());
                ASSERT_EQ(m.insert(k, v), ref.emplace(k, v).second);
                break;
            }
            case 1: ASSERT_EQ(m.erase(k), ref.erase(k) == 1); break;
            default: {
                auto it = ref.find(k);
                ASSERT_EQ(m.find(k), it == ref.end() ? IdMap::kNotFound : it->second);
            }
        }
        ASSERT_EQ(m.size(), ref.size());
    }
}

TEST(Parsing, NativeFormat) {
    const Msg a = parse_native_line("A,7,B,1000,300");
    EXPECT_EQ(a.type, MsgType::AddLimit);
    EXPECT_EQ(a.id, 7u);
    EXPECT_EQ(a.side, Side::Buy);
    EXPECT_EQ(a.price, 1000);
    EXPECT_EQ(a.qty, 300);
    EXPECT_EQ(a.tif, TimeInForce::GTC);
    EXPECT_EQ(parse_native_line("A,8,S,1001,100,I").tif, TimeInForce::IOC);
    EXPECT_EQ(parse_native_line("M,9,S,500").type, MsgType::AddMarket);
    EXPECT_EQ(parse_native_line("C,7").type, MsgType::Cancel);
    const Msg r = parse_native_line("R,7,100");
    EXPECT_EQ(r.type, MsgType::Reduce);
    EXPECT_EQ(r.qty, 100);
    EXPECT_THROW(parse_native_line("X,1"), std::runtime_error);
    EXPECT_THROW(parse_native_line("A,1,Q,1,1"), std::runtime_error);
}

TEST(Parsing, LobsterReplayReproducesExchangeFills) {
    // Hand-written excerpt in LOBSTER message-file layout.
    const char* path = "lobster_sample_test.csv";
    {
        std::ofstream f(path);
        f << "34200.01,1,101,100,5853300,-1\n"   // ask 585.33 x100
             "34200.02,1,102,200,5853300,-1\n"   // ask 585.33 x200 (behind 101)
             "34200.03,1,201,50,5852000,1\n"     // bid 585.20 x50
             "34200.04,4,101,100,5853300,-1\n"   // exec: buyer hits 101 fully
             "34200.05,2,102,50,5853300,-1\n"    // partial cancel 102 by 50
             "34200.06,4,102,30,5853300,-1\n"    // exec 30 of 102
             "34200.07,5,0,10,5853000,1\n"       // hidden exec: skipped
             "34200.08,3,201,50,5852000,1\n";    // delete bid
    }
    LobsterStats st;
    const auto msgs = load_lobster(path, 100, &st);
    std::remove(path);
    EXPECT_EQ(st.submits, 3u);
    EXPECT_EQ(st.executions, 2u);
    EXPECT_EQ(st.skipped, 1u);
    ASSERT_EQ(msgs.size(), 7u);
    EXPECT_EQ(msgs[0].price, 58533);
    EXPECT_EQ(msgs[3].type, MsgType::AddMarket);
    EXPECT_EQ(msgs[3].side, Side::Buy);  // aggressor is opposite the resting sell
    EXPECT_EQ(msgs[3].expected_maker, 101u);

    struct FirstMaker {
        std::vector<OrderId> makers;
        void on_trade(const Trade& t) { makers.push_back(t.maker_id); }
    };
    OrderBook<FirstMaker> book(BookConfig{58000, 59000, 16});
    for (const Msg& m : msgs) apply(book, m);
    EXPECT_EQ(book.listener().makers, (std::vector<OrderId>{101, 102}));
    EXPECT_EQ(*book.order_qty(102), 120);  // 200 - 50 cancelled - 30 executed
    EXPECT_FALSE(book.best_bid());
}
