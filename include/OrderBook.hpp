#pragma once

#include <cstdint>
#include <array>

namespace clob {

// Financial exchanges represent prices as 64-bit integers (in "ticks", e.g., $70,000.50 -> 7000050)
// to avoid IEEE-754 floating-point precision errors and slow FPU operations.
using Price = int64_t;
using Quantity = uint64_t;
using OrderId = uint64_t;
using Timestamp = uint64_t;

enum class Side : uint8_t {
    Buy = 0,
    Sell = 1
};

// Represents a single resting limit order in the book (32 bytes total)
struct alignas(32) Order {
    OrderId id          = 0;
    Price price         = 0;
    Quantity quantity   = 0;
    Timestamp timestamp = 0;
    Side side           = Side::Buy;
    bool active         = false;
};

// Aligned to 64 bytes (exact size of one CPU L1 cache line) to prevent false sharing
// and ensure a single memory fetch loads the entire price level summary.
struct alignas(64) PriceLevel {
    Price price             = 0;
    Quantity total_volume   = 0;
    uint32_t order_count    = 0;
    uint32_t head_order_idx = 0; // Index into pre-allocated Order pool for FIFO matching
    uint32_t tail_order_idx = 0;
};

// Pre-allocated, zero-heap-allocation Limit Order Book skeleton
template <size_t MaxLevels = 1024, size_t MaxOrders = 65536>
class OrderBook {
private:
    // Contiguous arrays stored directly in pre-allocated memory (zero runtime malloc/new)
    std::array<PriceLevel, MaxLevels> bids_{};
    std::array<PriceLevel, MaxLevels> asks_{};
    std::array<Order, MaxOrders> order_pool_{};

    size_t best_bid_idx_ = 0;
    size_t best_ask_idx_ = 0;

public:
    OrderBook() = default;

    // O(1) lookup for top-of-book Best Bid and Best Ask
    [[nodiscard]] const PriceLevel& best_bid() const noexcept {
        return bids_[best_bid_idx_];
    }

    [[nodiscard]] const PriceLevel& best_ask() const noexcept {
        return asks_[best_ask_idx_];
    }

    // Spread in integer ticks between Best Ask and Best Bid
    [[nodiscard]] Price spread() const noexcept {
        return asks_[best_ask_idx_].price - bids_[best_bid_idx_].price;
    }
};

} // namespace clob
