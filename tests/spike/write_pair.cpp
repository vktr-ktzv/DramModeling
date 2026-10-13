// Две записи подряд, затем чтение. Записи буферизуются и дренируются
// параллельно, поэтому, пока контроллер обслуживает чтение, в его очереди
// записи лежат сразу несколько транзакций.
// Возврат: 0 — успех, иначе — ошибка сверки.

using u64 = unsigned long long;

extern "C" int main()
{
    volatile u64* dram = reinterpret_cast<volatile u64*>(0x40000000UL);

    u64 acc = 0;
    for (unsigned i = 0; i < 2000; i += 2)
    {
        dram[i] = static_cast<u64>(i);
        dram[i + 1] = static_cast<u64>(i + 1);
        const u64 got = dram[i];
        if (got != static_cast<u64>(i))
        {
            return static_cast<int>(i) + 1;
        }
        acc += got;
    }
    return acc == 0 ? 1 : 0;
}
