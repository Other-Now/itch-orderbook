// Generates a small, valid BinaryFILE-framed ITCH 5.0 stream for testing and
// benchmarking without the multi-GB real feed. Emits a System Event plus a mix
// of Add / Execute / Cancel / Delete / Replace over one symbol, with a
// non-crossing penny-tick book. Only references live orders, so a correct parser
// reports zero unknown references.
//
//   usage: itch_gen [num_adds] [out_file]   (default: 500000 -> data/synthetic.NASDAQ_ITCH50)

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

void be16(std::vector<unsigned char>& v, std::uint16_t x) { v.push_back(x >> 8); v.push_back(x); }
void be32(std::vector<unsigned char>& v, std::uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
void be48(std::vector<unsigned char>& v, std::uint64_t x) {
    for (int i = 5; i >= 0; --i) v.push_back((x >> (i * 8)) & 0xff);
}
void be64(std::vector<unsigned char>& v, std::uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back((x >> (i * 8)) & 0xff);
}
void frame(std::vector<unsigned char>& out, const std::vector<unsigned char>& m) {
    be16(out, static_cast<std::uint16_t>(m.size()));
    out.insert(out.end(), m.begin(), m.end());
}

}  // namespace

int main(int argc, char** argv) {
    const long num_adds = argc > 1 ? std::stol(argv[1]) : 500000;
    const std::string out_path = argc > 2 ? argv[2] : "data/synthetic.NASDAQ_ITCH50";

    std::vector<unsigned char> out;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> depth(1, 20);       // ticks from mid
    std::uniform_int_distribution<int> shares_d(1, 500);
    std::uniform_int_distribution<int> coin(0, 99);
    constexpr std::int32_t kMid = 1000000;  // $100.0000
    constexpr std::uint16_t kLocate = 1;
    std::uint64_t ts = 0;

    auto stamp = [&](std::vector<unsigned char>& m) { be16(m, kLocate); be16(m, 0); be48(m, ts++); };

    // System Event 'O' (start of messages).
    { std::vector<unsigned char> m; m.push_back('S'); stamp(m); m.push_back('O'); frame(out, m); }

    // Mirror the engine's live-order state so every message hits a resting order.
    struct Live { std::uint64_t ref; char side; std::uint32_t shares; };
    std::vector<Live> live;
    std::uint64_t next_ref = 1;

    auto price_for = [&](char side) {  // buys below mid, sells above -> never crosses
        return (side == 'B') ? kMid - depth(rng) * 100 : kMid + depth(rng) * 100;
    };

    auto emit_add = [&](char side) {
        const auto sh = static_cast<std::uint32_t>(shares_d(rng));
        std::vector<unsigned char> m; m.push_back('A'); stamp(m);
        be64(m, next_ref); m.push_back(static_cast<unsigned char>(side));
        be32(m, sh);
        for (int i = 0; i < 8; ++i) m.push_back(' ');  // stock symbol
        be32(m, static_cast<std::uint32_t>(price_for(side)));
        frame(out, m);
        live.push_back({next_ref++, side, sh});
    };

    auto pick_index = [&]() {
        return std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng);
    };
    auto swap_remove = [&](std::size_t i) { live[i] = live.back(); live.pop_back(); };

    for (long i = 0; i < num_adds; ++i) {
        emit_add((i & 1) ? 'B' : 'S');

        if (live.size() < 8) continue;  // keep a working set before churning
        const int r = coin(rng);
        if (r < 45) {  // Delete: remove the whole order
            const std::size_t i = pick_index();
            std::vector<unsigned char> m; m.push_back('D'); stamp(m); be64(m, live[i].ref); frame(out, m);
            swap_remove(i);
        } else if (r < 60) {  // Cancel 1 share; order dies if it reaches zero
            const std::size_t i = pick_index();
            std::vector<unsigned char> m; m.push_back('X'); stamp(m);
            be64(m, live[i].ref); be32(m, 1); frame(out, m);
            if (--live[i].shares == 0) swap_remove(i);
        } else if (r < 70) {  // Execute 1 share; order dies if it reaches zero
            const std::size_t i = pick_index();
            std::vector<unsigned char> m; m.push_back('E'); stamp(m);
            be64(m, live[i].ref); be32(m, 1); be64(m, ts); frame(out, m);
            if (--live[i].shares == 0) swap_remove(i);
        } else if (r < 78) {  // Replace: retire old ref, install new on the same side
            const std::size_t i = pick_index();
            const Live old = live[i];
            const auto sh = static_cast<std::uint32_t>(shares_d(rng));
            std::vector<unsigned char> m; m.push_back('U'); stamp(m);
            be64(m, old.ref); be64(m, next_ref); be32(m, sh);
            be32(m, static_cast<std::uint32_t>(price_for(old.side)));
            frame(out, m);
            live[i] = {next_ref++, old.side, sh};
        }
    }

    FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::printf("wrote %s: %zu bytes, %ld adds, %zu still resting\n",
                out_path.c_str(), out.size(), num_adds, live.size());
    return 0;
}
