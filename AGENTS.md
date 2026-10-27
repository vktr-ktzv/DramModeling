# AGENTS.md

## What this repo is
SystemC 3.0.2 / TLM-2.0 example of a DRAM controller in C++26. One executable,
`spike_dram` (`src/spike_dram_main.cpp`), embeds the RISC-V Spike ISS and drives two
controller models of different fidelity that share `dram::DramControllerBase`:
`DramControllerAsync` (detailed, arbitrary `sc_time` latencies from the bank-level
`DramChip`) and `DramControllerSync` (simple, fixed read/write latency in clock
cycles, clocked via `sc_in<bool> clk`). The base owns `kPorts = 2` tagged target
sockets, read and write queues, round-robin arbitration with a turnaround penalty,
and back-pressure. A write-back `StoreBuffer` sits in front of the controller so
several transactions can be in flight at once.

Both controllers forward every request through their `mem_socket`, but to different
targets. Async talks to `dram::DramChip` (`include/dram_chip.h`), which owns the
banks: it runs the ACTIVATE/READ/WRITE/PRECHARGE state machine (tRCD/tCAS/tRP/tWR),
decides row hit/miss, records a full command trace to CSV, and returns the exact
access delay; the controller only waits it. Sync talks to `dram::SimpleMemory`
(`include/simple_memory.h`), which only stores bytes and adds no delay; the sync
controller waits a fixed `read_latency_cycles`/`write_latency_cycles`. So sync has
no bank state, row hit/miss, activations, or precharge — it is deliberately the
coarser model.

`tools/trace_stats.py` is a dependency-free Python 3 helper that parses the CSV
traces from either model and compares them via `mem_busy = access_time +
turnaround_time` (see "Trace analysis" below). It is not part of the C++ build.

`../perf-modeling-lectures` holds the source lectures (Russian) the models
follow; reference material, not built. Some code comments are Russian to match it.

## Build & run
C++26, so it must link a SystemC built with C++26. `/usr/local/systemc-3.0.2` is
C++17-only and fails at link; `CMakeLists.txt` defaults `SYSTEMC_ROOT` to the local
C++26 rebuild `/home/viktor/mipt/masters_diploma/systemc-cxx26` and auto-selects
`clang++` from `PATH`, so no flags are normally needed:

```sh
cmake -S . -B build
cmake --build build
tests/spike/build.sh fib
./build/spike_dram tests/spike/fib.elf
```

- Override the library with `-DSYSTEMC_ROOT=<prefix>` or `-DCMAKE_PREFIX_PATH=...`.
- `BUILD_RPATH` points at the SystemC lib dir, so run the binary directly; do
  **not** export `LD_LIBRARY_PATH`.
- No test framework. Verification = run an ELF and confirm `exit code = 0` and the
  expected `reads`/`writes`/`conflicts`; `fib.elf` is the baseline.
- One target: `spike_dram` (Spike co-simulation, see below); the CMake `project()`
  is still named `dram_models`.
- The parent dir `/home/viktor/mipt/masters_diploma/` holds the SystemC source
  tree and tarball. Do not edit or treat those as this project.

## Spike co-simulation (`spike_dram`)
Embeds a built `riscv-isa-sim` (`SPIKE_ROOT`, default
`/home/viktor/mipt/masters_diploma/riscv-isa-sim`) into an `sc_main`; RISC-V
loads/stores to `0x4000_0000`+ go through the TLM controller. The Spike lib is
static/dynamic C++17/g++ while our TU is C++26/clang — both on libstdc++, which
links fine. Build and run:

```sh
tests/spike/build.sh                      # dram_test.elf (asm); `build.sh fib` -> C++
./build/spike_dram --async tests/spike/fib.elf   # default is async
./build/spike_dram --sync  tests/spike/fib.elf
tests/spike/run_both.sh fib                       # same ELF on both controllers
```

- `sc_main` picks the controller by flag: `--sync` builds `DramControllerSync`
  (`clk` driven by an `sc_clock`, period 10 ns) wired to a `SimpleMemory`;
  `--async` builds `DramControllerAsync` wired to a `DramChip`. Both live in a
  `unique_ptr<DramControllerBase>`, so the stats code is shared. C++ tests are
  compiled freestanding (`clang++ --target=riscv64 -nostdlib -ffreestanding
  -std=c++20`, no STL/libc) with `tests/spike/crt0.S` providing `_start`/`tohost`.
- `sim_t::add_device(base, dev)` routes MMIO; `bus_t` calls `dev->load/store`
  with an **already offset** address (`paddr - base`), so `SpikeTlmDevice` must
  not subtract the base again.
- The blocking `b_transport` (`wait(done)`) only works because `sim_t::run()` is
  called from an `SC_THREAD` (`SpikeRunner`) inside `sc_start()`; never call it
  from Spike's own loop.
- Time is decoupled from memory: `CosimSim` subclasses `sim_t` and overrides the
  virtual `idle()` to run one `processor_t::step(1)` then `wait(cycle)`, so the
  SystemC host owns the scheduler (no caches; reads block, writes are buffered).
  `cycle` equals the clock period (10 ns). Program exit is the HTIF
  `tohost`/`fromhost` protocol (write `(code<<1)|1` to `tohost`); the ELF must
  define those symbols.
- Do **not** add `${SPIKE_ROOT}/fesvr` to include paths: its `syscall.h` shadows
  glibc's and breaks `<memory>`; use `${SPIKE_ROOT}` for `<fesvr/htif.h>`.
- `triggers`/CLINT/PLIC are absent because `sim_t` is built with
  `dtb_enabled=false` and `start_pc` set explicitly.

## Trace analysis (`tools/trace_stats.py`)
Dependency-free Python 3; reads the CSV written by `--trace`, auto-detects the
format from the header, and reports `mem_busy = access_time + turnaround_time`:

```sh
./build/spike_dram --async --trace /tmp/async.csv tests/spike/write_burst.elf
./build/spike_dram --sync  --trace /tmp/sync.csv  tests/spike/write_burst.elf
python3 tools/trace_stats.py /tmp/async.csv /tmp/sync.csv
```

- async: `access_time` is the sum of `latency_ns` over all command rows (so
  ACT/PRE/RD/WR contributions and row hit/miss are included); sync has no
  `latency_ns`, so it uses `reads * --read-ns + writes * --write-ns` (defaults
  30/20 ns = 3/2 cycles at 10 ns).
- `turnaround_time` is the number of `RD`↔`WR` changes among access rows times
  `--switch-ns` (default 20 ns) for both models.
- With two traces, async is the reference; comparison prints `mem_busy_ns`,
  `access_ns`, and `avg_mem_ns` (`turnaround_ns` only appears in the table).
  `--mode auto|async|sync` overrides detection; `--csv-out <file>` writes metrics.
- Do not compare bank counters: `SimpleMemory` has none. Compare `mem_busy` and
  its components, or full simulation time separately (it includes instruction
  cycles and idle time, which `mem_busy` does not).

## Architecture notes
- `include/dram_chip.h` (`dram::DramChip`) owns the byte array (`dram::DramStorage`
  from `dram_common.h`) **and** the bank/row state machine + trace. It is the async
  target only. It exposes one `simple_target_socket` and computes access delay in
  `b_transport`.
- `include/simple_memory.h` (`dram::SimpleMemory`) is the sync target: it owns a
  `DramStorage` and applies data in `b_transport`, but adds **no** delay and keeps
  no bank state. It can dump a minimal access trace (`time, command, type, addr,
  len`) to CSV.
- `DramControllerBase` no longer stores data: it only arbitrates. After picking a
  request it calls `mem_socket->b_transport(*trans, delay)` to reach the selected
  memory model; async then waits the returned delay, sync waits its fixed latency.
- `DramChip` decodes `[col | bank | row]` (defaults 6/2 bits), keeps per-bank
  `state` (`Idle`/`Active`) and `open_row`, and runs the FSM: precharge on row
  conflict, then activate, then the column command. Each step is pushed onto
  `trace_` and dumped to CSV via `dump_trace()`.
- `DramTiming` is used only by `DramChip` (async); there is no quantum/rounding.
- `DramControllerBase` registers `b_transport` on each tagged socket via
  `register_b_transport(this, &DramControllerBase::b_transport, i)`. The method is
  `private` and takes the socket tag as a first `int` argument that nobody reads
  (queues are chosen by `trans.get_command()`); this `(int, payload&, time&)` overload only
  exists on `simple_target_socket_tagged`, not `simple_target_socket`.
- `b_transport` is blocking, not LT-style: it queues the request, notifies
  `new_request_`, then `wait(done)` on a **local** `sc_event` (whose address is
  stored in the queued `Request`), then sets `delay = SC_ZERO_TIME` because the
  scheduler already advanced time.
- `next_request()` increments `conflicts_` when both queues are non-empty and picks
  direction round-robin from `rr_`. `turnaround()`/`remember()` track the last
  direction so a read/write switch waits `switch_penalty`.
- `serve()` is pure virtual; each subclass starts it with `SC_THREAD(serve)` in its
  constructor. Async calls `issue()` and waits the chip-computed delay immediately.
  Sync waits `clk.posedge_event()` first, calls `issue()` (SimpleMemory returns
  zero), then waits `cycles(read_latency_cycles | write_latency_cycles)`; a
  direction change additionally waits `cycles(switch_penalty_cycles)`, so sync also
  pays the 20 ns turnaround penalty by default.
- `--trace <file>` overrides the CSV path (default `dram_trace_<mode>.csv`). Async
  writes bank commands (`ACT/PRE/RD/WR`); sync writes one row per access (`RD/WR`).
  Run `tests/spike/run_both.sh write_burst` to compare: the async detailed model
  shows act/precharge/hit/miss, while sync only shows fixed-latency accesses. The
  two also differ in request ordering/conflict counts because service speed feeds
  back into the store buffer backlog.
- `src/spike_dram_main.cpp`: `SpikeRunner` (an `SC_THREAD`) runs the whole `sim_t`
  inside `sc_start()`; on exit it calls `store_buffer.flush()` and `sc_stop()`.
- `include/store_buffer.h` / `src/store_buffer.cpp`: `store()` is non-blocking
  (queue + `kWriteLanes = 3` drain threads); `load()` forwards not-yet-applied
  writes (store-to-load) and serializes overlapping writes to keep program order.

## SystemC 3.0 / IEEE 1666-2023 quirks
- SystemC and user code must be built with the **same** `-std`: headers reference
  an ABI symbol like `sc_api_version_3_0_2_cxx202400L`, so a standard mismatch
  shows up as an undefined reference at link (not a compile error). Changing
  `CMAKE_CXX_STANDARD` means rebuilding SystemC too.
- `SC_HAS_PROCESS` is obsolete; omit it.
- `sc_dt` is a top-level namespace, not `sc_core::sc_dt`.
- Avoid the deprecated `sc_time(uint64, bool)` constructor; use `sc_time * double`.
- With an `sc_clock` present the simulation never starves, so `sc_start()` returns
  only because `SpikeRunner` calls `sc_stop()`.

## Style
- `.clang-format` is the source of truth: 120 columns, 4-space indent (access
  modifiers at -2), pointer/reference **left** (`Type* p`, `Type& r`). It uses
  Allman braces (`BreakBeforeBraces: Custom`, `AfterFunction` and
  `AfterControlStatement` true, `IndentBraces: false`), so **`{` goes on its own
  line** for functions and if/for/while/switch — write code that way before running
  `clang-format -i` on changed C++ files.
- `sc_main` uses `using namespace sc_core;`; elsewhere `sc_core::` is written out.
