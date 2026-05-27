# zedmd-pico-drivers

`zedmd-pico-drivers` is a HUB75 LED matrix driver for RP2040/RP2350-based boards.
It is derived from Pimoroni's `hub75` driver and contains ZeDMD-specific changes such as:

- support for additional shift-driver and line-decoder chips
- optional split control pins for dual-head output
- timing changes for panels that do not behave like plain HUB75 panels

This package currently exports the code in [`drivers/hub75`](drivers/hub75).

## Overview

The main class is `pimoroni::Hub75` from [`drivers/hub75/hub75.hpp`](drivers/hub75/hub75.hpp).

The driver:

- stores pixels in a packed internal framebuffer
- uses PIO for pixel shifting and row/latch timing
- uses DMA to feed pixel data continuously
- supports one physical panel chain or two heads with separate `CLK/LAT/OE` pins

The constructor takes:

- panel `width`
- panel `height`
- optional external `Pixel *buffer`
- `ShiftDriver`
- `LineDecoder`
- optional `inverted_stb`
- optional `COLOR_ORDER`
- optional LUT table

Typical usage:

```cpp
#include "hub75.hpp"

using namespace pimoroni;

Hub75 display(
    128,
    64,
    nullptr,
    SHIFT_DRIVER_FM6124,
    LINE_DECODER_TYPE595,
    false
);
```

## Pin Model

By default the driver expects the usual HUB75 pin layout:

- color data: `R0 G0 B0 R1 G1 B1`
- row pins: `A B C D E`
- control pins: `CLK LAT OE`

The defaults come from the `HUB75_*` macros in [`hub75.hpp`](drivers/hub75/hub75.hpp). Override those macros before including the header if your board uses different pins.

For dual-head mode, define separate:

- `HUB75_CLK2`
- `HUB75_LAT2`
- `HUB75_OE2`

If those differ from `HUB75_CLK/HUB75_LAT/HUB75_OE`, the driver enables split-head mode automatically.

## ShiftDriver

`ShiftDriver` selects the behavior of the panel's column driver / PWM driver chips.

### `SHIFT_DRIVER_SHIFTREG`

Generic HUB75 scan path. Use this for normal panels with no known special init or latch timing requirements.

### `SHIFT_DRIVER_FM6124`

Uses the same runtime scan/latch path as `DP3246`, but skips the `DP3246` magic startup sequence. This is intentional because some FM6124-family panels behave correctly with the DP3246-style timing but must not receive the DP3246 init sequence.

### `SHIFT_DRIVER_FM6126A`

Has a dedicated chip-init sequence implemented in `FM6126A_setup()`.

### `SHIFT_DRIVER_ICN2038S`

Currently uses the generic runtime path. There is no chip-specific init or timing path for it yet.

### `SHIFT_DRIVER_MBI5124`

Currently uses the generic runtime path. There is no chip-specific init or timing path for it yet.

### `SHIFT_DRIVER_DP3246`

Has a dedicated startup sequence in `DP3246_setup()` and a dedicated runtime timing path.

This chip requires a special latch/clock relationship and a startup "magic latch/clock sequence".

### `SHIFT_DRIVER_RUL6024`

Has a dedicated startup sequence in `RUL6024_setup()`, ported from the reference `../hub75` implementation.

## LineDecoder

`LineDecoder` selects how row selection is generated.

### `LINE_DECODER_TYPE138`

Standard direct row-address output for `A/B/C/D/E` style panels using the normal row-address PIO program.

### `LINE_DECODER_TYPE595`

Discrete shift-register row decoder path. This uses a dedicated PIO row program and payload encoding for `595`-style row selection.

This is distinct from the `SM5266P` / `SM5368P` path.

### `LINE_DECODER_TYPE_DIRECT`

Direct row addressing path. Use this when the panel behaves like a normal directly-addressed HUB75 row decoder without the `595` serial-row behavior.

### `LINE_DECODER_SM5266P`

GPIO-stepped serial row decoder path. The row select state is maintained outside the row PIO program and advanced explicitly in software.

### `LINE_DECODER_SM5368P`

Currently uses the same implementation as `LINE_DECODER_SM5266P`.

It is kept as a separate enum because the chip identity matters, even if the current implementation is shared.

## What Is Fully Implemented

Distinct shift-driver implementations:

- `SHIFT_DRIVER_SHIFTREG`
- `SHIFT_DRIVER_FM6126A`
- `SHIFT_DRIVER_DP3246`
- `SHIFT_DRIVER_RUL6024`

Shared-but-intentional shift-driver behavior:

- `SHIFT_DRIVER_FM6124` shares the DP3246-style runtime path, but not the DP3246 startup sequence

Generic-only shift-driver labels at the moment:

- `SHIFT_DRIVER_ICN2038S`
- `SHIFT_DRIVER_MBI5124`

Distinct line-decoder implementations:

- `LINE_DECODER_TYPE138`
- `LINE_DECODER_TYPE595`
- `LINE_DECODER_TYPE_DIRECT`
- `LINE_DECODER_SM5266P`

Shared-but-intentional line-decoder behavior:

- `LINE_DECODER_SM5368P` currently shares the `SM5266P` implementation

## Notes On Panel Matching

The enum names describe the intended chip family, but some panels only work correctly with a different timing path than the chip labels might suggest.

Examples:

- some FM6124-family panels work best with `SHIFT_DRIVER_FM6124`
- some DP3246-like timing panels need the DP3246 runtime path but not the DP3246 startup sequence
- some `SM5368P` panels behave like the current `SM5266P` software-stepped row-decoder path

So the enums are best understood as "select this driver behavior" rather than "the PCB is guaranteed to contain exactly this chip".

## Dual-Head Support

If `CLK2/LAT2/OE2` differ from `CLK/LAT/OE`, the driver scans the left and right heads in alternating phases while sharing the framebuffer and row state.

This is intended for setups such as:

- `2 x 128x64` panels driven as one `256x64` display

## Library Metadata

Current library metadata from [`library.json`](library.json):

- name: `zedmd-pico-drivers`
- version: `0.3.0`
- framework: `arduino`
- platform: `raspberrypi`
- source directory: `drivers/hub75`

## Status

This repository currently contains the core driver code only. It does not include a full example set or build documentation in this trimmed form.
