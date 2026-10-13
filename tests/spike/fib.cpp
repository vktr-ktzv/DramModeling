// Числа Фибоначчи через DRAM-контроллер (MMIO-диапазон 0x4000_0000).
// Считает F(0..N-1), хранит их в DRAM, перечитывает и сверяет.
// Возврат: 0 — успех, иначе — ошибка.

using u64 = unsigned long long;

extern "C" int main()
{
    volatile u64* dram = reinterpret_cast<volatile u64*>(0x40000000UL);

    const unsigned n = 30;
    dram[0] = 0;
    dram[1] = 1;
    for (unsigned i = 2; i < n; ++i)
    {
        dram[i] = dram[i - 1] + dram[i - 2];
    }

    if (dram[10] != 55)
    {
        return 1;
    }
    if (dram[19] != 4181)
    {
        return 1;
    }
    return 0;
}
