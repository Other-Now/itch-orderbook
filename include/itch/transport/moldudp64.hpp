#pragma once

#include <cstddef>
#include <cstdint>

#include "itch/protocol/dispatch.hpp"
#include "itch/core/endian.hpp"

// Live transport: MoldUDP64, the framing NASDAQ uses for real-time UDP multicast.
// It adds, over the file, an outer packet header (Session[10] + SequenceNumber[8]
// + MessageCount[2]), per-message sequencing for gap detection and duplicate
// skipping, and packet-oriented delivery. Messages inside a packet use the same
// 2-byte length framing, so each goes through the identical dispatch_message().
// Feed datagrams via on_packet(); the socket layer is separate (udp_source.hpp).

namespace itch {

template <class BookSet>
class MoldUdp64Framer {
public:
    explicit MoldUdp64Framer(BookSet& book) : book_(book) {}

    struct Stats {
        MessageStats msg;              // per-message (shared shape with the file)
        std::uint64_t packets{0};
        std::uint64_t heartbeats{0};   // MessageCount == 0
        std::uint64_t gaps{0};         // count of missed sequence numbers
        std::uint64_t duplicates{0};   // messages already applied (retransmit/B-line)
        std::uint64_t malformed{0};    // short header or overrunning length
    };

    static constexpr std::size_t kHeaderLen = 20;   // 10 + 8 + 2
    static constexpr std::uint16_t kEndOfSession = 0xFFFF;

    // Process one UDP datagram (a MoldUDP64 packet).
    void on_packet(const unsigned char* pkt, std::size_t len) {
        ++stats_.packets;
        if (len < kHeaderLen) { ++stats_.malformed; return; }

        const std::uint64_t seq = load_be64(pkt + 10);   // seq # of the first message
        const std::uint16_t count = load_be16(pkt + 18);

        if (count == kEndOfSession) { end_of_session_ = true; return; }
        if (count == 0) { ++stats_.heartbeats; return; }  // heartbeat carries no messages

        if (!started_) { expected_ = seq; started_ = true; }
        if (seq > expected_) stats_.gaps += (seq - expected_);  // datagram(s) dropped

        const unsigned char* p = pkt + kHeaderLen;
        const unsigned char* const end = pkt + len;
        std::uint64_t s = seq;

        for (std::uint16_t i = 0; i < count; ++i) {
            if (p + 2 > end) { ++stats_.malformed; break; }
            const std::uint16_t mlen = load_be16(p);
            p += 2;
            if (p + mlen > end) { ++stats_.malformed; break; }

            if (s < expected_) {
                ++stats_.duplicates;                 // already applied; skip
            } else {
                dispatch_message(p, mlen, book_, stats_.msg);
                expected_ = s + 1;                   // advance past what we applied
            }
            p += mlen;
            ++s;
        }
    }

    const Stats& stats() const noexcept { return stats_; }
    std::uint64_t next_expected_sequence() const noexcept { return expected_; }
    bool end_of_session() const noexcept { return end_of_session_; }

private:
    BookSet& book_;
    Stats stats_;
    std::uint64_t expected_{0};  // next sequence number we still need to apply
    bool started_{false};
    bool end_of_session_{false};
};

}  // namespace itch
