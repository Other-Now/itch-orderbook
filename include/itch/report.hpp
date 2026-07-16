#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Shared book-summary printer, so file replay and the live client report the
// book identically. Templated on the book set to serve all three book models.

namespace itch {

inline std::string price_str(std::int32_t p) {  // ITCH price is 1/10000 of a dollar
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d.%04d", p / 10000, p % 10000);
    return buf;
}

template <class BookSet>
void print_book_summary(const BookSet& book) {
    const auto& s = book.stats();
    std::printf("book operations\n");
    std::printf("  adds          : %llu\n", (unsigned long long)s.adds);
    std::printf("  executes      : %llu\n", (unsigned long long)s.executes);
    std::printf("  cancels       : %llu\n", (unsigned long long)s.cancels);
    std::printf("  deletes       : %llu\n", (unsigned long long)s.deletes);
    std::printf("  replaces      : %llu\n", (unsigned long long)s.replaces);
    std::printf("  unknown ref   : %llu\n", (unsigned long long)s.unknown_ref);
    std::printf("  live orders   : %zu\n", book.live_orders());
    std::printf("  stocks seen   : %zu\n", book.book_count());

    // Sample top-of-book for the first stock with both sides.
    for (std::uint32_t loc = 0; loc < 65535; ++loc) {
        const auto* b = book.book(static_cast<std::uint16_t>(loc));
        if (!b) continue;
        auto bid = b->best_bid();
        auto ask = b->best_ask();
        if (bid && ask) {
            std::printf("sample top-of-book (stock_locate=%u): %s x %s\n", loc,
                        price_str(*bid).c_str(), price_str(*ask).c_str());
            break;
        }
    }
}

}  // namespace itch
