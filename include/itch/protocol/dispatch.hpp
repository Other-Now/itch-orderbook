#pragma once

#include <array>
#include <cstdint>

#include "itch/protocol/messages.hpp"

// Source-agnostic message dispatch: decode one framed ITCH message and apply it
// to the book. The seam that keeps the decoders and order book unaware of the
// transport -- file replay and the live MoldUDP64 framer both call it identically.

namespace itch {

struct MessageStats {
    std::uint64_t total_messages{0};
    std::array<std::uint64_t, 256> by_type{};  // indexed by message-type byte
    std::uint64_t length_mismatches{0};         // framed len != canonical len
};

// `msg` points at the type byte; `len` is the framed body length (type included).
template <class BookSet>
inline void dispatch_message(const unsigned char* msg, std::uint16_t len, BookSet& book,
                             MessageStats& stats) {
    const char type = static_cast<char>(msg[0]);

    const int canonical = message_length(type);
    if (canonical > 0 && canonical != static_cast<int>(len)) ++stats.length_mismatches;

    switch (type) {
        case 'A':
        case 'F':  // Add w/ MPID: identical prefix, extra attribution ignored
            book.add_order(AddOrder{msg});
            break;
        case 'E': {
            OrderExecuted m{msg};
            book.execute(m.order_ref(), m.executed_shares());
            break;
        }
        case 'C': {
            OrderExecutedWithPrice m{msg};
            book.execute(m.order_ref(), m.executed_shares());
            break;
        }
        case 'X': {
            OrderCancel m{msg};
            book.cancel(m.order_ref(), m.canceled_shares());
            break;
        }
        case 'D':
            book.del(OrderDelete{msg}.order_ref());
            break;
        case 'U':
            book.replace(OrderReplace{msg});
            break;
        default:
            break;  // system events, trades, NOII, etc. -- counted only
    }

    ++stats.by_type[static_cast<unsigned char>(type)];
    ++stats.total_messages;
}

}  // namespace itch
