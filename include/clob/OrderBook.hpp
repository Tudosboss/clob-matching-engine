#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace clob {

using Price    = int64_t;   // integer ticks
using Quantity = uint32_t;
using OrderId  = uint64_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

enum class Reject : uint8_t { None, BadPrice, BadQuantity, DuplicateId, BookFull };

struct AddResult {
    uint64_t filled = 0;          // quantity executed immediately
    Quantity rested = 0;          // quantity left resting on the book
    Reject reject   = Reject::None;
};

// Default listener does nothing. Pass your own type to get a call for every trade.
struct NullListener {
    void on_trade(OrderId /*taker*/, OrderId /*maker*/, Price /*price*/, Quantity /*qty*/) noexcept {}
};

constexpr uint32_t NULL_IDX = 0xFFFFFFFFu;

// One order in the pool. prev/next link it into its price level's queue.
struct alignas(32) Order {
    OrderId id        = 0;
    Quantity quantity = 0;
    uint32_t level    = 0;          // price level index (price - BasePrice)
    uint32_t prev_idx = NULL_IDX;
    uint32_t next_idx = NULL_IDX;
    Side side         = Side::Buy;
};
static_assert(sizeof(Order) == 32, "Order must stay at 32 bytes (two per cache line)");

// Queue of orders at one price
struct PriceLevel {
    uint64_t total_volume = 0;
    uint32_t order_count  = 0;
    uint32_t head_idx     = NULL_IDX;
    uint32_t tail_idx     = NULL_IDX;
};

// The book is several MB, so create it with std::make_unique, not on the stack.
template <Price BasePrice = 700000, size_t MaxLevels = 2048, size_t MaxOrders = 131072,
          typename Listener = NullListener>
class OrderBook {
    static_assert(std::has_single_bit(MaxOrders), "MaxOrders must be a power of 2");
    static_assert(MaxOrders < NULL_IDX, "slot indices are 32-bit");
    static_assert(MaxLevels % 64 == 0 && MaxLevels < NULL_IDX, "MaxLevels must be a multiple of 64");

public:
    static constexpr Price MinPrice = BasePrice;
    static constexpr Price MaxPrice = BasePrice + static_cast<Price>(MaxLevels) - 1;

private:
    static constexpr uint32_t NONE     = NULL_IDX;
    static constexpr size_t HashSize   = MaxOrders * 2;   // load factor <= 0.5
    static constexpr size_t HashMask   = HashSize - 1;
    static constexpr int    HashShift  = 64 - std::countr_zero(HashSize);
    static constexpr size_t Words      = MaxLevels / 64;

    struct IdBucket {
        OrderId id    = 0;
        uint32_t slot = NULL_IDX;
    };

    // One side of the book. Bit i of `bits` is 1 when level i has orders, which lets us
    // find the next best price by checking 64 levels at a time.
    struct BookSide {
        std::array<PriceLevel, MaxLevels> levels{};
        std::array<uint64_t, Words> bits{};
        uint32_t best = NONE;
    };

    BookSide bids_;
    BookSide asks_;
    std::array<Order, MaxOrders> orders_{};
    std::array<IdBucket, HashSize> id_map_{};

    uint32_t free_head_   = 0;
    uint32_t live_orders_ = 0;
    Listener listener_{};

    template <Side S> BookSide& book() noexcept {
        if constexpr (S == Side::Buy) return bids_; else return asks_;
    }

    // Order ID -> pool slot (open addressing, linear probing)

    // Fibonacci hashing: take the top bits of the product
    [[nodiscard]] static constexpr size_t hash_id(OrderId id) noexcept {
        return static_cast<size_t>((id * 0x9E3779B97F4A7C15ULL) >> HashShift);
    }

    void map_insert(OrderId id, uint32_t slot) noexcept {
        size_t idx = hash_id(id);
        while (id_map_[idx].slot != NULL_IDX) idx = (idx + 1) & HashMask;
        id_map_[idx] = {id, slot};
    }

    [[nodiscard]] uint32_t map_find(OrderId id) const noexcept {
        size_t idx = hash_id(id);
        while (id_map_[idx].slot != NULL_IDX) {
            if (id_map_[idx].id == id) return id_map_[idx].slot;
            idx = (idx + 1) & HashMask;
        }
        return NULL_IDX;
    }

    // Shift later entries back instead of leaving tombstones
    void map_erase(OrderId id) noexcept {
        size_t i = hash_id(id);
        while (id_map_[i].slot != NULL_IDX && id_map_[i].id != id) i = (i + 1) & HashMask;
        if (id_map_[i].slot == NULL_IDX) return;

        size_t j = (i + 1) & HashMask;
        while (id_map_[j].slot != NULL_IDX) {
            size_t k = hash_id(id_map_[j].id);
            if (((j - k) & HashMask) >= ((j - i) & HashMask)) {
                id_map_[i] = id_map_[j];
                i = j;
            }
            j = (j + 1) & HashMask;
        }
        id_map_[i] = {0, NULL_IDX};
    }

    [[nodiscard]] uint32_t alloc_slot() noexcept {
        if (free_head_ == NULL_IDX) return NULL_IDX;
        uint32_t slot = free_head_;
        free_head_ = orders_[slot].next_idx;
        ++live_orders_;
        return slot;
    }

    void free_slot(uint32_t slot) noexcept {
        orders_[slot].next_idx = free_head_;
        free_head_ = slot;
        --live_orders_;
    }

    static void set_bit(BookSide& b, uint32_t lvl) noexcept   { b.bits[lvl >> 6] |=  (1ULL << (lvl & 63)); }
    static void clear_bit(BookSide& b, uint32_t lvl) noexcept { b.bits[lvl >> 6] &= ~(1ULL << (lvl & 63)); }

    // Lowest non-empty level >= from, or NONE
    [[nodiscard]] static uint32_t find_up(const BookSide& b, uint32_t from) noexcept {
        if (from >= MaxLevels) return NONE;
        size_t w = from >> 6;
        uint64_t word = b.bits[w] & (~0ULL << (from & 63));
        for (;;) {
            if (word) return static_cast<uint32_t>(w * 64 + std::countr_zero(word));
            if (++w == Words) return NONE;
            word = b.bits[w];
        }
    }

    // Highest non-empty level <= from, or NONE
    [[nodiscard]] static uint32_t find_down(const BookSide& b, uint32_t from) noexcept {
        size_t w = from >> 6;
        uint64_t word = b.bits[w] & (~0ULL >> (63 - (from & 63)));
        for (;;) {
            if (word) return static_cast<uint32_t>(w * 64 + 63 - std::countl_zero(word));
            if (w-- == 0) return NONE;
            word = b.bits[w];
        }
    }

    // Called when level `lvl` on side S has just become empty
    template <Side S>
    void level_emptied(uint32_t lvl) noexcept {
        BookSide& b = book<S>();
        clear_bit(b, lvl);
        if (b.best != lvl) return;
        if constexpr (S == Side::Sell) b.best = find_up(b, lvl + 1);
        else                           b.best = (lvl == 0) ? NONE : find_down(b, lvl - 1);
    }

    void unlink_order(PriceLevel& level, uint32_t slot) noexcept {
        Order& ord = orders_[slot];
        if (ord.prev_idx != NULL_IDX) orders_[ord.prev_idx].next_idx = ord.next_idx;
        else level.head_idx = ord.next_idx;

        if (ord.next_idx != NULL_IDX) orders_[ord.next_idx].prev_idx = ord.prev_idx;
        else level.tail_idx = ord.prev_idx;

        level.total_volume -= ord.quantity;
        --level.order_count;
    }

    template <Side S>
    [[nodiscard]] static bool crosses(uint32_t opp_best, uint32_t limit) noexcept {
        if (opp_best == NONE) return false;
        if constexpr (S == Side::Buy) return opp_best <= limit;
        else                          return opp_best >= limit;
    }

    // Match an incoming order against the other side, up to price level `limit`.
    // Reduces qty by the amount traded and returns that amount.
    template <Side S>
    uint64_t match(OrderId taker, Quantity& qty, uint32_t limit) noexcept {
        constexpr Side Opp = (S == Side::Buy) ? Side::Sell : Side::Buy;
        BookSide& opp = book<Opp>();
        uint64_t filled = 0;

        while (qty > 0 && crosses<S>(opp.best, limit)) {
            const uint32_t lvl_idx = opp.best;
            PriceLevel& lvl = opp.levels[lvl_idx];
            const Price px = BasePrice + static_cast<Price>(lvl_idx);

            while (qty > 0 && lvl.head_idx != NULL_IDX) {
                const uint32_t slot = lvl.head_idx;
                Order& resting = orders_[slot];
                const Quantity fill = std::min(qty, resting.quantity);
                qty -= fill;
                resting.quantity -= fill;
                lvl.total_volume -= fill;
                filled += fill;
                listener_.on_trade(taker, resting.id, px, fill);

                if (resting.quantity == 0) {
                    unlink_order(lvl, slot);
                    map_erase(resting.id);
                    free_slot(slot);
                }
            }
            if (lvl.head_idx == NULL_IDX) level_emptied<Opp>(lvl_idx);
        }
        return filled;
    }

    template <Side S>
    AddResult add_limit(OrderId id, uint32_t lvl_idx, Quantity qty) noexcept {
        AddResult r;
        r.filled = match<S>(id, qty, lvl_idx);
        if (qty == 0) return r;

        const uint32_t slot = alloc_slot();
        if (slot == NULL_IDX) {            // pool is full: drop what is left
            r.reject = Reject::BookFull;
            return r;
        }

        BookSide& b = book<S>();
        PriceLevel& lvl = b.levels[lvl_idx];
        orders_[slot] = Order{id, qty, lvl_idx, lvl.tail_idx, NULL_IDX, S};
        if (lvl.tail_idx != NULL_IDX) orders_[lvl.tail_idx].next_idx = slot;
        else { lvl.head_idx = slot; set_bit(b, lvl_idx); }
        lvl.tail_idx = slot;
        lvl.total_volume += qty;
        ++lvl.order_count;
        map_insert(id, slot);

        if constexpr (S == Side::Buy) { if (b.best == NONE || lvl_idx > b.best) b.best = lvl_idx; }
        else                          { if (b.best == NONE || lvl_idx < b.best) b.best = lvl_idx; }

        r.rested = qty;
        return r;
    }

    template <Side S>
    void remove_resting(uint32_t slot) noexcept {
        const uint32_t lvl_idx = orders_[slot].level;
        PriceLevel& lvl = book<S>().levels[lvl_idx];
        unlink_order(lvl, slot);
        if (lvl.head_idx == NULL_IDX) level_emptied<S>(lvl_idx);
    }

public:
    OrderBook() noexcept { reset(); }

    void reset() noexcept {
        for (BookSide* b : {&bids_, &asks_}) {
            b->levels.fill(PriceLevel{});
            b->bits.fill(0);
            b->best = NONE;
        }
        id_map_.fill(IdBucket{0, NULL_IDX});
        for (uint32_t i = 0; i < MaxOrders; ++i) {
            orders_[i].next_idx = (i + 1 < MaxOrders) ? (i + 1) : NULL_IDX;
        }
        free_head_ = 0;
        live_orders_ = 0;
    }

    // Limit order: trades what it can, the rest waits in the book until cancelled.
    AddResult add_order(OrderId id, Side side, Price price, Quantity qty) noexcept {
        if (price < MinPrice || price > MaxPrice) return {0, 0, Reject::BadPrice};
        if (qty == 0) return {0, 0, Reject::BadQuantity};
        if (map_find(id) != NULL_IDX) return {0, 0, Reject::DuplicateId};

        const auto lvl_idx = static_cast<uint32_t>(price - BasePrice);
        return side == Side::Buy ? add_limit<Side::Buy>(id, lvl_idx, qty)
                                 : add_limit<Side::Sell>(id, lvl_idx, qty);
    }

    // Market order: trades at any price, anything left over is dropped.
    AddResult add_market_order(OrderId id, Side side, Quantity qty) noexcept {
        if (qty == 0) return {0, 0, Reject::BadQuantity};
        if (map_find(id) != NULL_IDX) return {0, 0, Reject::DuplicateId};

        AddResult r;
        r.filled = side == Side::Buy ? match<Side::Buy>(id, qty, MaxLevels - 1)
                                     : match<Side::Sell>(id, qty, 0);
        return r;
    }

    // Cancel by order ID
    bool cancel_order(OrderId id) noexcept {
        const uint32_t slot = map_find(id);
        if (slot == NULL_IDX) return false;

        if (orders_[slot].side == Side::Buy) remove_resting<Side::Buy>(slot);
        else                                 remove_resting<Side::Sell>(slot);
        map_erase(id);
        free_slot(slot);
        return true;
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        if (bids_.best == NONE) return std::nullopt;
        return BasePrice + static_cast<Price>(bids_.best);
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        if (asks_.best == NONE) return std::nullopt;
        return BasePrice + static_cast<Price>(asks_.best);
    }

    [[nodiscard]] uint64_t volume_at(Side side, Price price) const noexcept {
        if (price < MinPrice || price > MaxPrice) return 0;
        const auto& b = (side == Side::Buy) ? bids_ : asks_;
        return b.levels[static_cast<size_t>(price - BasePrice)].total_volume;
    }

    [[nodiscard]] uint32_t live_orders() const noexcept { return live_orders_; }
    [[nodiscard]] static constexpr size_t capacity() noexcept { return MaxOrders; }

    Listener& listener() noexcept { return listener_; }
};

} // namespace clob
