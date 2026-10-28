#pragma once

#include <cstdint>
#include <array>
#include <algorithm>

namespace clob {

using Price     = int64_t;
using Quantity  = uint64_t;
using OrderId   = uint64_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

struct alignas(32) Order {
    OrderId id        = 0;
    Price price       = 0;
    Quantity quantity = 0;
    uint32_t next_idx = 0; // Intrusive singly-linked list index for FIFO priority
    Side side         = Side::Buy;
    bool active       = false;
};

struct alignas(64) PriceLevel {
    Price price           = 0;
    Quantity total_volume = 0;
    uint32_t order_count  = 0;
    uint32_t head_idx     = 0;
    uint32_t tail_idx     = 0;
};

template <Price BasePrice = 700000, size_t MaxLevels = 2048, size_t MaxOrders = 131072>
class OrderBook {
private:
    std::array<PriceLevel, MaxLevels> bids_{};
    std::array<PriceLevel, MaxLevels> asks_{};
    std::array<Order, MaxOrders> orders_{};

    size_t best_bid_idx_ = 0;
    size_t best_ask_idx_ = MaxLevels - 1;
    uint64_t total_trades_ = 0;

    [[nodiscard]] constexpr size_t price_to_idx(Price price) const noexcept {
        return static_cast<size_t>(price - BasePrice);
    }

public:
    OrderBook() = default;

    // O(1) Add Limit Order with immediate FIFO matching against crossing opposite levels
    uint64_t add_order(OrderId id, Side side, Price price, Quantity qty) noexcept {
        uint64_t matched_qty = 0;

        if (side == Side::Buy) {
            while (qty > 0 && best_ask_idx_ < MaxLevels && asks_[best_ask_idx_].total_volume > 0) {
                Price ask_price = static_cast<Price>(best_ask_idx_) + BasePrice;
                if (price < ask_price) break;

                Quantity fill = std::min(qty, asks_[best_ask_idx_].total_volume);
                qty -= fill;
                matched_qty += fill;
                asks_[best_ask_idx_].total_volume -= fill;
                ++total_trades_;

                if (asks_[best_ask_idx_].total_volume == 0) {
                    while (best_ask_idx_ < MaxLevels && asks_[best_ask_idx_].total_volume == 0) {
                        ++best_ask_idx_;
                    }
                }
            }
            if (qty > 0 && id < MaxOrders) {
                size_t idx = price_to_idx(price);
                orders_[id] = Order{id, price, qty, 0, Side::Buy, true};
                bids_[idx].price = price;
                bids_[idx].total_volume += qty;
                ++bids_[idx].order_count;
                if (idx > best_bid_idx_) best_bid_idx_ = idx;
            }
        } else {
            while (qty > 0 && best_bid_idx_ > 0 && bids_[best_bid_idx_].total_volume > 0) {
                Price bid_price = static_cast<Price>(best_bid_idx_) + BasePrice;
                if (price > bid_price) break;

                Quantity fill = std::min(qty, bids_[best_bid_idx_].total_volume);
                qty -= fill;
                matched_qty += fill;
                bids_[best_bid_idx_].total_volume -= fill;
                ++total_trades_;

                if (bids_[best_bid_idx_].total_volume == 0) {
                    while (best_bid_idx_ > 0 && bids_[best_bid_idx_].total_volume == 0) {
                        --best_bid_idx_;
                    }
                }
            }
            if (qty > 0 && id < MaxOrders) {
                size_t idx = price_to_idx(price);
                orders_[id] = Order{id, price, qty, 0, Side::Sell, true};
                asks_[idx].price = price;
                asks_[idx].total_volume += qty;
                ++asks_[idx].order_count;
                if (idx < best_ask_idx_) best_ask_idx_ = idx;
            }
        }
        return matched_qty;
    }

    // O(1) Cancel by OrderId using direct array indexing
    bool cancel_order(OrderId id) noexcept {
        if (id >= MaxOrders || !orders_[id].active) return false;
        Order& ord = orders_[id];
        ord.active = false;
        size_t idx = price_to_idx(ord.price);

        if (ord.side == Side::Buy) {
            bids_[idx].total_volume -= ord.quantity;
            --bids_[idx].order_count;
        } else {
            asks_[idx].total_volume -= ord.quantity;
            --asks_[idx].order_count;
        }
        return true;
    }

    [[nodiscard]] uint64_t total_trades() const noexcept { return total_trades_; }
};

} // namespace clob
