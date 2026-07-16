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

_Machine: Apple M2 (MacBook Air, **fanless**), Apple Clang 14, `-O3`._

**Read this first.** The M2 Air throttles under sustained load, so whole-file
wall-clock replay swings ~2× with thermal state and is only a rough sanity
check. The trustworthy comparison is the Google Benchmark steady-state
micro-benchmark below — 5 repetitions, **stddev < 1%**.

Steady-state add + delete churn (the realistic regime, ~50 levels/side):

| Book model | ns / op | vs map | uniquely answers |
| --- | --- | --- | --- |
| **MBP, flat vector** | **58.4** | 1.38× faster | — |
| MBO, price-time FIFO | 70.9 | 1.13× faster | queue position, fills |
| MBP, `std::map` | 80.3 | baseline | — |

Select at the CLI with `./build/itch_replay [--flat | --mbo] <file>`. All three
agree on BBO and size-at-price on real data (0 mismatches, 0 unknown refs).

> **The tradeoffs:**
>
> *Flat vs map (MBP).* The flat vector wins ~1.38× because a real book keeps only
> tens of active levels per side, so contiguous storage is cache-friendly and
> `best()` is a single load. On synthetic books with ~20,000 sub-penny levels the
> flat vector is ~4× *slower* — its O(levels) insert loses to the tree once depth
> is large. The win is conditional on realistic depth.
>
> *MBO vs MBP.* The full order book is **faster than the `std::map` baseline**
> (70.9 vs 80.3 ns) *despite* maintaining a per-order FIFO, because its slab pool
> allocator removes the per-node heap traffic that dominates the `std::map`
> version. Relative to the fastest aggregate book (flat) it costs ~1.21× — a
> small price for queue-position / fill-simulation capability that aggregation
> structurally cannot provide.
>
> *A measurement-rigor note (worth saying out loud).* My first single-shot runs
> reported a 2.1× flat speedup and an MBO that looked ~1.8× slower — both partly
> thermal-throttling artifacts on this fanless machine. Measuring with
> repetitions and low variance gave the real, smaller, steady-state gaps above.
> Repeat-and-report-variance isn't optional on thermally-constrained hardware.
>
> Apple Silicon has 128-byte cache lines and no RDTSC; timing uses Google
> Benchmark's CPU timer. A cross-architecture x86 run (RDTSC cycle counts, on a
> thermally stable box) is planned.

## Status

Done: parse + three book models (MBP map, MBP flat, MBO price-time FIFO with a
pool allocator and `queue_position`), Catch2 unit tests, Google Benchmark suite,
the flat-vector optimization (~1.38× in stable benchmarks) and the measured
MBP↔MBO tradeoff, ASan/UBSan-clean, and GitHub Actions CI.

Roadmap: NOII/cross message handling, A/B-line arbitration and TCP retransmit
recovery for the live path, a lock-free parse/book hand-off, and an x86
cross-architecture (RDTSC cycle-count) comparison.

## License

MIT.
