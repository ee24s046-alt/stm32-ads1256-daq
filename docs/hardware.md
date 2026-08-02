# Hardware

STM32F4-Discovery (STM32F407VGT6) plus an ADS1256 analogue board. Nothing here
is exotic; the notes are about the choices that are not free.

## Pin map

| Signal | Pin | Mode | Note |
|---|---|---|---|
| SPI1 SCK | PA5 | AF5, high speed | to ADS1256 SCLK |
| SPI1 MISO | PA6 | AF5 | from ADS1256 DOUT |
| SPI1 MOSI | PA7 | AF5 | to ADS1256 DIN |
| ADC /CS | PA4 | GPIO out | software chip select |
| ADC /DRDY | PC3 | input, pull-up, **EXTI3** | falling edge |
| ADC /SYNC-PDWN | PC2 | GPIO out | idle high |
| ADC /RESET | PC1 | GPIO out | idle high |
| USART2 TX | PA2 | AF7 | ST-LINK VCP, 921600 8N1 |
| USART2 RX | PA3 | AF7 | |
| USB DM | PA11 | AF10 | fixed by silicon |
| USB DP | PA12 | AF10 | fixed by silicon |
| LED heartbeat | PD13 | GPIO out | orange |
| LED error | PD14 | GPIO out | red |

### Why `/DRDY` is on PC3 specifically

It has to be on an EXTI line that is not shared. EXTI0–4 each have their own
vector; EXTI5–15 are multiplexed onto two. Putting the most timing-critical
interrupt in the system on a shared vector would add a source-decode branch to
the ISR whose measured worst case is 1.21 µs, for no benefit.

### Chip select is a GPIO, not hardware NSS

Hardware NSS deasserts between bytes. The ADS1256 requires CS to stay low across
a whole command-plus-data sequence, so `/CS` is driven manually and the whole
9-byte channel-cycling sequence happens inside one CS window.

`SSM` + `SSI` are both set in `SPI_CR1`. Setting `SSM` without `SSI` leaves the
internal NSS low, the peripheral drops out of master mode and sets `MODF`, and
the symptom is "SPI does nothing" with no obvious cause.

## Clock tree

```
HSE 8 MHz (ST-LINK MCO, square wave → HSEBYP set)
   ÷ M=8   → 1 MHz VCO input
   × N=336 → 336 MHz VCO
   ÷ P=2   → 168 MHz SYSCLK
   ÷ Q=7   → 48.000 MHz USB
```

The ÷7 on Q is why N is 336 and not, say, 320: getting exactly 48.000 MHz out of
the same VCO that produces 168 MHz constrains the solution to essentially this
one point. A "close enough" USB clock enumerates fine on some hosts and fails
CRC on others, which is a miserable bug to chase.

AHB ÷1 (168 MHz), APB1 ÷4 (42 MHz, max 42), APB2 ÷2 (84 MHz, max 84).

Note that APB1's **timer** clock is doubled to 84 MHz because the APB1 prescaler
is not 1. Forgetting that doubling makes a nominal 1 MHz TIM2 tick at 500 kHz,
which shows up as every timestamp being exactly 2× wrong — and reads as a
sample-rate bug rather than a clock bug.

## ADS1256 front end

| Parameter | Value | Consequence |
|---|---|---|
| CLKIN | 7.68 MHz can oscillator | sets SCLK ≤ 1.92 MHz and all DRATE values |
| VREF | 2.5 V | full scale = ±2·VREF/PGA = ±5 V |
| PGA | 1 | |
| Buffer | enabled | raises input impedance, costs a little noise |
| DRATE | 30 kSPS | with 4-channel cycling → 7500 settled conversions/s |
| Channels | AIN0-1, AIN2-3, AIN4-5, AIN6-7 | four differential pairs |

`CLKOUT` is disabled in `ADCON`. It is a square wave at 7.68 MHz sitting next to
a 24-bit converter; leaving it running is free noise.

### SCLK selection

The constraint is SCLK ≤ fCLKIN/4 = 1.92 MHz. From an 84 MHz APB2 the usable
prescalers are:

| Prescaler | SCLK | Verdict |
|---|---:|---|
| ÷32 | 2.625 MHz | **out of spec** — and it mostly works, which is worse |
| ÷64 | 1.3125 MHz | used |
| ÷128 | 656 kHz | unnecessarily slow |

"Mostly works" is the dangerous case: it violates a setup time rather than a hard
limit, so it fails intermittently and temperature-dependently. This is why
`ads1256_init()` verifies every written register by read-back — a corrupted
`DRATE` write is otherwise silent and turns into hours of "why is my noise floor
20 dB high".

## Analogue notes

For the noise figures in [measured-results.md](measured-results.md) to be
reproducible:

- Star-ground the analogue section at the ADS1256's AGND pin. The most common
  cause of a noise floor several dB above datasheet is the return current from
  the digital side sharing copper with the reference.
- The 2.5 V reference wants its own low-ESR bulk cap close to the pin, and the
  reference's own noise appears directly in the measurement — a noisy reference
  cannot be calibrated out.
- Short the differential pairs **at the input connector**, not at the ADC pins,
  when characterising. Shorting at the chip measures the converter; shorting at
  the connector measures the instrument, which is the thing being built.
- USB introduces a ground path to the host. If the noise floor changes when the
  host is plugged into mains versus running on battery, that is what you are
  seeing, and it is a real effect rather than a firmware problem.

## Bill of materials

| Item | Part |
|---|---|
| MCU board | STM32F4-Discovery (STM32F407VGT6) |
| ADC | TI ADS1256, 24-bit Σ-Δ, 8-channel mux |
| Clock | 7.68 MHz CMOS oscillator |
| Reference | 2.5 V precision shunt or series reference |
| Link | USB micro-B to the OTG_FS connector (PA11/PA12) |
| Debug | ST-LINK VCP on PA2/PA3 |
