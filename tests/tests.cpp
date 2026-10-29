// Unit tests, plus random orders checked against MapOrderBook.
#include <clob/OrderBook.hpp>
#include <clob/MapOrderBook.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

using namespace clob;

static int failures = 0;
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

struct Trade {
    OrderId taker, maker;
    Price price;
    Quantity qty;
    bool operator==(const Trade&) const = default;
};

struct RecordingListener {
    std::vector<Trade> trades;
    void on_trade(OrderId taker, OrderId maker, Price price, Quantity qty) {
        trades.push_back({taker, maker, price, qty});
    }
};

using Book = OrderBook<1000, 128, 16, RecordingListener>;   // small book: easy to fill and to hit the edges

static void test_price_time_priority() {
    auto b = std::make_unique<Book>();
    b->add_order(1, Side::Sell, 1010, 5);
    b->add_order(2, Side::Sell, 1010, 5);
    b->add_order(3, Side::Sell, 1005, 5);
    auto r = b->add_order(4, Side::Buy, 1010, 12);
    CHECK(r.filled == 12 && r.rested == 0);
    const std::vector<Trade> expect = {{4, 3, 1005, 5}, {4, 1, 1010, 5}, {4, 2, 1010, 2}};
    CHECK(b->listener().trades == expect);   // better price first, then FIFO within a level
    CHECK(b->best_ask() == 1010);
    CHECK(b->volume_at(Side::Sell, 1010) == 3);
}

static void test_partial_fill_rests_remainder() {
    auto b = std::make_unique<Book>();
    b->add_order(1, Side::Sell, 1050, 4);
    auto r = b->add_order(2, Side::Buy, 1060, 10);
    CHECK(r.filled == 4 && r.rested == 6);
    CHECK(b->best_bid() == 1060);
    CHECK(!b->best_ask().has_value());
}

static void test_cancel_best_level_moves_best() {
    auto b = std::make_unique<Book>();
    b->add_order(1, Side::Buy, 1020, 1);
    b->add_order(2, Side::Buy, 1001, 1);     // far away, different bitmap word
    b->add_order(3, Side::Sell, 1100, 1);
    b->add_order(4, Side::Sell, 1127, 1);
    CHECK(b->cancel_order(1));
    CHECK(b->best_bid() == 1001);
    CHECK(b->cancel_order(3));
    CHECK(b->best_ask() == 1127);
    CHECK(!b->cancel_order(3));               // already gone
    CHECK(b->cancel_order(2) && b->cancel_order(4));
    CHECK(!b->best_bid() && !b->best_ask());
    CHECK(b->live_orders() == 0);
}

static void test_market_orders() {
    auto b = std::make_unique<Book>();
    b->add_order(1, Side::Buy, 1000, 3);      // lowest valid price
    b->add_order(2, Side::Buy, 1127, 3);      // highest valid price
    auto r = b->add_market_order(3, Side::Sell, 10);
    CHECK(r.filled == 6 && r.rested == 0);    // IOC: remainder is not rested
    CHECK(!b->best_bid() && !b->best_ask());
    CHECK(b->add_market_order(4, Side::Buy, 5).filled == 0);
}

static void test_rejects() {
    auto b = std::make_unique<Book>();
    CHECK(b->add_order(1, Side::Buy, 999, 1).reject == Reject::BadPrice);
    CHECK(b->add_order(1, Side::Buy, 1128, 1).reject == Reject::BadPrice);
    CHECK(b->add_order(1, Side::Buy, 1050, 0).reject == Reject::BadQuantity);
    CHECK(b->add_order(1, Side::Buy, 1050, 1).reject == Reject::None);
    CHECK(b->add_order(1, Side::Sell, 1050, 1).reject == Reject::DuplicateId);
    CHECK(b->best_bid() == 1050);             // the duplicate did not trade
}

static void test_pool_exhaustion() {
    auto b = std::make_unique<Book>();
    for (OrderId id = 1; id <= Book::capacity(); ++id) CHECK(b->add_order(id, Side::Buy, 1000, 1).rested == 1);
    auto r = b->add_order(100, Side::Buy, 1001, 1);
    CHECK(r.reject == Reject::BookFull && r.rested == 0);
    CHECK(b->best_bid() == 1000);
    // an aggressive order can still trade when the pool is full
    auto s = b->add_order(101, Side::Sell, 1000, 2);
    CHECK(s.filled == 2 && s.reject == Reject::None);
    CHECK(b->live_orders() == Book::capacity() - 2);
}

// Send the same random orders to both books and check they always agree.
static void test_against_reference(uint64_t seed, size_t events) {
    using Big = OrderBook<700000, 2048, 1 << 17, RecordingListener>;
    auto fast = std::make_unique<Big>();
    MapOrderBook<RecordingListener> ref;
    std::mt19937_64 rng(seed);
    std::vector<OrderId> ids;
    OrderId next_id = 1'000'000'001ULL;

    for (size_t i = 0; i < events; ++i) {
        const unsigned roll = rng() % 100;
        if (roll < 35 && !ids.empty()) {
            size_t k = rng() % ids.size();
            OrderId id = ids[k];
            ids[k] = ids.back();
            ids.pop_back();
            CHECK(fast->cancel_order(id) == ref.cancel_order(id));
        } else if (roll < 40) {
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Quantity qty = 1 + rng() % 200;
            OrderId id = next_id++;
            CHECK(fast->add_market_order(id, side, qty).filled == ref.add_market_order(id, side, qty));
        } else {
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Price price = 700000 + static_cast<Price>(rng() % 2048);
            Quantity qty = 1 + rng() % 50;
            OrderId id = next_id++;
            auto r = fast->add_order(id, side, price, qty);
            CHECK(r.reject == Reject::None);
            CHECK(r.filled == ref.add_order(id, side, price, qty));
            ids.push_back(id);
        }
        CHECK(fast->best_bid() == ref.best_bid());
        CHECK(fast->best_ask() == ref.best_ask());
        if (failures) return;
    }
    CHECK(fast->listener().trades == ref.listener().trades);
    CHECK(fast->live_orders() == ref.live_orders());
    std::printf("  seed %llu: %zu events, %zu trades, same in both books\n",
                static_cast<unsigned long long>(seed), events, fast->listener().trades.size());
}

int main() {
    test_price_time_priority();
    test_partial_fill_rests_remainder();
    test_cancel_best_level_moves_best();
    test_market_orders();
    test_rejects();
    test_pool_exhaustion();
    for (uint64_t seed : {1, 2, 3}) test_against_reference(seed, 300'000);

    if (failures) { std::printf("%d check(s) failed\n", failures); return EXIT_FAILURE; }
    std::printf("All tests passed\n");
    return EXIT_SUCCESS;
}
