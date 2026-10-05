# План: подключение DRAM-контроллера (SystemC/TLM) к Spike

## 0. Цель
Обращения Spike к выделенному DRAM-диапазону должны идти через
`DramControllerAsync`/`Sync` (`b_transport`), с таймингом и back-pressure.

## 1. Ключевой выбор: кто владеет планировщиком
- **Вариант A (рекомендуемый): SystemC — хост, Spike встроен как модуль.**
  `SC_THREAD` крутит `sim_t::step()`, а мост-устройство пробрасывает
  `load/store` в `b_transport`. Только так работает, потому что наш
  `b_transport` **блокирующий** (`wait(done)` в `dram_controller_base.cpp`),
  а `wait()` легален лишь в контексте SystemC-потока.
- Вариант B: два процесса (Spike и SystemC) + сокет с самодельным протоколом.
  Больше кода, выигрыша почти нет — только если нельзя линковать Spike в один
  бинарник.
- **Не подходит:** вызывать `b_transport` из главного цикла Spike — `wait()`
  вне SystemC-потока недопустим.

## 2. Предварительные шаги
- Зафиксировать версию `riscv-isa-sim` (API плавает) и сверить `sim.h`,
  `abstract_device.h`, `mmu.h`.
- Собрать Spike + `fesvr`; готовой `libspike` обычно нет — либо статическая
  либа из объектников, либо CMake `ExternalProject`.
- **ABI/стандарт:** SystemC-часть обязана быть C++26 (иначе не слинкуется
  `sc_api_version_3_0_2_cxx202400L`), Spike — C++17. Оставить bridge-TU на
  C++26 и линковать Spike, собранный тем же libstdc++ (clang по умолчанию им
  и пользуется). Проверить на минимальном примере.

## 3. Мост Spike → TLM
```cpp
class TlmDramDevice : public abstract_device_t
{
  public:
    TlmDramDevice(tlm_utils::simple_initiator_socket<...> &sock, uint64_t base);

    bool load(reg_t addr, size_t len, uint8_t *bytes) override
    {
        return xfer(tlm::TLM_READ_COMMAND, addr, bytes, len);
    }

    bool store(reg_t addr, size_t len, const uint8_t *bytes) override
    {
        return xfer(
            tlm::TLM_WRITE_COMMAND, addr, const_cast<uint8_t *>(bytes), len);
    }

  private:
    bool xfer(tlm::tlm_command cmd, reg_t addr, uint8_t *data, size_t len)
    {
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        // command/address(=addr-base)/data_ptr/data_length=len/streaming_width=len
        socket->b_transport(trans, delay); // блокируется, время уже продвинуто
        return trans.get_response_status() == tlm::TLM_OK_RESPONSE;
    }
};
```
Возврат `false` → Spike сгенерирует access fault. Владелец сокета — обёртка
из п.5; устройству отдаётся ссылка/колбэк (чтобы не тащить SystemC в spike-слой
без нужды).

## 4. Карта памяти и регистрация
- Выбрать `base` (напр. `0x8000_0000`) и размер; увеличить `DramStorage`
  (сейчас 1024 B) до реального объёма, адрес в `b_transport` — `addr - base`.
- Зарегистрировать: `sim.bus.add_device(base, dev)` (точное имя сверить) —
  физический путь `mmu_t` опрашивает устройства до RAM.
- Настроить `-m`/dtb так, чтобы диапазон не перекрывался RAM-апертурой Spike
  и был виден софту.
- Решить, лежит ли в DRAM только данные или ещё код: fetch инструкций пойдёт
  через контроллер (70 нс на инструкцию) — обычно нужен I-кэш/интерконнект
  перед памятью.

## 5. Драйвер co-simulation
- `class SpikeRunner : public sc_core::sc_module` с `SC_THREAD(run)`:
  - сконструировать `sim_t`, зарегистрировать устройство, загрузить ELF
    (**всё внутри потока**, тогда HTIF-load тоже пойдёт через DRAM и `wait()`
    сработает);
  - цикл `while (!done) ret = sim.step(1);` + опционально
    `wait(clock_period)` (п.6);
  - по завершении — `sc_stop()`.
- HTIF: отключить/перенаправить proxy-stdin (иначе поток встанет на вводе),
  обработать `tohost` exit.

## 6. Синхронизация времени
- Вариант 1: 1 инструкция = 1 такт — `wait(clock_period)` после каждого
  `step`, прогресс CPU привязан к тактовой сетке.
- Вариант 2: время двигают только обращения к памяти (через `b_transport`);
  быстро, но нефизично для CPU.
- Блокирующий `b_transport` даёт естественный stall CPU, но у нас **одна
  транзакция в полёте** — нет кэшей/outstanding. Для реализма добавить
  TLM-интерконнект/кэш перед контроллером.

## 7. Сборка
- Новый target `spike_dram`, `-DSPIKE_ROOT=...`: include spike/fesvr/softfloat,
  линковка `libspike`+`fesvr`, при желании `DRAM_ASAN=ON`.
- Либо собрать Spike через `add_library(spike STATIC ...)` из исходников, чтобы
  уйти от autoconf-сборки.

## 8. Верификация
- Bare-metal: записать паттерн в DRAM-диапазон и прочитать обратно, сверить.
- Активность контроллера должна отражаться в `read_count/write_count/conflicts`.
- Прогон под ASan.

## 9. Риски / открытые вопросы
- Дрейф API Spike (`abstract_device_t`/`bus_t`/`sim_t::step`) — прибить версию.
- Смешение стандартов C++17/26 и ABI libstdc++.
- HTIF/CLINT/PLIC и корректный dtb.
- Одна транзакция в полёте; дорогой fetch из DRAM.
- Нужно расширить `DramStorage` и, возможно, сделать `b_transport`
  неблокирующим для LT-режима.
