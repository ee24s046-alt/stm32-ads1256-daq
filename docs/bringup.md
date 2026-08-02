# Bring-up

## Toolchain

```bash
# Debian / Ubuntu
sudo apt install gcc-arm-none-eabi cmake ninja-build openocd

# macOS
brew install --cask gcc-arm-embedded && brew install cmake ninja open-ocd
```

## Build

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
cmake --build build
```

FreeRTOS V11.1.0 is fetched at configure time. To build offline, point at a local
checkout:

```bash
git clone --depth 1 -b V11.1.0 https://github.com/FreeRTOS/FreeRTOS-Kernel.git
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
      -DFREERTOS_KERNEL_PATH=$PWD/FreeRTOS-Kernel
```

A successful build prints the memory report and produces `daq.elf`, `daq.bin`,
`daq.hex` and `daq.map`.

## Flash

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program build/daq.elf verify reset exit"

# or
st-flash write build/daq.bin 0x08000000
```

## First light

Attach a terminal to the ST-LINK VCP at **921600 8N1** before powering up — the
banner is the first thing out and it is the only diagnosis you get if USB never
enumerates.

```
=================================================
 ADS1256 4-channel precision DAQ
 STM32F407 @ 168 MHz  |  FreeRTOS, fully static
 build: Sep 16 2026 11:42:07
=================================================
[cal ] none stored - unity gain, zero offset
[adc ] ok, 30 kSPS raw / 7.5 kSPS settled / 1875 scans per s
[usb ] attached, waiting for enumeration
[rtos] starting scheduler
```

Then:

```bash
pip install -e host[dev]
ads1256 monitor --port /dev/ttyACM0
```

## Failure modes, in the order you will meet them

### Nothing at all on the UART

The clock never came up, or the UART pins are wrong. `SystemInit` bounds every
wait loop precisely so this cannot hang silently — if HSE fails you get
`!! HSE FAILED (stage N) - running on HSI` and the part keeps going at 16 MHz.
Stage 1 is HSE not ready (check `HSEBYP`: the Discovery board is fed a square
wave from the ST-LINK, not a crystal), stage 2 is PLL not locking, stage 3 is the
switch not taking.

If there is genuinely nothing, the UART itself is misconfigured — check PA2/PA3
and that you are at 921600, not 115200.

### `[adc ] FAIL: /DRDY never asserted`

The ADS1256 has no CLKIN, or is not powered, or `/RESET` is held low. Scope
CLKIN first: without it the converter does nothing at all and every other symptom
follows from that.

### `[adc ] FAIL: STATUS.ID mismatch`

The SPI link is wrong. `STATUS[7:4]` reads `0b0011` on every ADS1256; getting
`0x00` or `0xFF` means MISO is not connected, CS is not being driven, or the SPI
mode is wrong (the ADS1256 wants CPOL = 0, CPHA = 1).

This check exists because a converter that is simply not connected otherwise
looks identical to one returning legitimate zeros, and "all my readings are zero"
is far more expensive to find at the analysis stage than at init.

### `[adc ] FAIL: register read-back mismatch`

A write did not stick. Almost always SCLK is too fast: the limit is
fCLKIN/4 = 1.92 MHz and the ÷32 prescaler gives 2.625 MHz, which *mostly* works
because it violates a setup time rather than a hard limit. Confirm
`SPI1_BR_DIV64` in `board.h`.

The other cause is a missing t6 delay before the data phase of an `RREG` —
registers then read back as `0x00` or as the previous value, intermittently, and
only at higher SCLK rates.

### Device never enumerates

- HSE failed, so the USB clock is not 48.000 MHz. Check the banner.
- VBUS sensing. The Discovery board does not wire VBUS to PA9; with sensing
  enabled the core never sees a valid session and simply never attaches. The
  firmware sets `GCCFG_NOVBUSSENS` for this reason — if you are on a board that
  *does* wire VBUS, clear it.
- On Windows, check Device Manager for a CDC device with a driver problem rather
  than assuming nothing appeared.

### Enumerates, then the stream stalls after a few hundred packets

Look at `stats`. If `usb retries` is climbing and `ring tx peak` is near 256, the
host is not draining. If `deadline misses` is non-zero, the sampler is being
starved — check that nothing has been added at an interrupt priority more urgent
than 5.

### `*** configASSERT failed ***`

Left enabled in release builds deliberately. The common trigger is an ISR at a
numeric priority below 5 calling a `FromISR()` API. Remember the STM32F4
implements only the top 4 bits, so priority 5 is register value `0x50`; passing
`5` directly to a register write gives priority 0, which is more urgent than the
kernel and will corrupt the scheduler.

### `*** STACK OVERFLOW in task: <name> ***`

Raise that task's depth in `shared.h`, rebuild, and re-check the high-water marks
with `stats`. Do not simply double it — the reported marks tell you the real
number.

### `================ FAULT ================`

The handler prints a decoded diagnosis. Two things to read carefully:

- **`IMPRECISE data bus error`** means a buffered write failed and the reported
  PC is *not* the culprit. Rebuild with `-O0` or set `ACTLR.DISDEFWBUF` to make
  it precise, then reproduce.
- **`MMFAR/BFAR: not valid`** means those registers hold stale garbage. The
  handler only prints them when the corresponding VALID bit is set, precisely
  because reporting them unconditionally has sent many people chasing an address
  that was never accessed.

`INVSTATE` almost always means a branch to an address with bit 0 clear — a lost
Thumb bit, usually from a hand-made function pointer.

## Running the tests

```bash
# portable core, natively, under ASan + UBSan
cmake -B build-host -DDAQ_BUILD_HOST_TESTS=ON
cmake --build build-host && ctest --test-dir build-host --output-on-failure

# host tooling
cd host && python -m pytest -q

# end-to-end decode path, no hardware needed
ads1256 selftest
```

## A 5-minute acceptance run

```bash
ads1256 capture --port /dev/ttyACM0 --seconds 300 --out run.npz --raw run.bin
ads1256 verify run.npz          # must print RESULT: PASS
ads1256 noise  run.npz          # inputs shorted
```

Then `stats` on the UART and compare against
[measured-results.md](measured-results.md).
