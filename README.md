# Limit order book in C++20

![CI](https://github.com/YOUR_GITHUB/clob-matching-engine/actions/workflows/ci.yml/badge.svg)

A single-threaded order book that matches orders by price, then by arrival time.
It supports limit, market and cancel orders. All memory is allocated when the book is
created, so nothing is allocated while orders are processed. Header-only, no dependencies.

## Example

```cpp
#include <clob/OrderBook.hpp>
#include <memory>

auto book = std::make_unique<clob::OrderBook<>>();   // ~8 MB, so not on the stack

book->add_order(1, clob::Side::Sell, 700'500, 10);    // rests on the book
auto r = book->add_order(2, clob::Side::Buy, 700'500, 4);
// r.filled == 4, r.rested == 0; order 1 has 6 left

book->cancel_order(1);
```

Prices are whole ticks. The default book covers prices 700,000 to 702,047 and holds up to
131,072 resting orders; both are template parameters.

## How it works

- Orders live in a fixed pool of 32-byte slots. Each price level keeps a linked list of its orders, oldest first.
- Price levels are an array indexed by `price - BasePrice`.
- A bitmap with one bit per price level is used to find the next best price when a level empties.
- Cancels find the order through a hash table keyed by order ID.
- Bad orders are rejected with a reason: price out of range, zero quantity, an ID that is already live, or a full pool.
- You can pass a listener type to get a callback for every trade.

## Build

```
cmake -B build
cmake --build build --config Release
```

Then run `build/tests` and `build/benchmark` (on Windows: `build/Release/tests.exe`).
For tests with AddressSanitizer and UBSan on Linux: `cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCLOB_SANITIZE=ON`.

## Tests

`tests/tests.cpp` covers priority order, partial fills, cancels, market orders, rejects and a full
pool. It then sends the same 900,000 random orders to this book and to `MapOrderBook`, a simple
version built on `std::map` and `std::list`, and checks that both produce exactly the same trades.

## Benchmark

`bench/benchmark.cpp` generates 1M random adds and cancels, runs them 20 times on each book and
prints the median. It stops with an error if the two books don't agree.

| Machine | Compiler | Orders/s | ns per order | vs std::map |
| --- | --- | --- | --- | --- |
| [CPU] | MSVC, Release | [X]M | [X] | [X]x |

## Limitations

- Single-threaded, one instrument.
- Fixed price range and pool size, chosen at compile time.
- No order modify yet, and no market data feed yet.

## Next

- Order modify (reduce quantity keeps priority, price change loses it)
- Replay of recorded exchange L2 data
