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
│   ├── dram_chip.h             # DramChip: банки, FSM ACTIVATE/READ/WRITE/PRECHARGE, трасса
│   ├── dram_controller_base.h  # база: 2 tagged target-сокета, очереди, арбитраж
│   ├── dram_controller_async.h # асинхронный контроллер (точные sc_time)
│   ├── dram_controller_sync.h  # синхронный контроллер, вход clk
│   ├── spike_dram_device.h     # SpikeTlmDevice (abstract_device_t) -> TLM
│   └── store_buffer.h          # write-back буфер: много транзакций в полёте
│
├── src/
│   ├── dram_chip.cpp
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
    ├── blocked_copy.cpp        # блочный memcpy: много конфликтов чтение/запись
    └── write_burst.cpp         # бенчмарк с сильным расхождением трасс sync/async
```

### Роли модулей

- `DramChip` (`dram_chip.h`) — модель микросхемы DRAM: банки, открытые строки,
  конечный автомат `ACTIVATE → READ/WRITE → PRECHARGE`, тайминги
  `tRCD/tCAS/tRP/tWR`, а также полная трасса обращений. Принимает запросы по
  `simple_target_socket`, применяет данные и **возвращает задержку** через
  `b_transport` (сама время не двигает).
- `DramControllerBase` — общая часть: `kPorts = 2` tagged target-сокета (порт 0 —
  чтения, порт 1 — записи), очереди чтения/записи, выбор направления round-robin
  с учётом `turnaround()`, счётчики `reads`/`writes`/`conflicts`. Данные не хранит:
  отдаёт запрос в `DramChip` через `mem_socket`.
- `DramControllerAsync` — `serve()` берёт задержку из `DramChip` (точные `sc_time`),
  сам добавляет только `switch_penalty` (20 нс) при смене направления.
- `DramControllerSync` — `serve()` синхронизируется по `clk.posedge_event()`,
  штраф смены направления — в тактах; `DramChip` для sync настраивается с
  `quantum = clock_period`, поэтому её задержки уже кратны такту.
- `SpikeTlmDevice` — `abstract_device_t` из Spike: его `load/store` вызывают
  `b_transport` буфера. Адрес приходит **уже смещённым** (Spike `bus_t`
  вычитает базу устройства), поэтому второй раз базу вычитать нельзя.
- `CosimSim` (в `spike_dram_main.cpp`) — наследник `sim_t`, владеет временем.
- `SpikeRunner` — `SC_THREAD`, внутри которого целиком крутится Spike.
- Тип контроллера выбирается в `sc_main`: `--sync` создаёт `DramControllerSync`
  с `sc_clock` (период 10 нс) и подключает `ctrl.clk(clk)`; `--async`
  (по умолчанию) создаёт `DramControllerAsync`. Оба лежат в
  `std::unique_ptr<DramControllerBase>`, поэтому статистика одинакова.

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

- async: `wait(switch_penalty)` при смене направления, затем `issue()` — вызов
  `DramChip::b_transport`, возвращающий задержку доступа (точные нс), и
  `wait(access)`;
- sync: каждую итерацию `wait(clk.posedge_event())`, штраф смены направления
  кратен периоду (`cycles(n) = clock_period_ * n`), а задержка из `DramChip` уже
  квантована по такту.

К моменту возврата из `b_transport` планировщик уже продвинул время, поэтому
возвращаемая `delay` выставляется в `SC_ZERO_TIME`. Только поэтому
`b_transport` можно вызывать из `SC_THREAD`, но не из собственного цикла Spike.

### Модель банков `DramChip`

Адрес раскладывается на `[столбец | банк | строка]` (по умолчанию 6 бит столбца,
2 бита банка). У каждого банка хранится `state` (`Idle`/`Active`) и `open_row`.
Автомат (лекция 6):

```
Idle --ACTIVATE--> Active --READ/WRITE--> Active --PRECHARGE--> Idle
```

- банк `Idle` — нужна активация строки (`+tRCD`);
- банк `Active`, та же строка — row hit (только `tCAS(+tWR)`);
- банк `Active`, другая строка — row miss: `PRECHARGE (+tRP)`, затем
  `ACTIVATE (+tRCD)`, затем колоночная команда.

Каждая команда, её тип доступа (`empty`/`hit`/`miss`), банк/строка/столбец и
вклад в задержку пишутся в трассу. `DramTiming.quantum` (период такта для sync)
округляет каждую задержку вверх до кратного; это и есть вся разница между async
и sync. Сводка (активации, precharge, hit/miss, средняя latency) печатается в
stdout, полная трасса выгружается в CSV.

### Расхождение трасс sync/async

`tests/spike/write_burst.cpp` — бенчмарк, на котором трассы расходятся сильно:
фаза длинных записей копится в write-back буфере, а затем идёт чтение из тех же
банков, но других строк. Сколько записей «протечёт» в фазу чтений, зависит от
скорости обслуживания, поэтому sync и async дают разное число активаций и row
miss. Трасса каждой модели пишется в `dram_trace_<mode>.csv` (или в файл из
`--trace <file>`):

```
time_ns,command,row_access,type,bank,row,col,latency_ns
70,ACT,empty,write,0,0,0,12
82,WR,empty,write,0,0,0,34
116,WR,hit,write,0,0,8,34
...
```

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
ld/st → mmu_t → bus_t → SpikeTlmDevice::load/store → StoreBuffer::load/store
      → b_transport → очередь → wait(done) → serve() двигает время → notify
```

Инструкция при этом «висит» на `wait(done)`, затем `idle()` всё равно добавляет
свой такт. Итоговое время складывается из тактов на инструкции и задержек
доступа к DRAM (их теперь считает банковый автомат, поэтому они зависят от
row hit/miss и потому индивидуальны):

```
T = N_insn * cycle + сумма(access_latency_i) + N_turnaround * switch_penalty
```

Для sync каждая `access_latency_i` ещё округляется вверх до такта
(`quantum = clock_period`), для async берётся точное значение.

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
Spike finished: insns=191, time=4544 ns, exit code = 0
async controller: reads=58 writes=30 conflicts=1
async dram: reads=58 writes=30 activations=4 precharges=0 row_hits=84 row_misses=0 row_empties=4 avg_latency=26.64 ns
trace -> dram_trace_async.csv (92 commands)
DRAM[0..29]: 0 1 1 2 3 5 8 13 21 34 55 89 144 233 377 610 987 1597 2584 4181 6765 10946 17711 28657 46368 75025 121393 196418 317811 514229

=== sync: fib ===
Spike finished: insns=191, time=5220 ns, exit code = 0
sync controller: reads=58 writes=30 conflicts=1
sync dram: reads=58 writes=30 activations=4 precharges=0 row_hits=84 row_misses=0 row_empties=4 avg_latency=37.73 ns
trace -> dram_trace_sync.csv (92 commands)
DRAM[0..29]: 0 1 1 2 3 5 8 13 21 34 55 89 144 233 377 610 987 1597 2584 4181 6765 10946 17711 28657 46368 75025 121393 196418 317811 514229
```

На маленьком `fib.elf` расходится только тайминг (одна и та же
последовательность команд). Чтобы увидеть расхождение **трасс**, гоняйте
`write_burst`:

```
=== async: write_burst ===
async dram: ... activations=3072 precharges=3068 row_hits=21504 row_misses=3068 ... avg_latency=29.0 ns
=== sync: write_burst ===
sync dram: ... activations=3527 precharges=3523 row_hits=21049 row_misses=3523 ... avg_latency=42.4 ns
```

`dram_test.elf` (одна запись/чтение слова) даёт `reads=1 writes=1` в обоих
режимах. `exit code = 0` — успех; ненулевой код или `instruction limit reached`
означает ошибку (несовпадение/зацикливание).

`write_pair.elf` и `blocked_copy.elf` специально нагружают контроллер смесью
чтений и записей — они дают много конфликтов и row miss на обоих контроллерах.
`write_burst.elf` — бенчмарк для сравнения трасс: на нём sync и async дают
разное число активаций/precharge и разный row hit rate.

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

`--trace <file>` задаёт путь CSV-трассы для текущего запуска (по умолчанию
`dram_trace_async.csv` / `dram_trace_sync.csv` в текущем каталоге).

Если `build.sh` запускается не из корня проекта, пути к `crt0.S`/`.S`/`.ld`
берутся относительно самого скрипта, а аргумент `spike_dram` — это путь к ELF.
