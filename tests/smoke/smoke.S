/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * dm32uv-qemu smoke test (CK803, ABIv2), board -M dm32.
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * A raw program-flash image: the DH_FLAG header and entry word at
 * 0x03000000, code at the entry 0x03001000. Checks, each reporting FAIL on
 * mismatch:
 *   - integer loop: sum of 1..100 == 5050
 *   - SRAM store/load round-trip at 0x18000000
 *   - ADC_CTRL_VERID (0x140d0040) reads 0x20151216
 *   - DAC_CTRL_VERID (0x140f0014) reads its reset value 0x20160229
 *   - WDG_LOCK (0x14010000) reads 1, locked, at reset
 * Then prints one line on UART0 (THR at 0x14030000) and halts.
 */

    .section .header, "a"
    .long   0x0c646874              /* DH_FLAG, "thd\x0c" */
    .long   _start                  /* entry, 0x03001000 */

    .text
    .align  2
    .global _start
_start:
    /* sum 1..100 into r4 */
    movi    r4, 0
    movi    r5, 100
sum_loop:
    addu    r4, r4, r5
    subi    r5, r5, 1
    bnez    r5, sum_loop
    lrw     r8, 5050
    cmpne   r4, r8
    bt      fail

    /* store r4 to SRAM, reload into r10, compare */
    lrw     r9, 0x18000000
    st.w    r4, (r9, 0)
    movi    r10, 0
    ld.w    r10, (r9, 0)
    cmpne   r10, r4
    bt      fail

    /* ADC controller version register */
    lrw     r9, 0x140d0040
    ld.w    r10, (r9, 0)
    lrw     r8, 0x20151216
    cmpne   r10, r8
    bt      fail

    /* DAC controller version register, reset value */
    lrw     r9, 0x140f0014
    ld.w    r10, (r9, 0)
    lrw     r8, 0x20160229
    cmpne   r10, r8
    bt      fail

    /* watchdog lock register: 1 (locked) at reset */
    lrw     r9, 0x14010000
    ld.w    r10, (r9, 0)
    cmpnei  r10, 1
    bt      fail

    lrw     r3, msg_pass
    br      print
fail:
    lrw     r3, msg_fail
print:
    /* write the NUL-terminated string at r3 to the UART0 THR */
    lrw     r11, 0x14030000
1:
    ld.b    r13, (r3, 0)
    bez     r13, halt
    st.w    r13, (r11, 0)
    addi    r3, r3, 1
    br      1b
halt:
    br      halt

    .section .rodata
msg_pass:
    .asciz  "dm32uv-qemu smoke test: PASS\n"
msg_fail:
    .asciz  "dm32uv-qemu smoke test: FAIL\n"
