# AGENTS.md

## What this repo is
SystemC 3.0.2 / TLM-2.0 example of a DRAM controller in C++26. One executable,
`spike_dram` (`src/spike_dram_main.cpp`), embeds the RISC-V Spike ISS and drives two
controller models that share `dram::DramControllerBase`: `DramControllerAsync`
(untimed, arbitrary `sc_time` latencies) and `DramControllerSync` (clocked via
`sc_in<bool> clk`). The base owns `kPorts = 2` tagged target sockets, read and write
queues, round-robin arbitration with a turnaround penalty, and back-pressure. A
write-back `StoreBuffer` sits in front of the controller so several transactions can
be in flight at once.

Both controllers forward every request through their `mem_socket` to a separate
`dram::DramChip` model (`include/dram_chip.h`) that owns the banks: it runs the
ACTIVATE/READ/WRITE/PRECHARGE state machine (tRCD/tCAS/tRP/tWR), decides row
hit/miss, records a full command trace to CSV, and returns the access delay; the
controller only waits it. `DramChip` is configured with a `DramTiming` whose
`quantum` is the clock period in sync mode and `SC_ZERO_TIME` in async mode — that
quantization is the whole timing difference between the two.

`slides.md` is the source lecture (Russian) the models follow; reference material,
not built. Some code comments are Russian to match it.

## Build & run
C++26, so it must link a SystemC built with C++26. `/usr/local/systemc-3.0.2` is
C++17-only and fails at link; `CMakeLists.txt` defaults `SYSTEMC_ROOT` to the local
C++26 rebuild `/home/viktor/mipt/masters_diploma/systemc-cxx26` and auto-selects
`clang++` (`/opt/llvm-project/build/bin/clang++`), so no flags are normally needed:

```sh
cmake -S . -B build
cmake --build build
tests/spike/build.sh fib
./build/spike_dram tests/spike/fib.elf
```

- Override the library with `-DSYSTEMC_ROOT=<prefix>` or `-DCMAKE_PREFIX_PATH=...`.
- AddressSanitizer: `cmake -S . -B build-asan -DDRAM_ASAN=ON`. The default clang++ 24
  ships no ASan runtime, so also pass `-DCMAKE_CXX_COMPILER=/usr/bin/g++`.
- `BUILD_RPATH` points at the SystemC lib dir, so run the binary directly; do
  **not** export `LD_LIBRARY_PATH`.
- No test framework. Verification = run an ELF and confirm `exit code = 0` and the
  expected `reads`/`writes`/`conflicts`; `fib.elf` is the baseline.
- One target: `spike_dram` (Spike co-simulation, see below); the CMake `project()`
  is still named `dram_models` and `dram_common.h` comments mention async/sync DRAM.
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
  (`clk` driven by an `sc_clock`, period 10 ns) and `--async` builds
  `DramControllerAsync`; both live in a `unique_ptr<DramControllerBase>`, so the
  stats code is shared. C++ tests are compiled freestanding
  (`clang++ --target=riscv64 -nostdlib -ffreestanding -std=c++20`, no STL/libc)
  with `tests/spike/crt0.S` providing `_start`/`tohost`.
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
- `SPIKE_PLAN.md` has the full plan and next milestones.

## Architecture notes
- `include/dram_chip.h` (`dram::DramChip`) owns the byte array (`dram::DramStorage`
  from `dram_common.h`) **and** the bank/row state machine + trace. It exposes one
  `simple_target_socket` and computes access delay in `b_transport`.
- `DramControllerBase` no longer stores data: it only arbitrates. After picking a
  request it calls `mem_socket->b_transport(*trans, delay)` to reach `DramChip`, then
  `serve()` does `wait(delay)`.
- `DramChip` decodes `[col | bank | row]` (defaults 6/2 bits), keeps per-bank
  `state` (`Idle`/`Active`) and `open_row`, and runs the FSM: precharge on row
  conflict, then activate, then the column command. Each step is pushed onto
  `trace_` and dumped to CSV via `dump_trace()`.
- `DramTiming.quantum`: if non-zero every delay is rounded **up** to a multiple of it.
  `sc_main` sets it to the clock period for `--sync` and `SC_ZERO_TIME` for `--async`,
  which is what makes sync vs async traces diverge.
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
  constructor. Both call `issue()` (chip-driven delay) then `wait(delay)`; async
  issues immediately, sync waits `clk.posedge_event()` first and also quantizes the
  turnaround penalty with `cycles(n) = clock_period_ * n`.
- `--trace <file>` overrides the CSV path (default `dram_trace_<mode>.csv`). Run
  `tests/spike/run_both.sh write_burst` to see sync vs async diverge: `write_burst`
  writes a long streaming region, then reads a same-bank/other-row region; the
  backlog that bleeds across the two phases depends on service speed, so the two
  controllers produce different row hit/miss and different command counts.
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
