OUTPUT_FORMAT("elf64-littleriscv")
/*
 * LK linker script for single segment binaries.
 */
PHDRS
{
    code PT_LOAD FLAGS(5);   /* PF_R|PF_X */
    rodata PT_LOAD FLAGS(4); /* PF_R */
    data PT_LOAD FLAGS(6);   /* PF_R|PF_W */
}

ENTRY(_start)
SECTIONS
{
    . = 0x200000; /* 2MB offset */

    /* text/read-only data */
    .text : {
        *(.text .text*)
        *(.gnu.linkonce.t.*)
    } :code

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    } :rodata

    /* trick to force any extra sections to be emitted here */
    . = .;

    __rodata_end = .;
    __rom_end = . ;
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_start_rom = .;

    .data : {
        __data_start = .;
        *(.data .data.* .gnu.linkonce.d.*)
        __ctor_list = .;
        KEEP(*(.ctors .init_array))
        __ctor_end = .;
        __dtor_list = .;
        KEEP(*(.dtors .fini_array))
        __dtor_end = .;
        *(.got*)
        *(.dynamic)
    } :data

    /* Try to put sdata and sbss near each other by putting sdata at the end of the data segment
     * and sbss at the start of the bss segment. This maximizes reach of things referenced off of
     * the global pointer. */
    .sdata : {
        __global_pointer$ = . + (4K / 2);
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2) *(.srodata .srodata.*)
        *(.sdata .sdata.* .gnu.linkonce.s.*)
    }

    . = ALIGN(64 / 8);
    __data_end = .;
    __bss_start = .;

    .sbss : {
        *(.dynsbss)
        *(.sbss .sbss.* .gnu.linkonce.sb.*)
        *(.scommon)
    }

    /* uninitialized data (in same segment as writable data) */
    .bss : {
        /* regular bss */
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
    }

    . = ALIGN(64 / 8);
    __bss_end = .;

    /* Align the end to ensure anything after the kernel ends up on its own pages */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    _end = .;

    /* Strip unnecessary stuff */
    /DISCARD/ : { *(.comment .note .eh_frame) }
}

