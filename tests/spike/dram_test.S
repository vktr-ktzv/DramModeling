# Bare-metal тест: доступ к MMIO-диапазону DRAM-контроллера (0x4000_0000).
# Успех: записали/прочитали одно слово, затем tohost = 1 (exit code 0).

    .section .text.init,"ax",@progbits
    .globl _start
_start:
    li   sp, 0x80008000          # стек в ОЗУ Spike
    li   t0, 0x40000000          # база DRAM-контроллера
    li   t1, 0x0eadbeef
    sw   t1, 0(t0)               # write -> write queue
    lw   t2, 0(t0)               # read  -> read queue
    bne  t1, t2, fail

    li   t0, 1                   # (code << 1) | 1, code = 0
    la   t1, tohost
    sd   t0, 0(t1)
1:
    j    1b

fail:
    li   t0, 3                   # exit code 1
    la   t1, tohost
    sd   t0, 0(t1)
2:
    j    2b

    .section .tohost,"aw",@progbits
    .align 3
    .globl tohost
tohost:
    .dword 0
    .align 3
    .globl fromhost
fromhost:
    .dword 0
