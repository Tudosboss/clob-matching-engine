// Build in Release mode (see README) before trusting the numbers.
#include <clob/OrderBook.hpp>
#include <clob/MapOrderBook.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

struct Event {
    uint8_t is_cancel;
    clob::Side side;
    clob::OrderId id;
    clob::Price price;
    clob::Quantity qty;
};

struct RunStats { double min, median, max; };

static RunStats summarize(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return { v.front(), v[v.size() / 2], v.back() };
}

int main() {
    constexpr size_t NUM_EVENTS = 1000000; // 1M events per run
    constexpr size_t RUNS = 20;            // 20M total ops per engine
    // Keep at most this many live orders, so the book never runs out of pool slots.
    constexpr size_t MAX_LIVE_IDS = 50000;

    // Generate all events before timing anything
    std::vector<Event> events;
    events.reserve(NUM_EVENTS);
    std::mt19937_64 rng(42);
    std::vector<clob::OrderId> active_ids;
    active_ids.reserve(65536);

    size_t num_cancels = 0;
    clob::OrderId next_id = 1000000001ULL; // IDs don't start at 0, like real exchange IDs
    for (size_t i = 0; i < NUM_EVENTS; ++i) {
        if (!active_ids.empty() && (active_ids.size() >= MAX_LIVE_IDS || (rng() % 100) < 35)) {
            size_t idx = rng() % active_ids.size();
            clob::OrderId cid = active_ids[idx];
            active_ids[idx] = active_ids.back();
            active_ids.pop_back();
            events.push_back({ 1, clob::Side::Buy, cid, 0, 0 });
            ++num_cancels;
        }
        else {
            clob::Side side = (rng() & 1) ? clob::Side::Buy : clob::Side::Sell;
            clob::Price offset = static_cast<clob::Price>(rng() % 300);
            clob::Price price = (side == clob::Side::Buy) ? (700850 + offset) : (700950 + offset);
            clob::Quantity qty = static_cast<clob::Quantity>(1 + (rng() % 50));
            clob::OrderId id = next_id++;
            active_ids.push_back(id);
            events.push_back({ 0, side, id, price, qty });
        }
    }

    auto clob_book = std::make_unique<clob::OrderBook<>>();
    clob::MapOrderBook<> map_book;

    // Warm-up run, also checks that both books give the same results
    uint64_t clob_matched = 0, clob_cancels = 0, map_matched = 0, map_cancels = 0;
    uint64_t rejects = 0;
    uint32_t peak_live = 0;
    for (const auto& ev : events) {
        if (ev.is_cancel) {
            clob_cancels += clob_book->cancel_order(ev.id);
            map_cancels += map_book.cancel_order(ev.id);
        }
        else {
            auto r = clob_book->add_order(ev.id, ev.side, ev.price, ev.qty);
            clob_matched += r.filled;
            rejects += (r.reject != clob::Reject::None);
            map_matched += map_book.add_order(ev.id, ev.side, ev.price, ev.qty);
        }
        peak_live = std::max(peak_live, clob_book->live_orders());
    }

    std::cout << "Check: CLOB (matched=" << clob_matched << ", cancels=" << clob_cancels
        << ") | Map (matched=" << map_matched << ", cancels=" << map_cancels << ")\n";
    std::cout << "Events: " << NUM_EVENTS << " events (" << (NUM_EVENTS - num_cancels) << " adds, "
        << num_cancels << " cancel requests), peak resting orders " << peak_live
        << " of " << clob_book->capacity() << "\n";

    if (clob_matched != map_matched || clob_cancels != map_cancels || rejects != 0) {
        std::cerr << "Error: the two books disagree (or the CLOB rejected " << rejects
                  << " orders), so timings would not be comparable.\n";
        return EXIT_FAILURE;
    }

    std::vector<double> clob_times_ns(RUNS);
    std::vector<double> map_times_ns(RUNS);
    uint64_t sink = 0;

    for (size_t r = 0; r < RUNS; ++r) {
        clob_book->reset();
        auto t0 = std::chrono::steady_clock::now();
        for (const auto& ev : events) {
            if (ev.is_cancel) sink += clob_book->cancel_order(ev.id);
            else sink += clob_book->add_order(ev.id, ev.side, ev.price, ev.qty).filled;
        }
        auto t1 = std::chrono::steady_clock::now();
        clob_times_ns[r] = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

        map_book.clear();
        auto m0 = std::chrono::steady_clock::now();
        for (const auto& ev : events) {
            if (ev.is_cancel) sink += map_book.cancel_order(ev.id);
            else sink += map_book.add_order(ev.id, ev.side, ev.price, ev.qty);
        }
        auto m1 = std::chrono::steady_clock::now();
        map_times_ns[r] = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(m1 - m0).count());
    }

    const RunStats c = summarize(clob_times_ns);
    const RunStats m = summarize(map_times_ns);

    double avg_ns_per_op = c.median / NUM_EVENTS;
    double throughput_m = (NUM_EVENTS / (c.median / 1e9)) / 1e6;
    double speedup = m.median / c.median;

    std::cout << "CLOB run time (ms)  min/median/max: " << c.min / 1e6 << " / " << c.median / 1e6 << " / " << c.max / 1e6 << "\n";
    std::cout << "Map  run time (ms)  min/median/max: " << m.min / 1e6 << " / " << m.median / 1e6 << " / " << m.max / 1e6 << "\n";
    std::cout << "Median throughput (" << RUNS << "x " << NUM_EVENTS / 1000000 << "M events): " << throughput_m << "M ops/sec\n";
    std::cout << "Average time per op: " << avg_ns_per_op << " ns\n";
    std::cout << "Speedup vs std::map version: " << speedup << "x faster (sink=" << sink << ")\n";
    return 0;
}
