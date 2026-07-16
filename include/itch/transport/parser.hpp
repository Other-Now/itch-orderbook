#pragma once

#include <cstddef>
#include <cstdint>

#include "itch/protocol/dispatch.hpp"
#include "itch/core/endian.hpp"

namespace itch {

// File transport: NASDAQ "BinaryFILE" framing -- one contiguous stream, each
// message preceded by a 2-byte big-endian length. Shares dispatch_message() with
// the live MoldUDP64 framer (moldudp64.hpp).

struct ParseStats {
    MessageStats msg;
    std::uint64_t bytes_consumed{0};
};

// Walk a mapped BinaryFILE buffer to completion. Stops on a zero-length or
// truncated frame, so a partially downloaded file still replays cleanly.
template <class BookSet>
ParseStats parse(const unsigned char* data, std::size_t size, BookSet& book) {
    ParseStats st;
    std::size_t off = 0;

    while (off + 2 <= size) {
        const std::uint16_t len = load_be16(data + off);
        if (len == 0) break;              // clean EOF
        if (off + 2 + len > size) break;  // truncated tail
        dispatch_message(data + off + 2, len, book, st.msg);
        off += 2 + len;
    }

    st.bytes_consumed = off;
    return st;
}

}  // namespace itch
