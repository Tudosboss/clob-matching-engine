#pragma once

#include "OrderBook.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

namespace clob {

// Simple order book built on std::map, std::list and std::unordered_map, with the
// same matching rules as OrderBook. Used as the benchmark baseline and in the tests.
template <typename Listener = NullListener>
class MapOrderBook {
    struct Node { OrderId id; Quantity qty; };
    struct Ref { Side side; Price price; typename std::list<Node>::iterator it; };

    std::map<Price, std::list<Node>, std::greater<Price>> bids_;
    std::map<Price, std::list<Node>> asks_;
    std::unordered_map<OrderId, Ref> index_;
    Listener listener_{};

    template <typename Levels>
    uint64_t take(Levels& levels, OrderId taker, Quantity& qty, Price limit, bool is_buy) {
        uint64_t matched = 0;
        while (qty > 0 && !levels.empty() &&
               (is_buy ? levels.begin()->first <= limit : levels.begin()->first >= limit)) {
            const Price px = levels.begin()->first;
            auto& q = levels.begin()->second;
            while (qty > 0 && !q.empty()) {
                auto& head = q.front();
                Quantity fill = std::min(qty, head.qty);
                qty -= fill;
                head.qty -= fill;
                matched += fill;
                listener_.on_trade(taker, head.id, px, fill);
                if (head.qty == 0) {
                    index_.erase(head.id);
                    q.pop_front();
                }
            }
            if (q.empty()) levels.erase(levels.begin());
        }
        return matched;
    }

    template <typename Levels>
    void rest(Levels& levels, OrderId id, Side side, Price price, Quantity qty) {
        auto& q = levels[price];
        q.push_back({id, qty});
        index_[id] = {side, price, std::prev(q.end())};
    }

public:
    void clear() { bids_.clear(); asks_.clear(); index_.clear(); }

    uint64_t add_order(OrderId id, Side side, Price price, Quantity qty) {
        uint64_t matched;
        if (side == Side::Buy) {
            matched = take(asks_, id, qty, price, true);
            if (qty > 0) rest(bids_, id, side, price, qty);
        } else {
            matched = take(bids_, id, qty, price, false);
            if (qty > 0) rest(asks_, id, side, price, qty);
        }
        return matched;
    }

    uint64_t add_market_order(OrderId id, Side side, Quantity qty) {
        if (side == Side::Buy) return take(asks_, id, qty, std::numeric_limits<Price>::max(), true);
        return take(bids_, id, qty, std::numeric_limits<Price>::min(), false);
    }

    bool cancel_order(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        const Ref ref = it->second;
        if (ref.side == Side::Buy) {
            auto lvl = bids_.find(ref.price);
            lvl->second.erase(ref.it);
            if (lvl->second.empty()) bids_.erase(lvl);
        } else {
            auto lvl = asks_.find(ref.price);
            lvl->second.erase(ref.it);
            if (lvl->second.empty()) asks_.erase(lvl);
        }
        index_.erase(it);
        return true;
    }

    std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }

    std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    uint64_t volume_at(Side side, Price price) const {
        uint64_t v = 0;
        if (side == Side::Buy) { auto it = bids_.find(price); if (it != bids_.end()) for (auto& n : it->second) v += n.qty; }
        else                   { auto it = asks_.find(price); if (it != asks_.end()) for (auto& n : it->second) v += n.qty; }
        return v;
    }

    size_t live_orders() const { return index_.size(); }
    Listener& listener() { return listener_; }
};

} // namespace clob
