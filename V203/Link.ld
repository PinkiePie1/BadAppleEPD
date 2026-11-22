/* Executable code and hot tables stay in zero-wait Flash. The sequential
 * payload follows, using the remaining front space and the 160 KiB tail. */
ENTRY(_start)
MEMORY {
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = __flash_size
    RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 20K
}
__stack_size = 2048;
__flash_image_limit = __flash_size;
SECTIONS {
    .init : { KEEP(*(.init)) . = ALIGN(4); } > FLASH
    .vector : { KEEP(*(.vector)) . = ALIGN(64); } > FLASH
    .text : {
        *(.text .text.*) *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(4);
    } > FLASH
    .hot_video : {
        KEEP(*(.apple.glyphs .apple.classes .apple.run_map .apple.tree .apple.run))
        . = ALIGN(4);
    } > FLASH
    .data : {
        _data_vma = .;
        *(.data .data.*)
        . = ALIGN(8);
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        . = ALIGN(4); _edata = .;
    } > RAM AT > FLASH
    _data_lma = LOADADDR(.data);
    __fast_image_end = LOADADDR(.data) + SIZEOF(.data);
    .video_payload : {
        __payload_start = .;
        KEEP(*(.apple.payload))
        . = ALIGN(4); __payload_end = .;
    } > FLASH
    .bss (NOLOAD) : {
        . = ALIGN(4); _sbss = .;
        *(.sbss .sbss.* .bss .bss.*) *(COMMON)
        . = ALIGN(4); _ebss = .;
    } > RAM
    _end = _ebss;
    _heap_end = ORIGIN(RAM) + LENGTH(RAM) - __stack_size;
    .stack _heap_end (NOLOAD) : {
        _susrstack = .; . += __stack_size; _eusrstack = .;
    } > RAM
    ASSERT(_ebss <= _heap_end, "Static RAM overlaps 2 KiB reserved stack")
    ASSERT(__fast_image_end <= 64K, "Code or hot tables exceed zero-wait region")
    ASSERT(__payload_end <= __flash_size, "Complete image exceeds selected Flash size")
}
