#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "itch/protocol/messages.hpp"

namespace itch {

// One order tracked by reference number; enough to undo its effect on the book.
struct Order {
    std::uint16_t stock_locate;
    std::int32_t price;   // ITCH price, 1/10000 of a dollar
    std::uint32_t shares;
    char side;            // 'B' (buy) or 'S' (sell)
};

// ---------------------------------------------------------------------------
// Price-level containers. Both aggregate resting shares per price and share the
// same interface -- add / reduce / best() -> optional<price> -- so OrderBook can
// be templated on the policy and benchmarked head-to-head. `best()` is the price
// sorting first under `Compare` (greater for bids, less for asks).
// ---------------------------------------------------------------------------

// v1: red-black tree. Simple; a node allocation per level, pointer-chasing per
// lookup.
template <class Compare>
class MapLevels {
public:
    void add(std::int32_t price, std::uint32_t shares) { lv_[price] += shares; }

    void reduce(std::int32_t price, std::uint32_t shares) {
        auto it = lv_.find(price);
        if (it == lv_.end()) return;
        if (shares >= it->second)
            lv_.erase(it);
        else
            it->second -= shares;
    }

    std::optional<std::int32_t> best() const {
        if (lv_.empty()) return std::nullopt;
        return lv_.begin()->first;
    }

private:
    std::map<std::int32_t, std::uint64_t, Compare> lv_;
};

// v2: sorted vector, best price at front(). A real book has only tens of active
// levels per side, so contiguous storage wins on cache locality and best() is a
// single load; inserts/erases are small, prefetch-friendly memmoves.
template <class Compare>
class FlatLevels {
public:
    void add(std::int32_t price, std::uint32_t shares) {
        auto it = find(price);
        if (it != lv_.end() && it->first == price)
            it->second += shares;
        else
            lv_.insert(it, {price, static_cast<std::uint64_t>(shares)});
    }

    void reduce(std::int32_t price, std::uint32_t shares) {
        auto it = find(price);
        if (it == lv_.end() || it->first != price) return;
        if (shares >= it->second)
            lv_.erase(it);
        else
            it->second -= shares;
    }

    std::optional<std::int32_t> best() const {
        if (lv_.empty()) return std::nullopt;
        return lv_.front().first;
    }

private:
    using Level = std::pair<std::int32_t, std::uint64_t>;
    std::vector<Level>::iterator find(std::int32_t price) {
        // Sorted best-first under Compare; lower_bound lands on `price` or its slot.
        return std::lower_bound(lv_.begin(), lv_.end(), price,
                                [](const Level& e, std::int32_t v) { return Compare{}(e.first, v); });
    }
    std::vector<Level> lv_;
};

// Per-stock book, parameterized on the level-container policy.
template <template <class> class LevelsTmpl>
class OrderBookT {
public:
    void add(char side, std::int32_t price, std::uint32_t shares) {
        if (side == 'B')
            bids_.add(price, shares);
        else
            asks_.add(price, shares);
    }

    void reduce(char side, std::int32_t price, std::uint32_t shares) {
        if (side == 'B')
            bids_.reduce(price, shares);
        else
            asks_.reduce(price, shares);
    }

    std::optional<std::int32_t> best_bid() const { return bids_.best(); }
    std::optional<std::int32_t> best_ask() const { return asks_.best(); }

private:
    LevelsTmpl<std::greater<std::int32_t>> bids_;  // highest price first
    LevelsTmpl<std::less<std::int32_t>> asks_;     // lowest price first
};

using OrderBook = OrderBookT<MapLevels>;       // v1
using FlatOrderBook = OrderBookT<FlatLevels>;  // v2

// Owns the global order-by-ref map and per-stock books, and applies ITCH
// message semantics to them.
template <class Book = OrderBook>
class OrderBookSetT {
public:
    struct Stats {
        std::uint64_t adds{0};
        std::uint64_t executes{0};
        std::uint64_t cancels{0};
        std::uint64_t deletes{0};
        std::uint64_t replaces{0};
        std::uint64_t unknown_ref{0};  // ops referencing an unseen order
    };

    void add_order(const AddOrder& m) {
        const std::uint64_t ref = m.order_ref();
        const char side = m.side();
        const auto price = static_cast<std::int32_t>(m.price());
        const std::uint32_t shares = m.shares();
        const std::uint16_t loc = m.stock_locate();

        orders_[ref] = Order{loc, price, shares, side};
        books_[loc].add(side, price, shares);
        ++stats_.adds;
    }

    // E / C: cumulative share deduction against the referenced order.
    void execute(std::uint64_t ref, std::uint32_t exec_shares) {
        reduce_order(ref, exec_shares);
        ++stats_.executes;
    }

    // X: cumulative cancel of a partial quantity (same path as execute).
    void cancel(std::uint64_t ref, std::uint32_t canceled_shares) {
        reduce_order(ref, canceled_shares);
        ++stats_.cancels;
    }

    // D: remove the whole remaining order.
    void del(std::uint64_t ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++stats_.unknown_ref; ++stats_.deletes; return; }
        remove_from_book(it->second);
        orders_.erase(it);
        ++stats_.deletes;
    }

    // U: retire the old ref, create a new ref with new price/shares, same side.
    void replace(const OrderReplace& m) {
        auto it = orders_.find(m.original_ref());
        if (it == orders_.end()) { ++stats_.unknown_ref; ++stats_.replaces; return; }
        const Order old = it->second;
        remove_from_book(old);
        orders_.erase(it);

        const auto price = static_cast<std::int32_t>(m.price());
        const std::uint32_t shares = m.shares();
        orders_[m.new_ref()] = Order{old.stock_locate, price, shares, old.side};
        books_[old.stock_locate].add(old.side, price, shares);
        ++stats_.replaces;
    }

    const Stats& stats() const noexcept { return stats_; }
    std::size_t live_orders() const noexcept { return orders_.size(); }
    std::size_t book_count() const noexcept { return books_.size(); }

    const Book* book(std::uint16_t stock_locate) const {
        auto it = books_.find(stock_locate);
        return it == books_.end() ? nullptr : &it->second;
    }

private:
    void remove_from_book(const Order& o) {
        auto it = books_.find(o.stock_locate);
        if (it != books_.end()) it->second.reduce(o.side, o.price, o.shares);
    }

    void reduce_order(std::uint64_t ref, std::uint32_t shares) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++stats_.unknown_ref; return; }
        Order& o = it->second;
        const std::uint32_t hit = shares >= o.shares ? o.shares : shares;
        auto bit = books_.find(o.stock_locate);
        if (bit != books_.end()) bit->second.reduce(o.side, o.price, hit);
        if (shares >= o.shares)
            orders_.erase(it);  // fully consumed -> order is dead
        else
            o.shares -= shares;
    }

    std::unordered_map<std::uint64_t, Order> orders_;
    std::unordered_map<std::uint16_t, Book> books_;
    Stats stats_;
};

using OrderBookSet = OrderBookSetT<OrderBook>;          // v1
using FlatOrderBookSet = OrderBookSetT<FlatOrderBook>;  // v2

}  // namespace itch
