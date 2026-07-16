# itch-orderbook

A from-scratch **NASDAQ TotalView-ITCH 5.0** parser and **limit-order-book
reconstructor** in modern C++20. It memory-maps a raw daily feed file, decodes
the message stream zero-copy, reconstructs the full book per stock, and reports
throughput and per-message latency.

I write market-data feed handlers and order books professionally; that code is
proprietary. This is a public artifact of the same craft, built on a spec and
sample data that are freely available.

## What it does

- Memory-maps a `NASDAQ_ITCH50` file and iterates messages via ITCH's 2-byte
  big-endian length framing — no per-message allocation or copy.
- Decodes the book-affecting message types: Add (A/F), Execute (E/C),
  Cancel (X), Delete (D), Replace (U); counts everything else.
- Reconstructs the book with a choice of three implementations behind one
  interface (select at the CLI): market-by-price on a `std::map`, market-by-
  price on a flat sorted vector, and full market-by-order with price-time
  priority.
- Reports message counts by type, throughput (M msgs/s), and ns/message.

## Architecture: transport-agnostic core

The decoders and the order book never learn where the bytes came from. Layers:

```
 source            framer                  dispatch            book
 ──────            ──────                  ────────            ────
 mmap file  ──►  parse()        (BinaryFILE) ─┐
                                              ├─► dispatch_message() ──► OrderBookSet
 UDP socket ──►  MoldUdp64Framer (live)      ─┘        (the switch)      / MboOrderBookSet
```

- **Decoders** (`protocol/messages.hpp`) are byte views — source-agnostic.
- **`dispatch_message()`** (`protocol/dispatch.hpp`) turns one framed message
  into a book call — source-agnostic.
- **The book** (`book/`) applies semantics — source-agnostic.
- Only the **framer** knows the transport: `parse()` (`transport/parser.hpp`)
  handles the file's contiguous 2-byte-length stream; `MoldUdp64Framer`
  (`transport/moldudp64.hpp`) handles live UDP packets (outer header, per-message
  sequencing, gap/duplicate detection). Both feed the identical
  `dispatch_message()`.

The headers mirror these layers: `include/itch/{core,protocol,transport,book}/`
plus `report.hpp`.

So going live added a framer and a socket (`udp_source.hpp`, the `itch_live`
binary) and changed nothing in the decoders or the book. `itch_live` receives a
MoldUDP64 multicast feed through exactly the same decode+book path as replay.

> **Scope note.** The live UDP client is a **functional demonstration that the
> core is transport-agnostic — for testing, not a fully optimized production
> ingest.** It uses a blocking `recvfrom` (no busy-poll / kernel bypass), is
> single-threaded (no separate NIC-read / book-build cores over a lock-free
> ring), and does no A/B-line arbitration, TCP retransmit recovery, or
> cross-datagram reassembly. Those are the production pieces on the roadmap. The
> optimized, benchmarked work here is the parser and the order book.

## Design: the central tension

Cancels, executes, deletes and replaces all reference a prior order by its
**reference number**, so we need O(1) lookup by ref. We *also* want a cheap
inside-of-book. Those pull toward different structures, so the book keeps both:
a hash map keyed by ref, and per-side price levels whose inside is O(1) to read.
The subtle correctness work is the **cumulative** share semantics — E/C/X deduct
against a live order, and an order that reaches zero shares must be removed and
its price level dropped.

Three book models are implemented and benchmarked head-to-head:

- **MBP / `std::map`** (`MapLevels`) — aggregate shares per price in a red-black
  tree. The simple, obviously-correct baseline.
- **MBP / flat vector** (`FlatLevels`) — aggregate shares per price in a sorted
  vector; best price at `front()`. Cache-friendly; the fastest for BBO/depth.
- **MBO / price-time FIFO** (`MboBook`) — every order is a node in an intrusive
  doubly-linked FIFO at its price level, pool-allocated, with an O(1) back-
  pointer for unlink. This is the only model that can answer time-priority
  questions; `queue_position(ref)` returns the shares resting ahead of an order.

For BBO and size-at-price the three print identical numbers — MBO earns its
extra cost only when you need queue position or fill simulation.

## Build

Requires CMake ≥ 3.20 and a C++20 compiler (GCC 13+/Clang 16+/Apple Clang 14+).

```bash
git clone https://github.com/Other-Now/itch-orderbook.git
cd itch-orderbook
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Tests (Catch2) and benchmarks (Google Benchmark) build automatically if those
libraries are installed, and are skipped otherwise:

```bash
brew install catch2 google-benchmark   # optional
ctest --test-dir build --output-on-failure
```

## Run

The repo ships a small synthetic feed (`data/synthetic.NASDAQ_ITCH50`), so it
runs immediately after building. Regenerate or resize it any time with
`itch_gen`; fetch a real Nasdaq day with `tools/download_sample.sh`.

```bash
./build/itch_replay        data/synthetic.NASDAQ_ITCH50   # MBP / std::map
./build/itch_replay --flat data/synthetic.NASDAQ_ITCH50   # MBP / flat vector
./build/itch_replay --mbo  data/synthetic.NASDAQ_ITCH50   # MBO / price-time FIFO

./build/itch_gen 2000000 data/big.NASDAQ_ITCH50   # custom-size synthetic feed
./tools/download_sample.sh                        # or a real Nasdaq day into data/
```

Tests and benchmarks:

```bash
ctest --test-dir build --output-on-failure        # Catch2 unit tests
./build/itch_bench --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true       # Google Benchmark
```

## Validation (real data)

Replayed a real Nasdaq TotalView-ITCH 5.0 feed (2019-01-30, 214 MiB,
**7,628,856 messages**, 7,757 symbols):

- **0 length mismatches** — every message decoded at exactly its ITCH canonical
  length, i.e. the framing and field offsets are correct against real data.
- **0 unknown references** — every Execute / Cancel / Delete / Replace resolved
  to a live prior order, i.e. the order-lifetime and cumulative-share semantics
  are correct across millions of real events.

## Benchmarks

Steady-state add + delete churn (the realistic regime, ~50 levels/side), measured
on two machines. **The winner depends on the platform**, so both columns are shown
rather than one headline number:

| Book model | Apple M2 (arm64) | AMD EPYC 9654 (x86) | uniquely answers |
| --- | --- | --- | --- |
| MBP, `std::map` | 80.3 ns (1.00×) | 91.5 ns (1.00×) | — |
| MBP, flat vector | **58.4 ns (1.38× faster)** | 97.1 ns (0.94×, *slower*) | — |
| MBO, price-time FIFO | 70.9 ns (1.13× faster) | **76.9 ns (1.19× faster)** | queue position, fills |

_M2: MacBook Air (fanless), Apple Clang 14. EPYC: g++ 11.5, glibc 2.35. Both `-O3`;
steady-state churn, median of repeated runs, cv < 1%._ The M2's fanless throttling
makes whole-file wall-clock replay swing ~2×, so only the micro-benchmark above is
trustworthy there. Select the model at the CLI with
`./build/itch_replay [--flat | --mbo] <file>`; all three agree on BBO and
size-at-price on real data (0 mismatches, 0 unknown refs).

> **Reading the table: the flat-vector win is platform-dependent; MBO's is not.**
>
> Each churn op either allocates+frees a red-black-tree node (map) or `memmove`s a
> contiguous vector (flat), so which wins is decided by the platform's allocator,
> not by the data structure. An isolation benchmark on the EPYC box — plain
> `std::map` vs a pool-backed `std::pmr::map` vs the flat vector, same tree and
> comparisons, only the node allocator differing — makes the mechanism concrete:
>
> - *Level churn (alloc-heavy):* flat 26.8 ns, map 30.4 ns, **`pmr::map` 38.2 ns**.
>   Pooling the map's nodes made it *slower*, not faster: glibc's per-thread tcache
>   already services fixed-size node churn about as cheaply as a bare free-list, so
>   a generic pool resource only adds bookkeeping. Allocation was never the map's
>   bottleneck on glibc — which is why the M2's flat-vector win doesn't reproduce.
> - *Level touch (no alloc):* map 6.5 ns, flat 9.6 ns. Remove allocation and the
>   red-black tree is actually *faster* than the vector at ~50 levels.
>
> So on the M2 (libmalloc, pricier node alloc/free) the flat vector's `memmove`
> wins by 1.38×; on x86/glibc the two roughly tie and flat lands ~6% behind. The
> flat vector is **not** universally faster — its advantage is an allocator
> artifact. On sub-penny synthetic books with ~20,000 levels it is ~4× *slower*
> on both, as its O(levels) insert loses to the tree once depth is large.
>
> The **MBO** book is the robust result: its slab pool removes per-order allocation
> from the hot path independent of the system allocator, so it is fastest on *both*
> machines (1.13× M2, 1.19× x86) — the price of that being queue-position and
> fill-simulation that an aggregate book structurally cannot answer.

Reproduce the allocator study on your own machine with `./build/itch_bench_levels`
(no dependencies — built-in timing harness).

## Status

Done: parse + three book models (MBP map, MBP flat, MBO price-time FIFO with a
pool allocator and `queue_position`), Catch2 unit tests, Google Benchmark suite,
the measured MBP↔MBO tradeoff and the cross-platform (M2 / x86) allocator study
showing the flat-vector win is allocator-dependent while MBO's holds on both,
ASan/UBSan-clean, and GitHub Actions CI.

Roadmap: NOII/cross message handling, A/B-line arbitration and TCP retransmit
recovery for the live path, and a lock-free parse/book hand-off.

## License

MIT.
