#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "itch/core/endian.hpp"
#include "itch/protocol/messages.hpp"
#include "itch/book/mbo_book.hpp"
#include "itch/transport/moldudp64.hpp"
#include "itch/book/order_book.hpp"
#include "itch/transport/parser.hpp"

using namespace itch;

namespace {

// Helpers to build big-endian fields into a byte buffer.
void put_be16(std::vector<unsigned char>& v, std::uint16_t x) {
    v.push_back(x >> 8); v.push_back(x & 0xff);
}
void put_be32(std::vector<unsigned char>& v, std::uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
void put_be48(std::vector<unsigned char>& v, std::uint64_t x) {
    for (int i = 5; i >= 0; --i) v.push_back((x >> (i * 8)) & 0xff);
}
void put_be64(std::vector<unsigned char>& v, std::uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back((x >> (i * 8)) & 0xff);
}

// One Add Order (A) message body (36 bytes, incl. type).
std::vector<unsigned char> make_add(std::uint16_t loc, std::uint64_t ref, char side,
                                    std::uint32_t shares, std::int32_t price) {
    std::vector<unsigned char> v;
    v.push_back('A');
    put_be16(v, loc);         // stock locate
    put_be16(v, 0);           // tracking
    put_be48(v, 1234567);     // timestamp
    put_be64(v, ref);
    v.push_back(static_cast<unsigned char>(side));
    put_be32(v, shares);
    for (int i = 0; i < 8; ++i) v.push_back(' ');  // stock symbol (padding)
    put_be32(v, static_cast<std::uint32_t>(price));
    return v;
}

// Frame a message with its 2-byte big-endian length prefix.
void frame(std::vector<unsigned char>& out, const std::vector<unsigned char>& msg) {
    put_be16(out, static_cast<std::uint16_t>(msg.size()));
    out.insert(out.end(), msg.begin(), msg.end());
}

}  // namespace

TEST_CASE("big-endian loads") {
    const unsigned char b16[] = {0x12, 0x34};
    CHECK(load_be16(b16) == 0x1234);
    const unsigned char b32[] = {0x01, 0x02, 0x03, 0x04};
    CHECK(load_be32(b32) == 0x01020304u);
    const unsigned char b48[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    CHECK(load_be48(b48) == 256u);
    const unsigned char b64[] = {0, 0, 0, 0, 0, 0, 0x01, 0x00};
    CHECK(load_be64(b64) == 256u);
}

TEST_CASE("add order decode") {
    auto m = make_add(7, 42, 'B', 100, 1015000);  // $101.50
    AddOrder a{m.data()};
    CHECK(a.stock_locate() == 7);
    CHECK(a.order_ref() == 42);
    CHECK(a.side() == 'B');
    CHECK(a.shares() == 100);
    CHECK(a.price() == 1015000u);
}

TEST_CASE("book: add builds top-of-book") {
    OrderBookSet set;
    set.add_order(AddOrder{make_add(1, 1, 'B', 100, 1000000).data()});
    set.add_order(AddOrder{make_add(1, 2, 'B', 200, 1005000).data()});  // better bid
    set.add_order(AddOrder{make_add(1, 3, 'S', 50, 1010000).data()});
    const OrderBook* b = set.book(1);
    REQUIRE(b != nullptr);
    CHECK(b->best_bid().value() == 1005000);
    CHECK(b->best_ask().value() == 1010000);
}

TEST_CASE("book: execute to zero removes order and level") {
    OrderBookSet set;
    set.add_order(AddOrder{make_add(1, 10, 'B', 100, 1000000).data()});
    set.execute(10, 40);   // partial
    CHECK(set.book(1)->best_bid().value() == 1000000);
    set.execute(10, 60);   // remaining -> order dies, level empties
    CHECK_FALSE(set.book(1)->best_bid().has_value());
    CHECK(set.live_orders() == 0);
}

TEST_CASE("book: cancel reduces, delete removes") {
    OrderBookSet set;
    set.add_order(AddOrder{make_add(1, 11, 'S', 300, 2000000).data()});
    set.cancel(11, 100);
    CHECK(set.book(1)->best_ask().value() == 2000000);
    set.del(11);
    CHECK_FALSE(set.book(1)->best_ask().has_value());
    CHECK(set.live_orders() == 0);
}

TEST_CASE("book: replace retires old ref, installs new") {
    OrderBookSet set;
    set.add_order(AddOrder{make_add(1, 20, 'B', 100, 1000000).data()});

    // Build a Replace (U): orig=20, new=21, shares=150, price=1002000.
    std::vector<unsigned char> u;
    u.push_back('U');
    put_be16(u, 1); put_be16(u, 0); put_be48(u, 1);
    put_be64(u, 20); put_be64(u, 21);
    put_be32(u, 150); put_be32(u, 1002000);
    set.replace(OrderReplace{u.data()});

    CHECK(set.book(1)->best_bid().value() == 1002000);
    CHECK(set.live_orders() == 1);
    // Old ref is dead: deleting it is a no-op counted as unknown_ref.
    set.del(20);
    CHECK(set.stats().unknown_ref == 1);
}

TEST_CASE("mbo: same BBO as the MBP book on identical input") {
    OrderBookSet mbp;
    MboOrderBookSet mbo;
    for (auto& m : {make_add(1, 1, 'B', 100, 1000000), make_add(1, 2, 'B', 200, 1005000),
                    make_add(1, 3, 'S', 50, 1010000), make_add(1, 4, 'S', 75, 1012000)}) {
        mbp.add_order(AddOrder{m.data()});
        mbo.add_order(AddOrder{m.data()});
    }
    CHECK(mbo.book(1)->best_bid() == mbp.book(1)->best_bid());
    CHECK(mbo.book(1)->best_ask() == mbp.book(1)->best_ask());
}

TEST_CASE("mbo: queue_position tracks price-time priority") {
    MboOrderBookSet s;
    // Three orders at the same price -> FIFO by arrival.
    s.add_order(AddOrder{make_add(1, 100, 'B', 10, 1000000).data()});
    s.add_order(AddOrder{make_add(1, 101, 'B', 20, 1000000).data()});
    s.add_order(AddOrder{make_add(1, 102, 'B', 30, 1000000).data()});
    CHECK(s.queue_position(100) == 0);    // front
    CHECK(s.queue_position(101) == 10);   // 10 ahead
    CHECK(s.queue_position(102) == 30);   // 10 + 20 ahead

    // Delete the front order: everyone behind advances.
    s.del(100);
    CHECK(s.queue_position(101) == 0);
    CHECK(s.queue_position(102) == 20);

    // Partial execute the new front: shares ahead of #102 shrink accordingly.
    s.execute(101, 5);                    // 101: 20 -> 15
    CHECK(s.queue_position(102) == 15);

    // Fully execute the front: #102 becomes the front.
    s.execute(101, 15);
    CHECK(s.queue_position(102) == 0);
    CHECK(s.queue_position(999) == MboOrderBookSet::kNoOrder);  // unknown ref
}

TEST_CASE("mbo: replace loses time priority (joins the back)") {
    MboOrderBookSet s;
    s.add_order(AddOrder{make_add(1, 1, 'B', 10, 1000000).data()});
    s.add_order(AddOrder{make_add(1, 2, 'B', 20, 1000000).data()});

    // Replace ref 1 in place (same price): it should move behind ref 2.
    std::vector<unsigned char> u;
    u.push_back('U');
    put_be16(u, 1); put_be16(u, 0); put_be48(u, 1);
    put_be64(u, 1); put_be64(u, 3);        // orig=1, new=3
    put_be32(u, 10); put_be32(u, 1000000);  // same price/shares
    s.replace(OrderReplace{u.data()});

    CHECK(s.queue_position(2) == 0);       // ref 2 now at the front
    CHECK(s.queue_position(3) == 20);      // replaced order behind ref 2
    CHECK(s.live_orders() == 2);
}

TEST_CASE("parser: framed stream dispatches to book") {
    std::vector<unsigned char> stream;
    frame(stream, make_add(3, 100, 'B', 500, 999900));
    frame(stream, make_add(3, 101, 'S', 500, 1000100));

    OrderBookSet set;
    ParseStats st = parse(stream.data(), stream.size(), set);
    CHECK(st.msg.total_messages == 2);
    CHECK(st.msg.by_type[static_cast<unsigned char>('A')] == 2);
    CHECK(st.msg.length_mismatches == 0);
    CHECK(set.book(3)->best_bid().value() == 999900);
    CHECK(set.book(3)->best_ask().value() == 1000100);
}

TEST_CASE("moldudp64: same book as file replay, over packets") {
    // Two Add messages, delivered as one MoldUDP64 packet (seq starts at 1).
    auto a1 = make_add(3, 100, 'B', 500, 999900);
    auto a2 = make_add(3, 101, 'S', 500, 1000100);

    std::vector<unsigned char> pkt;
    for (int i = 0; i < 10; ++i) pkt.push_back('X');   // session (10 bytes)
    put_be64(pkt, 1);                                   // sequence number
    put_be16(pkt, 2);                                   // message count
    frame(pkt, a1);                                     // [len][msg] blocks
    frame(pkt, a2);

    OrderBookSet set;
    MoldUdp64Framer<OrderBookSet> framer(set);
    framer.on_packet(pkt.data(), pkt.size());

    CHECK(framer.stats().msg.total_messages == 2);
    CHECK(framer.stats().gaps == 0);
    CHECK(framer.next_expected_sequence() == 3);
    CHECK(set.book(3)->best_bid().value() == 999900);
    CHECK(set.book(3)->best_ask().value() == 1000100);
}

TEST_CASE("moldudp64: detects a gap and skips duplicates") {
    OrderBookSet set;
    MoldUdp64Framer<OrderBookSet> framer(set);

    auto packet_of = [](std::uint64_t seq, const std::vector<unsigned char>& msg) {
        std::vector<unsigned char> pkt;
        for (int i = 0; i < 10; ++i) pkt.push_back(0);
        put_be64(pkt, seq);
        put_be16(pkt, 1);
        frame(pkt, msg);
        return pkt;
    };

    auto p1 = packet_of(1, make_add(1, 1, 'B', 100, 1000000));
    auto p3 = packet_of(3, make_add(1, 3, 'B', 100, 1000100));  // seq 2 missing
    framer.on_packet(p1.data(), p1.size());
    framer.on_packet(p3.data(), p3.size());
    CHECK(framer.stats().gaps == 1);                 // noticed the missing sequence
    CHECK(framer.stats().msg.total_messages == 2);

    // Re-deliver seq 1 (a retransmit / B-line copy): applied 0 times, counted dup.
    framer.on_packet(p1.data(), p1.size());
    CHECK(framer.stats().duplicates == 1);
    CHECK(framer.stats().msg.total_messages == 2);   // not re-applied
    CHECK(set.live_orders() == 2);
}
