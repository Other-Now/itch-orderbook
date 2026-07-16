#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>

#include "itch/protocol/messages.hpp"
#include "itch/core/object_pool.hpp"

namespace itch {

// ---------------------------------------------------------------------------
// Market-by-order (MBO) book: full price-time priority. Every resting order is a
// node in an intrusive doubly-linked FIFO at its price level (head = oldest). A
// global ref->node index gives O(1) location and the back-pointers give O(1)
// unlink. More than the MBP book needs for BBO, but it answers time-priority
// questions -- see queue_position().
// ---------------------------------------------------------------------------

struct MboLevel;

struct MboOrder {
    std::uint64_t ref{0};
    std::uint32_t shares{0};
    std::int32_t price{0};
    std::uint16_t stock_locate{0};
    char side{0};        // 'B' / 'S'
    MboOrder* prev{nullptr};  // older order at this level (toward head)
    MboOrder* next{nullptr};  // newer order at this level (toward tail)
    MboLevel* level{nullptr};
};

struct MboLevel {
    std::int32_t price{0};
    std::uint64_t total_shares{0};
    std::uint32_t order_count{0};
    MboOrder* head{nullptr};  // front of FIFO
    MboOrder* tail{nullptr};  // back of FIFO
};

// One symbol's book: bid and ask level maps (best price at begin()), each level
// owning a FIFO of order nodes.
class MboBook {
public:
    // Append to the tail of its price level (newest = lowest priority).
    void add(MboOrder* o) {
        MboLevel& lv = (o->side == 'B') ? bids_[o->price] : asks_[o->price];
        if (lv.order_count == 0) lv.price = o->price;
        o->prev = lv.tail;
        o->next = nullptr;
        o->level = &lv;
        if (lv.tail)
            lv.tail->next = o;
        else
            lv.head = o;
        lv.tail = o;
        lv.total_shares += o->shares;
        ++lv.order_count;
    }

    // Reduce shares (execute/cancel). Returns true if fully consumed and
    // unlinked (caller then frees it).
    bool reduce(MboOrder* o, std::uint32_t by) {
        const std::uint32_t hit = by >= o->shares ? o->shares : by;
        o->level->total_shares -= hit;
        o->shares -= hit;
        if (o->shares == 0) {
            unlink(o);
            return true;
        }
        return false;
    }

    // Remove the whole remaining order (delete).
    void remove(MboOrder* o) {
        o->level->total_shares -= o->shares;
        o->shares = 0;
        unlink(o);
    }

    std::optional<std::int32_t> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }
    std::optional<std::int32_t> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    const MboLevel* best_bid_level() const {
        return bids_.empty() ? nullptr : &bids_.begin()->second;
    }
    const MboLevel* best_ask_level() const {
        return asks_.empty() ? nullptr : &asks_.begin()->second;
    }

private:
    void unlink(MboOrder* o) {
        MboLevel* lv = o->level;
        if (o->prev)
            o->prev->next = o->next;
        else
            lv->head = o->next;
        if (o->next)
            o->next->prev = o->prev;
        else
            lv->tail = o->prev;
        --lv->order_count;
        if (lv->order_count == 0) {
            if (o->side == 'B')
                bids_.erase(o->price);
            else
                asks_.erase(o->price);
        }
    }

    std::map<std::int32_t, MboLevel, std::greater<>> bids_;  // highest first
    std::map<std::int32_t, MboLevel, std::less<>> asks_;     // lowest first
};

// Owns the order pool, the global ref index, and the per-symbol books.
class MboOrderBookSet {
public:
    struct Stats {
        std::uint64_t adds{0};
        std::uint64_t executes{0};
        std::uint64_t cancels{0};
        std::uint64_t deletes{0};
        std::uint64_t replaces{0};
        std::uint64_t unknown_ref{0};
    };

    // Sentinel returned by queue_position() for an unknown ref.
    static constexpr std::uint64_t kNoOrder = std::numeric_limits<std::uint64_t>::max();

    void add_order(const AddOrder& m) {
        MboOrder* o = pool_.allocate();
        o->ref = m.order_ref();
        o->shares = m.shares();
        o->price = static_cast<std::int32_t>(m.price());
        o->stock_locate = m.stock_locate();
        o->side = m.side();
        books_[o->stock_locate].add(o);
        orders_[o->ref] = o;
        ++stats_.adds;
    }

    void execute(std::uint64_t ref, std::uint32_t exec_shares) {
        reduce_order(ref, exec_shares);
        ++stats_.executes;
    }

    void cancel(std::uint64_t ref, std::uint32_t canceled_shares) {
        reduce_order(ref, canceled_shares);
        ++stats_.cancels;
    }

    void del(std::uint64_t ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++stats_.unknown_ref; ++stats_.deletes; return; }
        MboOrder* o = it->second;
        books_[o->stock_locate].remove(o);
        pool_.deallocate(o);
        orders_.erase(it);
        ++stats_.deletes;
    }

    void replace(const OrderReplace& m) {
        auto it = orders_.find(m.original_ref());
        if (it == orders_.end()) { ++stats_.unknown_ref; ++stats_.replaces; return; }
        MboOrder* old = it->second;
        const std::uint16_t loc = old->stock_locate;
        const char side = old->side;
        books_[loc].remove(old);
        pool_.deallocate(old);
        orders_.erase(it);

        // New ref joins the back of its level: a replace loses time priority.
        MboOrder* o = pool_.allocate();
        o->ref = m.new_ref();
        o->shares = m.shares();
        o->price = static_cast<std::int32_t>(m.price());
        o->stock_locate = loc;
        o->side = side;
        books_[loc].add(o);
        orders_[o->ref] = o;
        ++stats_.replaces;
    }

    // Shares resting ahead of `ref` in its level's FIFO (its own excluded). The
    // time-priority query the MBO structure exists for; MBP cannot answer it.
    std::uint64_t queue_position(std::uint64_t ref) const {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return kNoOrder;
        const MboOrder* target = it->second;
        std::uint64_t ahead = 0;
        for (const MboOrder* p = target->level->head; p && p != target; p = p->next)
            ahead += p->shares;
        return ahead;
    }

    const Stats& stats() const noexcept { return stats_; }
    std::size_t live_orders() const noexcept { return orders_.size(); }
    std::size_t book_count() const noexcept { return books_.size(); }

    const MboBook* book(std::uint16_t stock_locate) const {
        auto it = books_.find(stock_locate);
        return it == books_.end() ? nullptr : &it->second;
    }

private:
    void reduce_order(std::uint64_t ref, std::uint32_t shares) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++stats_.unknown_ref; return; }
        MboOrder* o = it->second;
        if (books_[o->stock_locate].reduce(o, shares)) {
            pool_.deallocate(o);
            orders_.erase(it);
        }
    }

    ObjectPool<MboOrder> pool_;
    std::unordered_map<std::uint64_t, MboOrder*> orders_;
    std::unordered_map<std::uint16_t, MboBook> books_;
    Stats stats_;
};

}  // namespace itch
