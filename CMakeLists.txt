cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# stm32-ads1256-daq
#
# Two build modes live in this one tree:
#
#   1. Firmware  (cross)  - requires the arm-none-eabi toolchain file:
#        cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
#
#   2. Host tests (native) - the portable core (SPSC ring, packet codec,
#      calibration math, CRC) compiled for the build machine and run under
#      CTest.  No hardware, no toolchain:
#        cmake -B build-host -DDAQ_BUILD_HOST_TESTS=ON
#
# The portable core is *the same source files* in both modes.  That is the
# whole point: the lock-free ring buffer and the wire codec that ship in the
# firmware are the ones covered by the unit tests.
# ---------------------------------------------------------------------------

project(stm32-ads1256-daq C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DAQ_BUILD_HOST_TESTS "Build the portable core natively and run unit tests" OFF)

# Shared warning set - deliberately strict.  The firmware builds clean at this
# level; anything that does not is a bug, not a nuisance.
set(DAQ_WARNINGS
    -Wall -Wextra -Wshadow -Wundef -Wconversion -Wsign-conversion
    -Wdouble-promotion -Wpointer-arith -Wcast-align -Wstrict-prototypes
    -Wmissing-prototypes -Wredundant-decls -Wswitch-default
    -Werror=implicit-function-declaration -Werror=return-type)

# ---------------------------------------------------------------------------
# Portable core - no MCU headers, no RTOS headers, no libc beyond string.h
# ---------------------------------------------------------------------------
add_library(daq_core STATIC
    firmware/lib/spsc_ring.c
    firmware/lib/packet.c
    firmware/lib/crc.c
    firmware/lib/calibration.c
)
target_include_directories(daq_core PUBLIC firmware/include)
target_compile_options(daq_core PRIVATE ${DAQ_WARNINGS})

# ---------------------------------------------------------------------------
# Mode 2: native unit tests
# ---------------------------------------------------------------------------
if(DAQ_BUILD_HOST_TESTS)
    enable_testing()
    add_subdirectory(tests)
    return()
endif()

# ---------------------------------------------------------------------------
# Mode 1: firmware
# ---------------------------------------------------------------------------
if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "Firmware build needs the cross toolchain.\n"
        "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake\n"
        "or build the portable core natively with -DDAQ_BUILD_HOST_TESTS=ON.")
endif()

# --- FreeRTOS kernel -------------------------------------------------------
# Vendored via FetchContent so the repo carries no third-party source.  Point
# FREERTOS_KERNEL_PATH at a local checkout to build without network access.
set(FREERTOS_KERNEL_PATH "" CACHE PATH "Local FreeRTOS-Kernel checkout (optional)")
if(FREERTOS_KERNEL_PATH)
    set(FRK_DIR "${FREERTOS_KERNEL_PATH}")
else()
    include(FetchContent)
    FetchContent_Declare(freertos_kernel
        GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
        GIT_TAG        V11.1.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(freertos_kernel)
    set(FRK_DIR "${freertos_kernel_SOURCE_DIR}")
endif()

set(MCU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)

add_library(freertos STATIC
    ${FRK_DIR}/tasks.c
    ${FRK_DIR}/list.c
    ${FRK_DIR}/queue.c
    ${FRK_DIR}/timers.c
    ${FRK_DIR}/portable/GCC/ARM_CM4F/port.c
)
target_include_directories(freertos PUBLIC
    ${FRK_DIR}/include
    ${FRK_DIR}/portable/GCC/ARM_CM4F
    ${CMAKE_SOURCE_DIR}/firmware/config
    ${CMAKE_SOURCE_DIR}/firmware/include
)
target_compile_options(freertos PRIVATE ${MCU_FLAGS} -O2 -ffunction-sections -fdata-sections)
# NOTE: heap_*.c is intentionally absent.  configSUPPORT_DYNAMIC_ALLOCATION is
# 0; every task, queue and timer in this firmware is statically allocated.  If
# someone later calls pvPortMalloc(), the build breaks at link time.  That is
# the enforcement mechanism.

add_executable(daq.elf
    firmware/startup/startup_stm32f407xx.s
    firmware/core/main.c
    firmware/core/system_clock.c
    firmware/core/interrupts.c
    firmware/core/fault_handler.c
    firmware/core/freertos_hooks.c
    firmware/core/cycle_counter.c
    firmware/drivers/gpio.c
    firmware/drivers/spi.c
    firmware/drivers/uart_debug.c
    firmware/drivers/flash_store.c
    firmware/drivers/ads1256.c
    firmware/usb/usb_core.c
    firmware/usb/usb_descriptors.c
    firmware/usb/usb_cdc.c
    firmware/rtos/task_sampler.c
    firmware/rtos/task_processor.c
    firmware/rtos/task_uplink.c
    firmware/rtos/shared.c
)
target_include_directories(daq.elf PRIVATE
    firmware/include firmware/config)
target_compile_options(daq.elf PRIVATE
    ${MCU_FLAGS} ${DAQ_WARNINGS}
    $<$<CONFIG:Debug>:-Og -g3>
    $<$<CONFIG:Release>:-O2 -g>
    -ffunction-sections -fdata-sections -fno-common)
target_link_options(daq.elf PRIVATE
    ${MCU_FLAGS}
    -T${CMAKE_SOURCE_DIR}/firmware/linker/STM32F407VGTx_FLASH.ld
    --specs=nano.specs --specs=nosys.specs
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_BINARY_DIR}/daq.map,--cref
    -Wl,--print-memory-usage)
target_link_libraries(daq.elf PRIVATE freertos daq_core)
target_compile_definitions(daq.elf PRIVATE STM32F407xx)
target_compile_options(daq_core PRIVATE ${MCU_FLAGS})

# Post-build artefacts + a hard size gate, so a careless change that doubles
# .bss shows up in CI rather than on the bench.
add_custom_command(TARGET daq.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:daq.elf> ${CMAKE_BINARY_DIR}/daq.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:daq.elf> ${CMAKE_BINARY_DIR}/daq.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:daq.elf>
    COMMENT "Generating daq.bin / daq.hex and reporting size")
