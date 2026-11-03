ENTRY(ram_entry)
MEMORY { RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 2K }
SECTIONS {
 .text : { KEEP(*(.entry)) *(.text*) *(.rodata*) } > RAM
 /DISCARD/ : { *(.comment*) *(.ARM.attributes*) *(.ARM.exidx*) *(.ARM.extab*) }
 .forbidden : { *(.data*) *(.bss*) *(COMMON) } > RAM
 ASSERT(SIZEOF(.forbidden) == 0, "Loader must not require data/BSS initialization")
 ASSERT(SIZEOF(.text) <= 2048, "Loader exceeds reserved SRAM")
}
