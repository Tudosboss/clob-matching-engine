# Central Limit Order Book (CLOB) & Matching Engine

A modular C++20 and Python quantitative trading systems project designed to ingest Level-2 market data feeds, maintain a cache-friendly in-memory Limit Order Book, and evaluate cross-exchange futures basis and funding rate inefficiencies.

## System Architecture (4-Stage Roadmap)

1. **Stage 1: In-Memory Limit Order Book & Feed Normaliser (C++20)** *(In Progress)*
   - Fixed-size, 64-byte cache-line-aligned data structures (`Order`, `PriceLevel`, `OrderBook`).
   - Contiguous `std::array` memory layout avoiding dynamic heap allocations (`new`/`malloc`) during book updates.
   - Price-Time Priority (FIFO) order matching logic and $O(1)$ price-level indexing.
2. **Stage 2: Statistical Arbitrage & Signal Research (Python)** *(Planned)*
   - Historical analysis of cross-exchange perpetual funding rate clamps and L2 order book volume imbalances.
3. **Stage 3: Execution Friction & Margin Risk Simulator** *(Planned)*
   - Modeling net Expected Value (EV) after maker/taker fee tiers, order book depth slippage, and isolated margin liquidation thresholds.
4. **Stage 4: Hardware Packet Filter Simulation (VHDL)** *(Planned)*
   - Register-transfer level (RTL) finite state machine for deterministic market tick parsing.
