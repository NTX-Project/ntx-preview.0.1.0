ENTRY(AsterKernelEntry)

SECTIONS
{
  . = __NK_LINK_BASE__;

  .text : ALIGN(16)
  {
    KEEP(*(.text.start))
    *(.text .text.*)
  }

  .rodata : ALIGN(16)
  {
    *(.rodata .rodata.*)
  }

  .data : ALIGN(16)
  {
    *(.data .data.*)
  }

  .bss : ALIGN(16)
  {
    *(.bss .bss.*)
    *(COMMON)
  }
}
