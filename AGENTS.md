# AGENTS.md

## What this repo is
SystemC 3.0.2 / TLM-2.0 example of a DRAM controller in C++26. One executable,
`dram_controller` (`src/controller_main.cpp`), instantiates two controller models
that share `dram::DramControllerBase`: `DramControllerAsync` (untimed, arbitrary
`sc_time` latencies) and `DramControllerSync` (clocked via `sc_in<bool> clk`,
whole-cycle latencies). The base owns `kPorts = 4` tagged target sockets, read and
write queues, round-robin arbitration with a turnaround penalty, and back-pressure.

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
./build/dram_controller
```

- Override the library with `-DSYSTEMC_ROOT=<prefix>` or `-DCMAKE_PREFIX_PATH=...`.
- AddressSanitizer: `cmake -S . -B build-asan -DDRAM_ASAN=ON`. The default clang++ 24
  ships no ASan runtime, so also pass `-DCMAKE_CXX_COMPILER=/usr/bin/g++`.
- `BUILD_RPATH` points at the SystemC lib dir, so run the binary directly; do
  **not** export `LD_LIBRARY_PATH`.
- No test framework. Verification = run the binary and confirm `OK` for all eight
  initiators, each controller reporting `reads=64 writes=64 conflicts=124`.
- Only the `dram_controller` target exists, even though the CMake `project()` is
  named `dram_models` and `dram_common.h` comments still mention async/sync DRAM.
- The parent dir `/home/viktor/mipt/masters_diploma/` holds the SystemC source
  tree and tarball. Do not edit or treat those as this project.

## Architecture notes
- `include/dram_common.h` (`dram::DramStorage`) owns the byte array and payload
  read/write logic; the controller only arbitrates and annotates timing.
- `DramControllerBase` registers `b_transport` on each tagged socket via
  `register_b_transport(this, &DramControllerBase::b_transport, i)`. The method is
  `private` and takes `int port` first; this `(int, payload&, time&)` overload only
  exists on `simple_target_socket_tagged`, not `simple_target_socket`.
- `b_transport` is blocking, not LT-style: it queues the request, notifies
  `new_request_`, then `wait(done)` on a **local** `sc_event` (whose address is
  stored in the queued `Request`), then sets `delay = SC_ZERO_TIME` because the
  scheduler already advanced time.
- `next_request()` increments `conflicts_` when both queues are non-empty and picks
  direction round-robin from `rr_`. `turnaround()`/`remember()` track the last
  direction so a read/write switch waits `switch_penalty`.
- `serve()` is pure virtual; each subclass starts it with `SC_THREAD(serve)` in its
  constructor. Async waits arbitrary `sc_time`; sync waits `clk.posedge_event()`
  and `cycles(n) = clock_period_ * n`.
- `src/controller_main.cpp`: 4 async initiators start at t=0; `Director` waits for
  them, then releases the 4 sync initiators via `BenchState::start`, prints stats
  and calls `sc_stop()`.

## SystemC 3.0 / IEEE 1666-2023 quirks
- SystemC and user code must be built with the **same** `-std`: headers reference
  an ABI symbol like `sc_api_version_3_0_2_cxx202400L`, so a standard mismatch
  shows up as an undefined reference at link (not a compile error). Changing
  `CMAKE_CXX_STANDARD` means rebuilding SystemC too.
- `SC_HAS_PROCESS` is obsolete; omit it.
- `sc_dt` is a top-level namespace, not `sc_core::sc_dt`.
- Avoid the deprecated `sc_time(uint64, bool)` constructor; use `sc_time * double`.
- With an `sc_clock` present the simulation never starves, so `sc_start()` returns
  only because `Director` calls `sc_stop()`.

## Style
- `.clang-format` is the source of truth: 80 columns, 2-space indent, pointer
  right. It uses Allman braces (`BreakBeforeBraces: Custom`, `AfterFunction` and
  `AfterControlStatement` true, `IndentBraces: false`), so **`{` goes on its own
  line** for functions and if/for/while/switch — write code that way before running
  `clang-format -i` on changed C++ files.
- `sc_main` uses `using namespace sc_core;`; elsewhere `sc_core::` is written out.
