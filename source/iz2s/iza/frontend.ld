ENTRY(iz_app)
PHDRS { code PT_LOAD FLAGS(5); }
SECTIONS {
    . = 0;
    .text : { KEEP(*(.text.iz_app)) *(.text*) *(.rodata*) . = ALIGN(4); } :code
    .data : { *(.data*) *(.bss*) *(COMMON) *(.got*) *(.tdata*) *(.tbss*) }
    /DISCARD/ : { *(.comment*) *(.note*) *(.eh_frame*) }
}
ASSERT(iz_app == ADDR(.text), "IZ2S calls the first instruction")
ASSERT(SIZEOF(.data) == 0, "The frontend stores its state in IzNativeApp.data")
