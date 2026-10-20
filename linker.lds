OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS(5);
    rodata  PT_LOAD    FLAGS(4);
    data    PT_LOAD    FLAGS(6);
}

SECTIONS
{
    . = 0xffffffff80000000;

    __kernel_start = .;

    .text : {
        __kernel_text_start = .;
        *(.text .text.*)
        *(.ltext .ltext.*)
        __kernel_text_end = .;
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
        *(.lrodata .lrodata.*)
    } :rodata

    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
        *(.ldata .ldata.*)
        *(.requests)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
        *(.lbss .lbss.*)
    } :data

    . = ALIGN(0x1000);
    __kernel_end = .;
}
