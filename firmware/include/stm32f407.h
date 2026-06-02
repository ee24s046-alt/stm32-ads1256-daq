/**
 * @file    stm32f407.h
 * @brief   Hand-written STM32F407 peripheral map - only the blocks used here.
 *
 * Register layouts transcribed from RM0090 Rev 19.  Every offset in this file
 * is reachable by the firmware; nothing is here "just in case".
 */
#ifndef STM32F407_H
#define STM32F407_H

#include "cortex_m4.h"

/* ------------------------------------------------------------------------ */
/* Peripheral bases                                                          */
/* ------------------------------------------------------------------------ */
#define PERIPH_BASE       0x40000000UL
#define APB1PERIPH_BASE   (PERIPH_BASE + 0x00000000UL)
#define APB2PERIPH_BASE   (PERIPH_BASE + 0x00010000UL)
#define AHB1PERIPH_BASE   (PERIPH_BASE + 0x00020000UL)
#define USB_OTG_FS_BASE   0x50000000UL

/* ------------------------------------------------------------------------ */
/* RCC                                                                       */
/* ------------------------------------------------------------------------ */
typedef struct {
    __IO uint32_t CR;          /* 0x00 */
    __IO uint32_t PLLCFGR;     /* 0x04 */
    __IO uint32_t CFGR;        /* 0x08 */
    __IO uint32_t CIR;         /* 0x0C */
    __IO uint32_t AHB1RSTR;    /* 0x10 */
    __IO uint32_t AHB2RSTR;    /* 0x14 */
    __IO uint32_t AHB3RSTR;    /* 0x18 */
    uint32_t      RES0;
    __IO uint32_t APB1RSTR;    /* 0x20 */
    __IO uint32_t APB2RSTR;    /* 0x24 */
    uint32_t      RES1[2];
    __IO uint32_t AHB1ENR;     /* 0x30 */
    __IO uint32_t AHB2ENR;     /* 0x34 */
    __IO uint32_t AHB3ENR;     /* 0x38 */
    uint32_t      RES2;
    __IO uint32_t APB1ENR;     /* 0x40 */
    __IO uint32_t APB2ENR;     /* 0x44 */
    uint32_t      RES3[2];
    __IO uint32_t AHB1LPENR;
    __IO uint32_t AHB2LPENR;
    __IO uint32_t AHB3LPENR;
    uint32_t      RES4;
    __IO uint32_t APB1LPENR;
    __IO uint32_t APB2LPENR;
    uint32_t      RES5[2];
    __IO uint32_t BDCR;
    __IO uint32_t CSR;
} RCC_Type;

#define RCC ((RCC_Type *)(AHB1PERIPH_BASE + 0x3800UL))

#define RCC_CR_HSION      (1UL << 0)
#define RCC_CR_HSIRDY     (1UL << 1)
#define RCC_CR_HSEON      (1UL << 16)
#define RCC_CR_HSERDY     (1UL << 17)
#define RCC_CR_HSEBYP     (1UL << 18)
#define RCC_CR_PLLON      (1UL << 24)
#define RCC_CR_PLLRDY     (1UL << 25)

#define RCC_CFGR_SW_Msk   (3UL << 0)
#define RCC_CFGR_SW_PLL   (2UL << 0)
#define RCC_CFGR_SWS_Msk  (3UL << 2)
#define RCC_CFGR_SWS_PLL  (2UL << 2)
#define RCC_CFGR_HPRE_DIV1  (0UL << 4)
#define RCC_CFGR_PPRE1_DIV4 (5UL << 10)
#define RCC_CFGR_PPRE2_DIV2 (4UL << 13)

#define RCC_AHB1ENR_GPIOAEN (1UL << 0)
#define RCC_AHB1ENR_GPIOBEN (1UL << 1)
#define RCC_AHB1ENR_GPIOCEN (1UL << 2)
#define RCC_AHB1ENR_GPIODEN (1UL << 3)
#define RCC_AHB2ENR_OTGFSEN (1UL << 7)
#define RCC_APB1ENR_TIM2EN  (1UL << 0)
#define RCC_APB1ENR_USART2EN (1UL << 17)
#define RCC_APB1ENR_PWREN   (1UL << 28)
#define RCC_APB2ENR_SPI1EN  (1UL << 12)
#define RCC_APB2ENR_SYSCFGEN (1UL << 14)

/* ------------------------------------------------------------------------ */
/* PWR / FLASH                                                               */
/* ------------------------------------------------------------------------ */
typedef struct { __IO uint32_t CR; __IO uint32_t CSR; } PWR_Type;
#define PWR ((PWR_Type *)(APB1PERIPH_BASE + 0x7000UL))
#define PWR_CR_VOS (3UL << 14)

typedef struct {
    __IO uint32_t ACR;
    __IO uint32_t KEYR;
    __IO uint32_t OPTKEYR;
    __IO uint32_t SR;
    __IO uint32_t CR;
    __IO uint32_t OPTCR;
} FLASH_Type;

#define FLASH ((FLASH_Type *)(AHB1PERIPH_BASE + 0x3C00UL))

#define FLASH_ACR_LATENCY_5WS (5UL << 0)
#define FLASH_ACR_PRFTEN      (1UL << 8)
#define FLASH_ACR_ICEN        (1UL << 9)
#define FLASH_ACR_DCEN        (1UL << 10)

#define FLASH_KEY1 0x45670123UL
#define FLASH_KEY2 0xCDEF89ABUL

#define FLASH_SR_EOP    (1UL << 0)
#define FLASH_SR_OPERR  (1UL << 1)
#define FLASH_SR_WRPERR (1UL << 4)
#define FLASH_SR_PGAERR (1UL << 5)
#define FLASH_SR_PGPERR (1UL << 6)
#define FLASH_SR_PGSERR (1UL << 7)
#define FLASH_SR_BSY    (1UL << 16)
#define FLASH_SR_ERRORS (FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                         FLASH_SR_PGPERR | FLASH_SR_PGSERR)

#define FLASH_CR_PG     (1UL << 0)
#define FLASH_CR_SER    (1UL << 1)
#define FLASH_CR_SNB_Pos 3U
#define FLASH_CR_PSIZE_X32 (2UL << 8)
#define FLASH_CR_STRT   (1UL << 16)
#define FLASH_CR_LOCK   (1UL << 31)

/* ------------------------------------------------------------------------ */
/* GPIO                                                                      */
/* ------------------------------------------------------------------------ */
typedef struct {
    __IO uint32_t MODER;
    __IO uint32_t OTYPER;
    __IO uint32_t OSPEEDR;
    __IO uint32_t PUPDR;
    __I  uint32_t IDR;
    __IO uint32_t ODR;
    __O  uint32_t BSRR;
    __IO uint32_t LCKR;
    __IO uint32_t AFR[2];
} GPIO_Type;

#define GPIOA ((GPIO_Type *)(AHB1PERIPH_BASE + 0x0000UL))
#define GPIOB ((GPIO_Type *)(AHB1PERIPH_BASE + 0x0400UL))
#define GPIOC ((GPIO_Type *)(AHB1PERIPH_BASE + 0x0800UL))
#define GPIOD ((GPIO_Type *)(AHB1PERIPH_BASE + 0x0C00UL))

/* ------------------------------------------------------------------------ */
/* SYSCFG / EXTI                                                             */
/* ------------------------------------------------------------------------ */
typedef struct {
    __IO uint32_t MEMRMP;
    __IO uint32_t PMC;
    __IO uint32_t EXTICR[4];
    uint32_t      RES[2];
    __IO uint32_t CMPCR;
} SYSCFG_Type;
#define SYSCFG ((SYSCFG_Type *)(APB2PERIPH_BASE + 0x3800UL))

typedef struct {
    __IO uint32_t IMR;
    __IO uint32_t EMR;
    __IO uint32_t RTSR;
    __IO uint32_t FTSR;
    __IO uint32_t SWIER;
    __IO uint32_t PR;
} EXTI_Type;
#define EXTI ((EXTI_Type *)(APB2PERIPH_BASE + 0x3C00UL))

/* ------------------------------------------------------------------------ */
/* SPI                                                                       */
/* ------------------------------------------------------------------------ */
typedef struct {
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t SR;
    __IO uint32_t DR;
    __IO uint32_t CRCPR;
    __I  uint32_t RXCRCR;
    __I  uint32_t TXCRCR;
    __IO uint32_t I2SCFGR;
    __IO uint32_t I2SPR;
} SPI_Type;

#define SPI1 ((SPI_Type *)(APB2PERIPH_BASE + 0x3000UL))

#define SPI_CR1_CPHA   (1UL << 0)
#define SPI_CR1_CPOL   (1UL << 1)
#define SPI_CR1_MSTR   (1UL << 2)
#define SPI_CR1_BR_Pos 3U
#define SPI_CR1_SPE    (1UL << 6)
#define SPI_CR1_LSBFIRST (1UL << 7)
#define SPI_CR1_SSI    (1UL << 8)
#define SPI_CR1_SSM    (1UL << 9)
#define SPI_SR_RXNE    (1UL << 0)
#define SPI_SR_TXE     (1UL << 1)
#define SPI_SR_BSY     (1UL << 7)
#define SPI_SR_OVR     (1UL << 6)

/* ------------------------------------------------------------------------ */
/* USART                                                                     */
/* ------------------------------------------------------------------------ */
typedef struct {
    __IO uint32_t SR;
    __IO uint32_t DR;
    __IO uint32_t BRR;
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t CR3;
    __IO uint32_t GTPR;
} USART_Type;

#define USART2 ((USART_Type *)(APB1PERIPH_BASE + 0x4400UL))
#define USART_SR_TXE  (1UL << 7)
#define USART_SR_TC   (1UL << 6)
#define USART_CR1_TE  (1UL << 3)
#define USART_CR1_RE  (1UL << 2)
#define USART_CR1_UE  (1UL << 13)

/* ------------------------------------------------------------------------ */
/* TIM2 - 32-bit, 1 MHz tick.  Sole source of the packet timestamp field.    */
/* ------------------------------------------------------------------------ */
typedef struct {
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t SMCR;
    __IO uint32_t DIER;
    __IO uint32_t SR;
    __IO uint32_t EGR;
    __IO uint32_t CCMR1;
    __IO uint32_t CCMR2;
    __IO uint32_t CCER;
    __IO uint32_t CNT;
    __IO uint32_t PSC;
    __IO uint32_t ARR;
} TIM_Type;

#define TIM2 ((TIM_Type *)(APB1PERIPH_BASE + 0x0000UL))
#define TIM_CR1_CEN (1UL << 0)
#define TIM_EGR_UG  (1UL << 0)

/* ------------------------------------------------------------------------ */
/* USB OTG FS (device subset)                                                */
/* ------------------------------------------------------------------------ */
typedef struct {
    __IO uint32_t GOTGCTL;
    __IO uint32_t GOTGINT;
    __IO uint32_t GAHBCFG;
    __IO uint32_t GUSBCFG;
    __IO uint32_t GRSTCTL;
    __IO uint32_t GINTSTS;
    __IO uint32_t GINTMSK;
    __IO uint32_t GRXSTSR;
    __IO uint32_t GRXSTSP;
    __IO uint32_t GRXFSIZ;
    __IO uint32_t DIEPTXF0;
    __IO uint32_t HNPTXSTS;
    uint32_t      RES0[2];
    __IO uint32_t GCCFG;
    __IO uint32_t CID;
    uint32_t      RES1[48];
    __IO uint32_t HPTXFSIZ;
    __IO uint32_t DIEPTXF[15];
} USB_Global_Type;

typedef struct {
    __IO uint32_t DCFG;
    __IO uint32_t DCTL;
    __IO uint32_t DSTS;
    uint32_t      RES0;
    __IO uint32_t DIEPMSK;
    __IO uint32_t DOEPMSK;
    __IO uint32_t DAINT;
    __IO uint32_t DAINTMSK;
    uint32_t      RES1[2];
    __IO uint32_t DVBUSDIS;
    __IO uint32_t DVBUSPULSE;
    __IO uint32_t DTHRCTL;
    __IO uint32_t DIEPEMPMSK;
} USB_Device_Type;

typedef struct {
    __IO uint32_t CTL;
    uint32_t      RES0;
    __IO uint32_t INT;
    uint32_t      RES1;
    __IO uint32_t TSIZ;
    uint32_t      RES2;
    __IO uint32_t TXFSTS;   /* IN endpoints only */
    uint32_t      RES3;
} USB_EP_Type;

#define USB_GLOBAL ((USB_Global_Type *)(USB_OTG_FS_BASE + 0x0000UL))
#define USB_DEVICE ((USB_Device_Type *)(USB_OTG_FS_BASE + 0x0800UL))
#define USB_INEP(i)  ((USB_EP_Type *)(USB_OTG_FS_BASE + 0x0900UL + ((i) * 0x20UL)))
#define USB_OUTEP(i) ((USB_EP_Type *)(USB_OTG_FS_BASE + 0x0B00UL + ((i) * 0x20UL)))
#define USB_PCGCCTL  (*(__IO uint32_t *)(USB_OTG_FS_BASE + 0x0E00UL))
#define USB_FIFO(i)  ((__IO uint32_t *)(USB_OTG_FS_BASE + 0x1000UL + ((i) * 0x1000UL)))

#define GAHBCFG_GINTMSK   (1UL << 0)
#define GAHBCFG_TXFELVL   (1UL << 7)
#define GUSBCFG_PHYSEL    (1UL << 6)
#define GUSBCFG_FDMOD     (1UL << 30)
#define GRSTCTL_CSRST     (1UL << 0)
#define GRSTCTL_RXFFLSH   (1UL << 4)
#define GRSTCTL_TXFFLSH   (1UL << 5)
#define GRSTCTL_AHBIDL    (1UL << 31)
#define GCCFG_PWRDWN      (1UL << 16)
#define GCCFG_VBUSBSEN    (1UL << 19)
#define GCCFG_NOVBUSSENS  (1UL << 21)

#define GINTSTS_USBRST    (1UL << 12)
#define GINTSTS_ENUMDNE   (1UL << 13)
#define GINTSTS_RXFLVL    (1UL << 4)
#define GINTSTS_IEPINT    (1UL << 18)
#define GINTSTS_OEPINT    (1UL << 19)
#define GINTSTS_USBSUSP   (1UL << 11)
#define GINTSTS_WKUPINT   (1UL << 31)
#define GINTSTS_SOF       (1UL << 3)

#define DCTL_SDIS         (1UL << 1)
#define DCFG_DSPD_FS      (3UL << 0)
#define DSTS_ENUMSPD_Msk  (3UL << 1)

#define DEPCTL_EPENA      (1UL << 31)
#define DEPCTL_EPDIS      (1UL << 30)
#define DEPCTL_SNAK       (1UL << 27)
#define DEPCTL_CNAK       (1UL << 26)
#define DEPCTL_STALL      (1UL << 21)
#define DEPCTL_USBAEP     (1UL << 15)
#define DEPCTL_SD0PID     (1UL << 28)
#define DEPCTL_TXFNUM_Pos 22U
#define DEPCTL_EPTYP_Pos  18U

#define DIEPINT_XFRC      (1UL << 0)
#define DOEPINT_XFRC      (1UL << 0)
#define DOEPINT_STUP      (1UL << 3)

/* ------------------------------------------------------------------------ */
/* IRQ numbers (subset)                                                      */
/* ------------------------------------------------------------------------ */
typedef enum {
    MemoryManagement_IRQn = -12,
    BusFault_IRQn         = -11,
    UsageFault_IRQn       = -10,
    SVCall_IRQn           = -5,
    PendSV_IRQn           = -2,
    SysTick_IRQn          = -1,
    EXTI0_IRQn            = 6,
    EXTI1_IRQn            = 7,
    EXTI2_IRQn            = 8,
    EXTI3_IRQn            = 9,
    EXTI4_IRQn            = 10,
    SPI1_IRQn             = 35,
    OTG_FS_IRQn           = 67,
} IRQn_Type;

#endif /* STM32F407_H */
