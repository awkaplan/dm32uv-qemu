/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Program flash is mapped at 0x03000000; the board enters at 0x03001000. */
ENTRY(_start)
SECTIONS
{
    . = 0x03000000;
    .header : { KEEP(*(.header)) }
    . = 0x03001000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
}
