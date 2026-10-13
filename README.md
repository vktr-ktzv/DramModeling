# Модели DRAM на SystemC / TLM-2.0 и co-simulation со Spike

Единственный таргет — `spike_dram`: co-simulation, внутрь SystemC (`sc_main`)
встроено RISC-V-ядро Spike; обращения гостевой программы к диапазону DRAM идут
через TLM-контроллер. Один и тот же ELF можно прогнать на **асинхронном** или
**синхронном** контроллере (флаг `--async`/`--sync`).

## Структура проекта

```
.
├── CMakeLists.txt              # таргет spike_dram
├── README.md
├── ARCHITECTURE.md             # обзор архитектуры решения
├── AGENTS.md / SPIKE_PLAN.md   # заметки по архитектуре и план co-simulation
├── .clang-format               # стиль (120 колонок, Allman)
│
├── include/
│   ├── dram_common.h           # DramStorage: массив байт + обработка TLM-payload
│   ├── dram_controller_base.h  # база: 4 tagged target-сокета, очереди, арбитраж
│   ├── dram_controller_async.h # тайминг в произвольных sc_time
│   ├── dram_controller_sync.h  # тайминг в тактах, вход clk
│   ├── spike_dram_device.h     # SpikeTlmDevice (abstract_device_t) -> TLM
│   └── store_buffer.h          # write-back буфер: много транзакций в полёте
│
├── src/
│   ├── dram_controller_base.cpp
│   ├── dram_controller_async.cpp
│   ├── dram_controller_sync.cpp
│   ├── spike_dram_device.cpp
│   ├── store_buffer.cpp
│   └── spike_dram_main.cpp     # sc_main co-simulation (CosimSim + SpikeRunner)
│
└── tests/spike/
    ├── build.sh                # build.sh [name] — собирает <name>.cpp (+crt0) или <name>.S
    ├── run_both.sh             # run_both.sh [name] — один ELF на async и sync
    ├── crt0.S                  # старт C++-тестов: стек, вызов main(), выход через HTIF
    ├── link.ld                 # линкер-скрипт: код с 0x8000_0000
    ├── dram_test.S             # asm-тест: sw/lw в 0x4000_0000
    ├── fib.cpp                 # C++-тест: числа Фибоначчи через DRAM-контроллер
    ├── write_pair.cpp          # две записи + чтение: конфликты на async и sync
    └── blocked_copy.cpp        # блочный memcpy: много конфликтов чтение/запись
```

### Роли модулей

- `DramControllerBase` — общая часть: `kPorts = 4` tagged target-сокета, очереди
  чтения/записи, выбор направления round-robin с учётом `turnaround()`, счётчики
  `reads`/`writes`/`conflicts`. Регистрирует `b_transport` на каждом сокете.
- `DramStorage` — линейный массив байт; контроллер только арбитрирует и
  «размечает» время, данные читает/пишет хранилище.
- `DramControllerAsync` — поток `serve()` ждёт `read_latency` (70 нс) /
  `write_latency` (50 нс) и `switch_penalty` (20 нс) при смене направления.
- `DramControllerSync` — поток `serve()` синхронизируется по `clk.posedge_event()`,
  задержки заданы в тактах (read 3, write 2, switch 1).
- `SpikeTlmDevice` — `abstract_device_t` из Spike: его `load/store` вызывают
  `b_transport` контроллера. Адрес приходит **уже смещённым** (Spike `bus_t`
  вычитает базу устройства), поэтому второй раз базу вычитать нельзя.
- `CosimSim` (в `spike_dram_main.cpp`) — наследник `sim_t`, владеет временем.
- `SpikeRunner` — `SC_THREAD`, внутри которого целиком крутится Spike.
- Тип контроллера выбирается в `sc_main`: `--sync` создаёт `DramControllerSync`
  с `sc_clock` (период 10 нс) и подключает `ctrl.clk(clk)`; `--async`
  (по умолчанию) создаёт `DramControllerAsync`. Оба лежат в
  `std::unique_ptr<DramControllerBase>`, поэтому мосты и статистика одинаковы.

## Карта памяти co-simulation

| Диапазон                     | Что                    | Обслуживание         |
| ---------------------------- | ---------------------- | -------------------- |
| `0x4000_0000` (256 МиБ)      | MMIO DRAM-контроллера   | `SpikeTlmDevice` → TLM |
| `0x8000_0000` (2 ГиБ)        | ОЗУ Spike: код, стек, `tohost`/`fromhost` | `mem_t` (без контроллера) |

`start_pc = 0x8000_0000`, `dtb_enabled = false`, CLINT/PLIC/triggers не
поднимаются — для голых bare-metal тестов этого достаточно. Код и данные тестов
лежат в ОЗУ Spike, а через контроллер идут только явные обращения к
`0x4000_0000`.

## Как работает время

### Контроллер: блокирующий `b_transport` и `serve()`

`b_transport` **блокирующий**: он кладёт запрос в очередь, уведомляет
`new_request_` и делает `wait(done)` на локальном событии, адрес которого лежит
в запросе. Пока вызывающий спит, поток `serve()` контроллера продвигает время:

- async: `wait(switch_penalty)` при смене направления, затем
  `wait(write_latency | read_latency)`, потом `finish_request()` и
  `done.notify()`;
- sync: каждую итерацию `wait(clk.posedge_event())`, а задержки кратны периоду
  (`cycles(n) = clock_period_ * n`), поэтому нужен `sc_clock`.

К моменту возврата из `b_transport` планировщик уже продвинул время, поэтому
возвращаемая `delay` выставляется в `SC_ZERO_TIME`. Только поэтому
`b_transport` можно вызывать из `SC_THREAD`, но не из собственного цикла Spike.

### Co-simulation (`spike_dram`)

Время **отвязано** от обращений к памяти: SystemC-хост владеет планировщиком.

`htif_t::run()` (базовый цикл HTIF) устроен так: прочитать `tohost`; если он
нулевой — вызвать виртуальный `idle()`; иначе обработать команду (в т.ч. выход).
`CosimSim` переопределяет `idle()`:

```cpp
void idle() override
{
    if (done()) return;
    get_core(0)->step(1);      // ровно одна инструкция RISC-V
    ++insns_;
    sc_core::wait(cycle_);     // +1 такт SystemC (cycle_ = 10 нс)
}
```

То есть **1 инструкция = 1 такт** (такт = 10 нс, он же период `sc_clock` в
sync-режиме): время SystemC двигается на каждой инструкции, а не только при
обращении в память. HTIF-обработка `tohost`/`fromhost` и корректный выход
остаются из `htif_t::run()`.

Обращение гостя в диапазон DRAM устроено по-прежнему блокирующе:

```
ld/st → mmu_t → bus_t → SpikeTlmDevice::load/store → TlmBridge::transfer
      → b_transport → очередь → wait(done) → serve() двигает время → notify
```

Инструкция при этом «висит» на `wait(done)`, затем `idle()` всё равно добавляет
свой такт. Итоговое время складывается из тактов на инструкции и задержек DRAM:

```
T = N_insn * cycle
  + N_write * write_latency + N_read * read_latency
  + N_turnaround * switch_penalty
```

Задержки зависят от контроллера: async — 50/70/20 нс, sync — 2/3/1 такта
(20/30/10 нс при периоде 10 нс). Пример для `fib.elf` (93 инструкции, 20
записей, 38 чтений, 37 смен направления):

- async: `93*10 + 20*50 + 38*70 + 37*20 = 5330` нс;
- sync: `93*10 + 20*20 + 38*30 + 37*10 = 2840` нс.

Оба числа совпадают с выводом `spike_dram` и отличаются только таймингом —
данные и количество обращений к DRAM одинаковы.

Есть страховочный лимит `kMaxInsns` (50 млн): при его достижении симуляция
завершается с кодом выхода 1.

## Сборка и запуск

### Требования

- SystemC 3.0.2, собранный тем же стандартом C++26 (по умолчанию
  `/home/viktor/mipt/masters_diploma/systemc-cxx26`). Несовпадение `-std` даёт
  undefined reference на `sc_api_version_...`.
- Для `spike_dram`: собранный `riscv-isa-sim` (`SPIKE_ROOT`, по умолчанию
  `/home/viktor/mipt/masters_diploma/riscv-isa-sim`, нужен
  `build/libriscv.so`).
- Для тестов: `clang`/`clang++` с RISC-V-таргетом (`/opt/llvm/bin/`) и
  `riscv64-linux-gnu-ld`.

### Co-simulation со Spike

Бинарник несёт RPATH на библиотеку SystemC, поэтому `LD_LIBRARY_PATH`
экспортировать не нужно.

Сборка (тесты можно писать на ассемблере или на C++):

```sh
cmake -S . -B build
cmake --build build                    # соберёт spike_dram, если найден libriscv.so

tests/spike/build.sh                   # dram_test.elf (asm)
tests/spike/build.sh fib               # fib.elf из fib.cpp + crt0.S
```

Запуск одного и того же ELF на обоих контроллерах:

```sh
./build/spike_dram --async tests/spike/fib.elf   # по умолчанию тоже async
./build/spike_dram --sync  tests/spike/fib.elf

tests/spike/run_both.sh fib                      # оба режима подряд
```

Ожидаемый результат `fib.elf` — данные совпадают, тайминг разный:

```
=== async: fib ===
Spike finished: insns=191, time=7760 ns, exit code = 0
async controller: reads=58 writes=30 conflicts=1
DRAM[0..29]: 0 1 1 2 3 5 8 13 21 34 55 89 144 233 377 610 987 1597 2584 4181 6765 10946 17711 28657 46368 75025 121393 196418 317811 514229

=== sync: fib ===
Spike finished: insns=191, time=4230 ns, exit code = 0
sync controller: reads=58 writes=30 conflicts=0
DRAM[0..29]: 0 1 1 2 3 5 8 13 21 34 55 89 144 233 377 610 987 1597 2584 4181 6765 10946 17711 28657 46368 75025 121393 196418 317811 514229
```

`dram_test.elf` (одна запись/чтение слова) даёт `reads=1 writes=1` в обоих
режимах (`time=260 ns` async, `180 ns` sync). `exit code = 0` — успех; ненулевой
код или `instruction limit reached` означает ошибку (несовпадение/зацикливание).

`write_pair.elf` и `blocked_copy.elf` специально нагружают контроллер смесью
чтений и записей — они дают много конфликтов (`write_pair`: ~1000 на обоих
контроллерах, `blocked_copy`: async ~3888, sync ~2046).

### Как писать свои тесты на C++

Готовый C++-тест — это `tests/spike/<name>.cpp` с точкой входа `main`:

```cpp
using u64 = unsigned long long;

extern "C" int main()
{
    volatile u64 *dram = reinterpret_cast<volatile u64 *>(0x40000000UL);
    // ... читать/писать dram[i], вернуть 0 (успех) или !=0 (ошибка)
    return 0;
}
```

`tests/spike/build.sh <name>` соберёт `<name>.cpp` вместе с `crt0.S` (он
настраивает стек, вызывает `main` и пишет `(code<<1)|1` в `tohost`) и
`link.ld`. Компиляция — `clang++ --target=riscv64 -nostdlib -ffreestanding
-std=c++20`, поэтому **STL и libc недоступны**: используйте только встроенные
типы и указатели на MMIO. Если у `<name>` нет `.cpp`, но есть `.S`, скрипт
соберёт ассемблерный тест (так работает `dram_test`).

### Полезные опции

```sh
cmake -S . -B build -DSYSTEMC_ROOT=<prefix>       # другая SystemC
cmake -S . -B build -DSPIKE_ROOT=<prefix>          # другой riscv-isa-sim
```

Если `build.sh` запускается не из корня проекта, пути к `crt0.S`/`.S`/`.ld`
берутся относительно самого скрипта, а аргумент `spike_dram` — это путь к ELF.
