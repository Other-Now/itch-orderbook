#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "itch/transport/moldudp64.hpp"
#include "itch/book/order_book.hpp"
#include "itch/report.hpp"
#include "itch/transport/udp_source.hpp"

// Live market-data client: join a MoldUDP64 multicast group and run every
// datagram through the same decode + book path as file replay -- only the framer
// and source differ.
//
//   usage: itch_live <multicast-group> <port> [--flat]
//
// A functional demonstration that the core is transport-agnostic, not a
// production ingest: blocking recvfrom, single-threaded, no A/B arbitration,
// retransmit recovery, or cross-datagram reassembly.

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <multicast-group> <port> [--flat]\n", argv[0]);
        return 2;
    }
    const std::string group = argv[1];
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));

    std::signal(SIGINT, on_sigint);

    try {
        itch::OrderBookSet book;
        itch::MoldUdp64Framer<itch::OrderBookSet> framer(book);
        itch::UdpMulticastSource source(group, port);

        std::printf("listening on %s:%u (Ctrl-C to stop)\n", group.c_str(), port);
        unsigned char buf[2048];
        std::uint64_t last_report = 0;

        while (!g_stop && !framer.end_of_session()) {
            const std::ptrdiff_t n = source.recv(buf, sizeof(buf));
            if (n <= 0) continue;
            framer.on_packet(buf, static_cast<std::size_t>(n));

            const auto& s = framer.stats();
            if (s.msg.total_messages - last_report >= 100000) {
                last_report = s.msg.total_messages;
                std::printf("msgs=%llu packets=%llu gaps=%llu dups=%llu live_orders=%zu\n",
                            (unsigned long long)s.msg.total_messages,
                            (unsigned long long)s.packets, (unsigned long long)s.gaps,
                            (unsigned long long)s.duplicates, book.live_orders());
            }
        }

        const auto& s = framer.stats();
        std::printf("\nstopped. packets=%llu messages=%llu gaps=%llu duplicates=%llu malformed=%llu\n\n",
                    (unsigned long long)s.packets, (unsigned long long)s.msg.total_messages,
                    (unsigned long long)s.gaps, (unsigned long long)s.duplicates,
                    (unsigned long long)s.malformed);
        itch::print_book_summary(book);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
