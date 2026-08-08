# Hardware

Electrical documentation for the acquisition front end. The pin map, clock tree
and analogue notes live in [`docs/hardware.md`](../docs/hardware.md); this
directory holds the design files and the interconnect drawing.

## Which case applies

**A. Off-the-shelf ADS1256 module.** Then there is no schematic to publish —
redrawing someone else's board would be odd, and the pin table in
`docs/hardware.md` already carries the electrical detail. What belongs here is
an **interconnect diagram**: the eleven signals between the Discovery board and
the module, plus power and grounding. A clean hand drawing photographed, or a
Draw.io / KiCad-schematic-as-documentation export, both work.

Put it at `hardware/interconnect.pdf` and note the module's marking (vendor,
silkscreen part number) in the table below so a reader can find the same board.

**B. You designed the analogue front end.** Then this directory is one of the
strongest things in the repository, and it changes what the project *is* — from
"firmware on a dev board" to "an instrument". Include:

```
hardware/
  schematic.pdf            full schematic, readable at 100 %
  pcb-top.png              board render or photo, top
  pcb-bottom.png
  bom.csv                  with manufacturer part numbers
  gerbers/                 optional, but it makes the design reproducible
  design-notes.md          see the prompts below
```

`design-notes.md` should answer the questions a hardware interviewer will ask
anyway:

- **Reference.** Which part, why that one, and what its noise contributes. The
  reference's own noise appears directly in every measurement and cannot be
  calibrated out — so its spec is an upper bound on the instrument.
- **Anti-alias filter.** Corner frequency and order, against the 30 kSPS
  modulator rate and the sinc decimation. Note what the ADS1256's own digital
  filter does and does not do for you.
- **Input buffer / impedance.** `STATUS.BUFEN` is enabled in firmware, which
  raises input impedance at a small noise cost. Say why that trade was made.
- **Grounding.** Where the analogue and digital returns meet, and why there.
  The most common cause of a noise floor several dB above datasheet is digital
  return current sharing copper with the reference.
- **Supply.** Rails, regulators, and any filtering between the digital supply
  and AVDD.

## Interconnect

Eleven signals. Full detail and the reasoning for each choice in
[`docs/hardware.md`](../docs/hardware.md).

| STM32F407 | Direction | ADS1256 |
|---|:---:|---|
| PA5 (SPI1_SCK, AF5) | → | SCLK |
| PA7 (SPI1_MOSI, AF5) | → | DIN |
| PA6 (SPI1_MISO, AF5) | ← | DOUT |
| PA4 (GPIO) | → | /CS |
| PC3 (EXTI3, pull-up) | ← | /DRDY |
| PC2 (GPIO) | → | /SYNC-PDWN |
| PC1 (GPIO) | → | /RESET |
| 3V3 | → | DVDD |
| GND | — | DGND |
| — | — | AVDD / AGND from the analogue supply |
| — | — | 7.68 MHz oscillator → CLKIN |

Two constraints that are not free choices: `/DRDY` must sit on EXTI0–4 (each has
its own vector; EXTI5–15 are multiplexed, which would add a source-decode branch
to the most timing-critical ISR in the system), and PA11/PA12 are fixed by
silicon as OTG_FS DM/DP.

## Bill of materials

| Item | Part | Notes |
|---|---|---|
| MCU board | STM32F4-Discovery (STM32F407VGT6) | 168 MHz, 1 MB flash, 192 KB RAM incl. 64 KB CCM |
| ADC | TI ADS1256 | 24-bit Σ-Δ, 8-ch mux, 30 kSPS |
| Clock | 7.68 MHz CMOS oscillator | sets SCLK ≤ 1.92 MHz and every DRATE value |
| Reference | 2.5 V precision reference | full scale = ±2·VREF/PGA = ±5 V |
| Link | USB micro-B → OTG_FS | PA11/PA12 |
| Debug | ST-LINK VCP | PA2/PA3, 921600 8N1 |

Fill in manufacturer part numbers for the reference and the oscillator — they
are the two components that actually bound the noise performance, so naming them
is part of making the noise figures meaningful.
