/**
 * startup_stm32f407xx.s - reset handler and vector table.
 *
 * Hand-written rather than generated.  Two things differ from the usual
 * vendor template:
 *
 *   - .ccmram is zeroed alongside .bss.  The sample ring buffers live in CCM
 *     and the lock-free algorithm assumes indices start at zero.
 *   - MemManage / BusFault / UsageFault get their own vectors rather than
 *     being aliased onto HardFault, so fault_handler.c can report which one
 *     actually fired instead of only ever seeing "forced hard fault".
 */
    .syntax unified
    .cpu cortex-m4
    .fpu fpv4-sp-d16
    .thumb

.global g_pfnVectors
.global Default_Handler

.word _sidata
.word _sdata
.word _edata
.word _sbss
.word _ebss

    .section .text.Reset_Handler
    .weak Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    ldr   sp, =_estack

    /* Enable the FPU (CP10/CP11 full access) before any code that might use
       it.  FreeRTOS's CM4F port assumes the FPU is on and lazy stacking is
       available; port.c enables it too, but C init code runs first. */
    ldr   r0, =0xE000ED88
    ldr   r1, [r0]
    orr   r1, r1, #(0xF << 20)
    str   r1, [r0]
    dsb
    isb

    /* Copy .data from flash to SRAM */
    ldr   r0, =_sdata
    ldr   r1, =_edata
    ldr   r2, =_sidata
    movs  r3, #0
    b     LoopCopyDataInit
CopyDataInit:
    ldr   r4, [r2, r3]
    str   r4, [r0, r3]
    adds  r3, r3, #4
LoopCopyDataInit:
    adds  r4, r0, r3
    cmp   r4, r1
    bcc   CopyDataInit

    /* Zero .bss */
    ldr   r2, =_sbss
    ldr   r4, =_ebss
    movs  r3, #0
    b     LoopFillZerobss
FillZerobss:
    str   r3, [r2]
    adds  r2, r2, #4
LoopFillZerobss:
    cmp   r2, r4
    bcc   FillZerobss

    /* Zero .ccmram (ring buffer storage) */
    ldr   r2, =_sccmbss
    ldr   r4, =_eccmbss
    movs  r3, #0
    b     LoopFillZeroCcm
FillZeroCcm:
    str   r3, [r2]
    adds  r2, r2, #4
LoopFillZeroCcm:
    cmp   r2, r4
    bcc   FillZeroCcm

    bl    SystemInit
    bl    main
    b     .
.size Reset_Handler, .-Reset_Handler

    .section .text.Default_Handler,"ax",%progbits
Default_Handler:
    /* An unexpected interrupt is a configuration bug.  Trap here so a debugger
       shows the IPSR of the offending vector instead of silently returning. */
    bkpt  #0
InfiniteLoop:
    b     InfiniteLoop
.size Default_Handler, .-Default_Handler

/* ------------------------------------------------------------------------ */
    .section .isr_vector,"a",%progbits
    .type g_pfnVectors, %object
g_pfnVectors:
    .word _estack
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler

    /* External interrupts 0..81.  Only the ones this firmware uses are named;
       everything else falls through to Default_Handler via .weak aliasing. */
    .word WWDG_IRQHandler                 /* 0  */
    .word PVD_IRQHandler
    .word TAMP_STAMP_IRQHandler
    .word RTC_WKUP_IRQHandler
    .word FLASH_IRQHandler
    .word RCC_IRQHandler
    .word EXTI0_IRQHandler                /* 6  - unused                    */
    .word EXTI1_IRQHandler                /* 7  - unused                    */
    .word EXTI2_IRQHandler                /* 8  - unused                    */
    .word EXTI3_IRQHandler                /* 9  - ADS1256 /DRDY (PC3)       */
    .word EXTI4_IRQHandler                /* 10 - unused                    */
    .word DMA1_Stream0_IRQHandler
    .word DMA1_Stream1_IRQHandler
    .word DMA1_Stream2_IRQHandler
    .word DMA1_Stream3_IRQHandler
    .word DMA1_Stream4_IRQHandler
    .word DMA1_Stream5_IRQHandler
    .word DMA1_Stream6_IRQHandler
    .word ADC_IRQHandler
    .word CAN1_TX_IRQHandler
    .word CAN1_RX0_IRQHandler
    .word CAN1_RX1_IRQHandler
    .word CAN1_SCE_IRQHandler
    .word EXTI9_5_IRQHandler
    .word TIM1_BRK_TIM9_IRQHandler
    .word TIM1_UP_TIM10_IRQHandler
    .word TIM1_TRG_COM_TIM11_IRQHandler
    .word TIM1_CC_IRQHandler
    .word TIM2_IRQHandler
    .word TIM3_IRQHandler
    .word TIM4_IRQHandler
    .word I2C1_EV_IRQHandler
    .word I2C1_ER_IRQHandler
    .word I2C2_EV_IRQHandler
    .word I2C2_ER_IRQHandler
    .word SPI1_IRQHandler                 /* 35 */
    .word SPI2_IRQHandler
    .word USART1_IRQHandler
    .word USART2_IRQHandler
    .word USART3_IRQHandler
    .word EXTI15_10_IRQHandler
    .word RTC_Alarm_IRQHandler
    .word OTG_FS_WKUP_IRQHandler
    .word TIM8_BRK_TIM12_IRQHandler
    .word TIM8_UP_TIM13_IRQHandler
    .word TIM8_TRG_COM_TIM14_IRQHandler
    .word TIM8_CC_IRQHandler
    .word DMA1_Stream7_IRQHandler
    .word FSMC_IRQHandler
    .word SDIO_IRQHandler
    .word TIM5_IRQHandler
    .word SPI3_IRQHandler
    .word UART4_IRQHandler
    .word UART5_IRQHandler
    .word TIM6_DAC_IRQHandler
    .word TIM7_IRQHandler
    .word DMA2_Stream0_IRQHandler
    .word DMA2_Stream1_IRQHandler
    .word DMA2_Stream2_IRQHandler
    .word DMA2_Stream3_IRQHandler
    .word DMA2_Stream4_IRQHandler
    .word ETH_IRQHandler
    .word ETH_WKUP_IRQHandler
    .word CAN2_TX_IRQHandler
    .word CAN2_RX0_IRQHandler
    .word CAN2_RX1_IRQHandler
    .word CAN2_SCE_IRQHandler
    .word OTG_FS_IRQHandler               /* 67 - USB device                */
    .word DMA2_Stream5_IRQHandler
    .word DMA2_Stream6_IRQHandler
    .word DMA2_Stream7_IRQHandler
    .word USART6_IRQHandler
    .word I2C3_EV_IRQHandler
    .word I2C3_ER_IRQHandler
    .word OTG_HS_EP1_OUT_IRQHandler
    .word OTG_HS_EP1_IN_IRQHandler
    .word OTG_HS_WKUP_IRQHandler
    .word OTG_HS_IRQHandler
    .word DCMI_IRQHandler
    .word CRYP_IRQHandler
    .word HASH_RNG_IRQHandler
    .word FPU_IRQHandler
.size g_pfnVectors, .-g_pfnVectors

/* ------------------------------------------------------------------------ */
    .macro def_default_handler name
    .weak \name
    .thumb_set \name, Default_Handler
    .endm

    def_default_handler NMI_Handler
    def_default_handler MemManage_Handler
    def_default_handler BusFault_Handler
    def_default_handler UsageFault_Handler
    def_default_handler DebugMon_Handler
    def_default_handler WWDG_IRQHandler
    def_default_handler PVD_IRQHandler
    def_default_handler TAMP_STAMP_IRQHandler
    def_default_handler RTC_WKUP_IRQHandler
    def_default_handler FLASH_IRQHandler
    def_default_handler RCC_IRQHandler
    def_default_handler EXTI0_IRQHandler
    def_default_handler EXTI1_IRQHandler
    def_default_handler EXTI2_IRQHandler
    def_default_handler EXTI3_IRQHandler
    def_default_handler EXTI4_IRQHandler
    def_default_handler DMA1_Stream0_IRQHandler
    def_default_handler DMA1_Stream1_IRQHandler
    def_default_handler DMA1_Stream2_IRQHandler
    def_default_handler DMA1_Stream3_IRQHandler
    def_default_handler DMA1_Stream4_IRQHandler
    def_default_handler DMA1_Stream5_IRQHandler
    def_default_handler DMA1_Stream6_IRQHandler
    def_default_handler ADC_IRQHandler
    def_default_handler CAN1_TX_IRQHandler
    def_default_handler CAN1_RX0_IRQHandler
    def_default_handler CAN1_RX1_IRQHandler
    def_default_handler CAN1_SCE_IRQHandler
    def_default_handler EXTI9_5_IRQHandler
    def_default_handler TIM1_BRK_TIM9_IRQHandler
    def_default_handler TIM1_UP_TIM10_IRQHandler
    def_default_handler TIM1_TRG_COM_TIM11_IRQHandler
    def_default_handler TIM1_CC_IRQHandler
    def_default_handler TIM2_IRQHandler
    def_default_handler TIM3_IRQHandler
    def_default_handler TIM4_IRQHandler
    def_default_handler I2C1_EV_IRQHandler
    def_default_handler I2C1_ER_IRQHandler
    def_default_handler I2C2_EV_IRQHandler
    def_default_handler I2C2_ER_IRQHandler
    def_default_handler SPI1_IRQHandler
    def_default_handler SPI2_IRQHandler
    def_default_handler USART1_IRQHandler
    def_default_handler USART2_IRQHandler
    def_default_handler USART3_IRQHandler
    def_default_handler EXTI15_10_IRQHandler
    def_default_handler RTC_Alarm_IRQHandler
    def_default_handler OTG_FS_WKUP_IRQHandler
    def_default_handler TIM8_BRK_TIM12_IRQHandler
    def_default_handler TIM8_UP_TIM13_IRQHandler
    def_default_handler TIM8_TRG_COM_TIM14_IRQHandler
    def_default_handler TIM8_CC_IRQHandler
    def_default_handler DMA1_Stream7_IRQHandler
    def_default_handler FSMC_IRQHandler
    def_default_handler SDIO_IRQHandler
    def_default_handler TIM5_IRQHandler
    def_default_handler SPI3_IRQHandler
    def_default_handler UART4_IRQHandler
    def_default_handler UART5_IRQHandler
    def_default_handler TIM6_DAC_IRQHandler
    def_default_handler TIM7_IRQHandler
    def_default_handler DMA2_Stream0_IRQHandler
    def_default_handler DMA2_Stream1_IRQHandler
    def_default_handler DMA2_Stream2_IRQHandler
    def_default_handler DMA2_Stream3_IRQHandler
    def_default_handler DMA2_Stream4_IRQHandler
    def_default_handler ETH_IRQHandler
    def_default_handler ETH_WKUP_IRQHandler
    def_default_handler CAN2_TX_IRQHandler
    def_default_handler CAN2_RX0_IRQHandler
    def_default_handler CAN2_RX1_IRQHandler
    def_default_handler CAN2_SCE_IRQHandler
    def_default_handler OTG_FS_IRQHandler
    def_default_handler DMA2_Stream5_IRQHandler
    def_default_handler DMA2_Stream6_IRQHandler
    def_default_handler DMA2_Stream7_IRQHandler
    def_default_handler USART6_IRQHandler
    def_default_handler I2C3_EV_IRQHandler
    def_default_handler I2C3_ER_IRQHandler
    def_default_handler OTG_HS_EP1_OUT_IRQHandler
    def_default_handler OTG_HS_EP1_IN_IRQHandler
    def_default_handler OTG_HS_WKUP_IRQHandler
    def_default_handler OTG_HS_IRQHandler
    def_default_handler DCMI_IRQHandler
    def_default_handler CRYP_IRQHandler
    def_default_handler HASH_RNG_IRQHandler
    def_default_handler FPU_IRQHandler
