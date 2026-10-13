# Общий crt0 для C++-тестов под Spike: стек, вызов main(), выход через HTIF.
# main() возвращает код: 0 — успех, иначе — ошибка.

    .section .text.init,"ax",@progbits
    .globl _start
_start:
    li   sp, 0x80008000          # стек в ОЗУ Spike
    call main                    # int main()
    slli a0, a0, 1               # (code << 1) | 1 для HTIF tohost
    ori  a0, a0, 1
    la   t0, tohost
    sd   a0, 0(t0)
1:
    j    1b

    .section .tohost,"aw",@progbits
    .align 3
    .globl tohost
tohost:
    .dword 0
    .align 3
    .globl fromhost
fromhost:
    .dword 0
