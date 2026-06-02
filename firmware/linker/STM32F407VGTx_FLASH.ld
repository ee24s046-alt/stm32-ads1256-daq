/* ---------------------------------------------------------------------------
 * STM32F407VGT6 - 1 MB flash / 128 KB SRAM1+SRAM2 / 64 KB CCM
 *
 * Two deliberate choices in here:
 *
 *   1. The last flash sector (sector 11, 0x080E0000, 128 KB) is carved out as
 *      a separate MEMORY region named CALSTORE and is *not* part of FLASH.
 *      The linker therefore physically cannot place code where the runtime
 *      calibration writer erases.  See docs/calibration.md.
 *
 *   2. CCMRAM (64 KB, core-coupled, zero-wait-state, not DMA-reachable) is
 *      exposed as its own region.  The two sample ring buffers live there via
 *      __attribute__((section(".ccmram"))) so ring traffic never contends with
 *      the USB peripheral's AHB accesses to SRAM1.
 *
 * There is no heap.  _end / __heap_start are defined only so that newlib-nano
 * links; _sbrk is not provided and any call to malloc() fails at link time.
 * ------------------------------------------------------------------------- */

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* top of SRAM1+2, MSP init value */

_Min_Stack_Size = 0x800;               /* 2 KB MSP: ISRs + pre-scheduler init */

MEMORY
{
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 896K   /* sectors 0..10      */
  CALSTORE (r)   : ORIGIN = 0x080E0000, LENGTH = 128K   /* sector 11          */
  RAM      (xrw) : ORIGIN = 0x20000000, LENGTH = 128K   /* SRAM1 + SRAM2      */
  CCMRAM   (xrw) : ORIGIN = 0x10000000, LENGTH = 64K    /* core-coupled       */
}

/* Exported so flash_store.c can assert at compile time that it is erasing the
   sector the linker actually reserved, rather than a hard-coded address that
   drifted. */
__calstore_start = ORIGIN(CALSTORE);
__calstore_size  = LENGTH(CALSTORE);

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH

  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .);
                     KEEP(*(.preinit_array*))
                     PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
  .init_array    : { PROVIDE_HIDDEN(__init_array_start = .);
                     KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*))
                     PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
  .fini_array    : { PROVIDE_HIDDEN(__fini_array_start = .);
                     KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*))
                     PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT >FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* CCM: ring buffer storage.  Zeroed by the startup code alongside .bss. */
  .ccmram (NOLOAD) :
  {
    . = ALIGN(4);
    _sccmbss = .;
    *(.ccmram)
    *(.ccmram*)
    . = ALIGN(4);
    _eccmbss = .;
  } >CCMRAM

  ._user_stack (NOLOAD) :
  {
    . = ALIGN(8);
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* Deliberately no heap region.  newlib-nano wants these symbols; give it
     zero bytes so any accidental malloc() fails loudly instead of quietly
     eating the stack. */
  PROVIDE(end     = _ebss);
  PROVIDE(_end    = _ebss);

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

ASSERT(_ebss + _Min_Stack_Size <= ORIGIN(RAM) + LENGTH(RAM),
       "ERROR: .bss + reserved MSP stack overflows SRAM")
