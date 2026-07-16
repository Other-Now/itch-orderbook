#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "itch/book/mbo_book.hpp"
#include "itch/transport/mmap_file.hpp"
#include "itch/book/order_book.hpp"
#include "itch/transport/parser.hpp"
#include "itch/report.hpp"

namespace {

template <class Book>
int run(const char* path) {
    itch::MmapFile file(path);
    Book book;

    const auto t0 = std::chrono::steady_clock::now();
    const itch::ParseStats st = itch::parse(file.data(), file.size(), book);
    const auto t1 = std::chrono::steady_clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double msgs = static_cast<double>(st.msg.total_messages);

    std::printf("file            : %s\n", path);
    std::printf("size            : %.2f MiB\n", file.size() / (1024.0 * 1024.0));
    std::printf("messages        : %llu\n", (unsigned long long)st.msg.total_messages);
    std::printf("bytes consumed  : %llu / %zu\n",
                (unsigned long long)st.bytes_consumed, file.size());
    std::printf("length mismatch : %llu\n", (unsigned long long)st.msg.length_mismatches);
    std::printf("elapsed         : %.3f s\n", secs);
    if (secs > 0.0) {
        std::printf("throughput      : %.2f M msgs/s\n", msgs / secs / 1e6);
        std::printf("per message     : %.1f ns\n", secs / msgs * 1e9);
    }

    std::printf("\n");
    itch::print_book_summary(book);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    enum { kMap, kFlat, kMbo } model = kMap;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--flat")
            model = kFlat;
        else if (arg == "--mbo")
            model = kMbo;
        else
            path = argv[i];
    }
    if (!path) {
        std::fprintf(stderr,
                     "usage: %s [--flat | --mbo] <NASDAQ_ITCH50 file>\n"
                     "  (default) market-by-price, std::map levels\n"
                     "  --flat    market-by-price, flat sorted-vector levels\n"
                     "  --mbo     market-by-order, price-time FIFO + pool\n",
                     argv[0]);
        return 2;
    }

    try {
        switch (model) {
            case kFlat: return run<itch::FlatOrderBookSet>(path);
            case kMbo:  return run<itch::MboOrderBookSet>(path);
            default:    return run<itch::OrderBookSet>(path);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
