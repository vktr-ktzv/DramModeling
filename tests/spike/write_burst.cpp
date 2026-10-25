// Фаза 1 — длинный поток записей в регион W: Spike выдаёт их не блокируясь,
// поэтому write-back буфер копит backlog, а контроллер дренирует его со своей
// скоростью.
// Фаза 2 — сразу начинается поток чтений из региона R, строки которого лежат
// в тех же банках, что и W, но в других строках.

using u64 = unsigned long long;

extern "C" int main()
{
    volatile u64* dram = reinterpret_cast<volatile u64*>(0x40000000UL);

    const unsigned n = 8192;
    volatile u64* w = dram;          // регион записей
    volatile u64* r = dram + 0x2000; // +64 КиБ: те же банки, другие строки

    for (unsigned i = 0; i < n; ++i)
    {
        w[i] = static_cast<u64>(i) * 7 + 1;
    }

    u64 acc = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        acc += r[i];
    }

    for (unsigned i = 0; i < n; ++i)
    {
        if (w[i] != static_cast<u64>(i) * 7 + 1)
        {
            return static_cast<int>(i) + 1;
        }
    }
    return acc == 0 ? 0 : 1;
}
