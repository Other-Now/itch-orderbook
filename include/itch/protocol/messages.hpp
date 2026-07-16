#pragma once

#include <cstdint>

#include "itch/core/endian.hpp"

// Typed, zero-copy accessors over ITCH 5.0 messages. Each accessor reads its
// field from a fixed offset with a big-endian load, avoiding the alignment and
// endianness hazards of reinterpreting a packed struct. Offsets are relative to
// the message-type byte at position 0.

namespace itch {

enum class MsgType : char {
    SystemEvent = 'S',
    StockDirectory = 'R',
    StockTradingAction = 'H',
    RegSHO = 'Y',
    MarketParticipant = 'L',
    MwcbDeclineLevel = 'V',
    MwcbStatus = 'W',
    IpoQuotingPeriod = 'K',
    LuldAuctionCollar = 'J',
    OperationalHalt = 'h',
    AddOrder = 'A',
    AddOrderMpid = 'F',
    OrderExecuted = 'E',
    OrderExecutedWithPrice = 'C',
    OrderCancel = 'X',
    OrderDelete = 'D',
    OrderReplace = 'U',
    Trade = 'P',
    CrossTrade = 'Q',
    BrokenTrade = 'B',
    Noii = 'I',
    Rpii = 'N',
};

// Canonical on-wire body length (including the type byte) for each message.
// Used for validation against the framing length prefix.
constexpr int message_length(char type) noexcept {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        default: return -1;
    }
}

// --- Add Order (A) : type,locate,tracking,ts(6),ref(8),side,shares,stock(8),price
struct AddOrder {
    const unsigned char* p;
    std::uint16_t stock_locate() const noexcept { return load_be16(p + 1); }
    std::uint64_t timestamp() const noexcept { return load_be48(p + 5); }
    std::uint64_t order_ref() const noexcept { return load_be64(p + 11); }
    char side() const noexcept { return static_cast<char>(p[19]); }  // 'B'/'S'
    std::uint32_t shares() const noexcept { return load_be32(p + 20); }
    const char* stock() const noexcept {  // 8 chars, space-padded
        return reinterpret_cast<const char*>(p + 24);
    }
    std::uint32_t price() const noexcept { return load_be32(p + 32); }  // 1/1e4
};

// --- Order Executed (E) : type,locate,tracking,ts(6),ref(8),exec_shares(4),match(8)
struct OrderExecuted {
    const unsigned char* p;
    std::uint64_t order_ref() const noexcept { return load_be64(p + 11); }
    std::uint32_t executed_shares() const noexcept { return load_be32(p + 19); }
    std::uint64_t match_number() const noexcept { return load_be64(p + 23); }
};

// --- Order Executed w/ Price (C) : E + printable(1) + exec_price(4)
struct OrderExecutedWithPrice {
    const unsigned char* p;
    std::uint64_t order_ref() const noexcept { return load_be64(p + 11); }
    std::uint32_t executed_shares() const noexcept { return load_be32(p + 19); }
    std::uint64_t match_number() const noexcept { return load_be64(p + 23); }
    char printable() const noexcept { return static_cast<char>(p[31]); }
    std::uint32_t exec_price() const noexcept { return load_be32(p + 32); }
};

// --- Order Cancel (X) : type,locate,tracking,ts(6),ref(8),canceled_shares(4)
struct OrderCancel {
    const unsigned char* p;
    std::uint64_t order_ref() const noexcept { return load_be64(p + 11); }
    std::uint32_t canceled_shares() const noexcept { return load_be32(p + 19); }
};

// --- Order Delete (D) : type,locate,tracking,ts(6),ref(8)
struct OrderDelete {
    const unsigned char* p;
    std::uint64_t order_ref() const noexcept { return load_be64(p + 11); }
};

// --- Order Replace (U) : type,locate,tracking,ts(6),orig_ref(8),new_ref(8),shares(4),price(4)
struct OrderReplace {
    const unsigned char* p;
    std::uint64_t original_ref() const noexcept { return load_be64(p + 11); }
    std::uint64_t new_ref() const noexcept { return load_be64(p + 19); }
    std::uint32_t shares() const noexcept { return load_be32(p + 27); }
    std::uint32_t price() const noexcept { return load_be32(p + 31); }
};

// --- System Event (S) : type,locate,tracking,ts(6),event_code(1)
struct SystemEvent {
    const unsigned char* p;
    char event_code() const noexcept { return static_cast<char>(p[11]); }
};

}  // namespace itch
