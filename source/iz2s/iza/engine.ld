ENTRY(iz_engine)
PHDRS {
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
}
SECTIONS {
    . = 0x48000000;
    .text : { PROVIDE_HIDDEN(__witchtube_text_start = .); KEEP(*(.text.iz_engine)) *(.text .text.* .gnu.linkonce.t.*) PROVIDE_HIDDEN(__witchtube_text_end = .); } :code
    .rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) } :code
    .got : { *(.got) } :code
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.* .data.rel.local.DW.ref.*) } :code
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :code
    .eh_frame : { PROVIDE_HIDDEN(__witchtube_eh_frame = .); KEEP(*(.eh_frame .eh_frame.*)) LONG(0); } :code
    . = ALIGN(4096) + 0x100000;
    .data : { *(.data .data.* .gnu.linkonce.d.*) } :data
    .got.plt : { *(.got.plt) } :data
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array)) PROVIDE_HIDDEN(__preinit_array_end = .);
    } :data
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data
    .tdata ALIGN(16) : {
        PROVIDE_HIDDEN(__tls_start = .);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        PROVIDE_HIDDEN(__tls_initialized = .);
    } :data :tls
    .tbss (NOLOAD) : { *(.tbss .tbss.* .gnu.linkonce.tb.*) PROVIDE_HIDDEN(__tls_end = .); } :data :tls
    .bss (NOLOAD) : { *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.note.gnu.property) }
}
ASSERT(iz_engine == ADDR(.text), "Engine entry is first")
ASSERT(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) <= 16, "Hosted TLS needs a larger prefix")
