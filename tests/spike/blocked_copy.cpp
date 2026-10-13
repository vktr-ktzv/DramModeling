// Классический блочный memcpy: сначала пачкой читаются 4 слова, затем пачкой
// пишутся. Пока идёт серия записей, следующая серия чтений уже обращается к
// контроллеру — так в полёте оказывается несколько транзакций, а арбитр
// разрешает конфликты чтение/запись.
// Возврат: 0 — успех, иначе — индекс первой несовпавшей ячейки + 1.

using u64 = unsigned long long;

extern "C" int main()
{
    volatile u64* dram = reinterpret_cast<volatile u64*>(0x40000000UL);

    const unsigned n = 4096;
    volatile u64* src = dram;
    volatile u64* dst = dram + n;

    for (unsigned i = 0; i < n; ++i)
    {
        src[i] = static_cast<u64>(i) * 2654435761ULL + 12345ULL;
    }

    for (unsigned i = 0; i < n; i += 4)
    {
        const u64 a0 = src[i + 0];
        const u64 a1 = src[i + 1];
        const u64 a2 = src[i + 2];
        const u64 a3 = src[i + 3];
        dst[i + 0] = a0;
        dst[i + 1] = a1;
        dst[i + 2] = a2;
        dst[i + 3] = a3;
    }

    for (unsigned i = 0; i < n; ++i)
    {
        if (dst[i] != static_cast<u64>(i) * 2654435761ULL + 12345ULL)
        {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}
