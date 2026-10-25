# Модели DRAM на SystemC / TLM-2.0 и co-simulation со Spike

Единственный таргет — `spike_dram`: co-simulation, внутрь SystemC (`sc_main`)
встроено RISC-V-ядро Spike; обращения гостевой программы к диапазону DRAM идут
через TLM-контроллер. Один и тот же ELF можно прогнать на двух моделях разного
уровня детализации (флаг `--async`/`--sync`):

- **async** — детальная модель: `DramChip` с банками, открытыми строками и
  row hit/miss, задержки в точных `sc_time`;
- **sync** — простая модель: фиксированное число тактов на чтение/запись, без
  состояния банков; данные хранит `SimpleMemory`.

## Структура проекта

```
.
├── CMakeLists.txt              # таргет spike_dram
├── README.md
├── ARCHITECTURE.md             # обзор архитектуры решения
├── AGENTS.md                   # заметки по архитектуре и сборке
├── .clang-format               # стиль (120 колонок, Allman)
│
├── include/
│   ├── dram_common.h           # DramStorage: массив байт + обработка TLM-payload
│   ├── dram_chip.h             # DramChip: банки, FSM ACTIVATE/READ/WRITE/PRECHARGE, трасса
│   ├── simple_memory.h         # SimpleMemory: массив байт без банков (для sync)
│   ├── dram_controller_base.h  # база: 2 tagged target-сокета, очереди, арбитраж
│   ├── dram_controller_async.h # асинхронный (детальный) контроллер
│   ├── dram_controller_sync.h  # синхронный (простой) контроллер, вход clk
│   ├── spike_dram_device.h     # SpikeTlmDevice (abstract_device_t) -> TLM
│   └── store_buffer.h          # write-back буфер: много транзакций в полёте
│
├── src/
│   ├── dram_chip.cpp
│   ├── simple_memory.cpp
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
    └── write_burst.cpp         # бенчмарк для сравнения моделей
```

### Роли модулей

- `DramChip` (`dram_chip.h`) — детальная модель микросхемы DRAM (используется
  только в async): банки, открытые строки, конечный автомат
  `ACTIVATE → READ/WRITE → PRECHARGE`, тайминги `tRCD/tCAS/tRP/tWR`, а также
  полная трасса обращений. Принимает запросы по `simple_target_socket`,
  применяет данные и **возвращает задержку** через `b_transport` (сама время не
  двигает).
- `SimpleMemory` (`simple_memory.h`) — простая модель памяти для sync: только
  массив байт, без банков и row hit/miss. `b_transport` применяет данные и не
  добавляет задержку; трассу обращений без банковских команд тоже умеет писать.
- `DramControllerBase` — общая часть: `kPorts = 2` tagged target-сокета (порт 0 —
  чтения, порт 1 — записи), очереди чтения/записи, выбор направления round-robin
  с учётом `turnaround()`, счётчики `reads`/`writes`/`conflicts`. Данные не хранит:
  отдаёт запрос в модель памяти через `mem_socket`.
- `DramControllerAsync` — `serve()` берёт задержку из `DramChip` (точные `sc_time`),
  сам добавляет только `switch_penalty` (20 нс) при смене направления.
- `DramControllerSync` — простая модель: `serve()` синхронизируется по
  `clk.posedge_event()`, а задержка — фиксированное число тактов на чтение или
  запись (`read_latency_cycles`/`write_latency_cycles`), без анализа банков.
- `SpikeTlmDevice` — `abstract_device_t` из Spike: его `load/store` вызывают
  `b_transport` буфера. Адрес приходит **уже смещённым** (Spike `bus_t`
  вычитает базу устройства), поэтому второй раз базу вычитать нельзя.
- `CosimSim` (в `spike_dram_main.cpp`) — наследник `sim_t`, владеет временем.
- `SpikeRunner` — `SC_THREAD`, внутри которого целиком крутится Spike.
- Тип контроллера выбирается в `sc_main`: `--sync` создаёт `DramControllerSync`
  с `sc_clock` (период 10 нс), подключает `ctrl.clk(clk)` и `SimpleMemory`;
  `--async` (по умолчанию) создаёт `DramControllerAsync` и `DramChip`. Оба
  контроллера лежат в `std::unique_ptr<DramControllerBase>`, поэтому общая
  статистика (число запросов и конфликтов) одинакова.

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
- sync: каждую итерацию `wait(clk.posedge_event())`, затем фиксированная
  задержка `cycles(read_latency_cycles)` или `cycles(write_latency_cycles)`;
  `SimpleMemory::b_transport` только применяет данные и время не двигает.

К моменту возврата из `b_transport` планировщик уже продвинул время, поэтому
возвращаемая `delay` выставляется в `SC_ZERO_TIME`. Только поэтому
`b_transport` можно вызывать из `SC_THREAD`, но не из собственного цикла Spike.

### Модель банков `DramChip` (async)

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

Модель последовательная: запросы обслуживаются по одному, банковский
параллелизм не моделируется; refresh и burst-доступы тоже не учитываются. Это
упрощение из лекции 6.

Каждая команда, её тип доступа (`empty`/`hit`/`miss`), банк/строка/столбец и
вклад в задержку пишутся в трассу. Сводка (активации, precharge, hit/miss,
средняя latency) печатается в stdout, полная трасса выгружается в CSV.

### Простая модель sync: `SimpleMemory` + фиксированные такты

В sync-режиме банков нет: `DramControllerSync` выдаёт команду по фронту `clk`,
применяет данные через `SimpleMemory` и ждёт фиксированное число тактов
(`kSyncReadCycles`/`kSyncWriteCycles` в `sc_main`, по умолчанию 3 и 2). Поэтому
у sync нет row hit/miss, активаций и precharge — это осознанно более грубая
модель. Трасса sync содержит только сами обращения:

```
time_ns,command,type,addr,len
50,WR,write,0,8
70,WR,write,8,8
120,RD,read,8,8
```

Сравнивать её с банковой трассой async «строка в строку» нельзя: это модели
разного уровня детализации. Общими остаются число запросов, конфликты арбитража
и итоговое время симуляции. Sync не обязательно быстрее по wall-clock: контроллер
просыпается на каждом фронте `clk`, а работа банкового автомата async на одну
транзакцию мала по сравнению со стоимостью шага Spike и переключений SystemC.

### Сравнение моделей на `write_burst`

`tests/spike/write_burst.cpp` — бенчмарк с длинной фазой записей и последующим
чтением тех же банков, но других строк. В async видно банковскую динамику
(активации, precharge, row hit/miss), в sync — только фиксированные чтения и
записи. При этом число и порядок запросов, которые доходят до контроллера,
зависят от скорости обслуживания: на async backlog записей «протекает» в фазу
чтений иначе, чем на sync, поэтому может расходиться и число конфликтов
(например, на `blocked_copy`: 3070 против 2046).

Трасса async (`DramChip`):

```
time_ns,command,row_access,type,bank,row,col,latency_ns
40,ACT,empty,write,0,0,0,12
52,WR,empty,write,0,0,0,34
86,WR,hit,write,0,0,8,34
140,RD,hit,read,0,0,8,22
...
```

Трасса sync (`SimpleMemory`):

```
time_ns,command,type,addr,len
50,WR,write,0,8
70,WR,write,8,8
120,RD,read,8,8
...
```

Обе пишутся в `dram_trace_<mode>.csv` (или в файл из `--trace <file>`).

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
доступа к памяти:

```
T = N_insn * cycle + сумма(access_latency_i) + N_turnaround * switch_penalty
```

В async `access_latency_i` — результат банкового автомата (зависит от row
hit/miss), в sync — фиксированные `read_latency_cycles`/`write_latency_cycles`
умноженные на период такта.

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
- Для тестов: `clang`/`clang++` с RISC-V-таргетом (из `PATH`) и
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

Ожидаемый результат `fib.elf` — данные совпадают, модели дают разный тайминг и
разную статистику:

```
=== async: fib ===
Spike finished: insns=250, time=4520 ns, exit code = 0
async controller: reads=58 writes=30 conflicts=0
async dram: reads=58 writes=30 activations=4 precharges=0 row_hits=84 row_misses=0 row_empties=4 avg_latency=26.6364 ns
trace -> dram_trace_async.csv (92 commands)

=== sync: fib ===
Spike finished: insns=250, time=4820 ns, exit code = 0
sync controller: reads=58 writes=30 conflicts=0
sync memory: reads=58 writes=30 read_latency=30 ns write_latency=20 ns avg_latency=26.5909 ns
trace -> dram_trace_sync.csv (88 accesses)
```

На `fib.elf` async показывает банковскую динамику (4 активации, row hits), а
sync — только фиксированные задержки. `write_burst` сильнее нагружает контроллер:

```
=== async: write_burst ===
async dram: ... activations=3072 precharges=3068 row_hits=21504 row_misses=3068 ... avg_latency=28.998 ns
=== sync: write_burst ===
sync memory: ... read_latency=30 ns write_latency=20 ns avg_latency=26.6667 ns
```

`dram_test.elf` (одна запись/чтение слова) даёт `reads=1 writes=1` в обоих
режимах. `exit code = 0` — успех; ненулевой код или `instruction limit reached`
означает ошибку (несовпадение/зацикливание).

`write_pair.elf` и `blocked_copy.elf` специально нагружают контроллер смесью
чтений и записей — они дают много конфликтов. Для async на них видна и
банковская статистика (row hit/miss, активации), которой у простой sync-модели
принципиально нет.

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
`dram_trace_async.csv` / `dram_trace_sync.csv` в текущем каталоге). Формат
зависит от модели: async пишет банковские команды (`ACT/PRE/RD/WR`), sync —
только обращения (`RD/WR`).

Если `build.sh` запускается не из корня проекта, пути к `crt0.S`/`.S`/`.ld`
берутся относительно самого скрипта, а аргумент `spike_dram` — это путь к ELF.
