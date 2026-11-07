# ocn-ePaper firmware

Firmware for the ocn-ePaper board's MSP430FR2111 and its Good Display GDEY0213B74 e-paper panel. It counts
minutes from power-up and shows HH:MM in large seven-segment digits, freezing at 99:59. The panel keeps the
last count when the board powers down.

Adapted from the [ocn-LCD](https://github.com/z2amiller/ocn-LCD) firmware, which does the same on a segment
LCD. The build is about 1.9 KB; the FR2111 has 3,712 bytes of FRAM and 1 KB of RAM.

Status: running on the first board (2026-11-06). The default orientation reads upright with the MAGNET
DISABLE end on the right.

## Measured on the first board

A Nordic PPK2 supplying 3.0 V in place of the cell, on Q1's side of R2, with nothing on J1. That board has
1N4148WS diodes in the booster (D1-D3) in place of the specified 1N5819WS.

| | Charge | Time | Peak |
|---|---|---|---|
| Partial update (each minute) | 1.04 mC | 0.8 s | 58 mA |
| Full refresh (every sixth update) | 3.25 mC | 2.3 s | 59 mA |
| Power-up, including the crystal start and a full refresh | 3.44 mC | 2.4 s | 59 mA |
| Asleep between updates | about 0.4 µA | | |

That averages about 24 µA over the six-minute cycle of five partial updates and a full refresh, or about
7 years of nonstop counting on a 1,500 mAh CR123A. The updates are nearly all of it: one partial update
costs as much as about 40 minutes asleep, so the update interval is the lever. The sleep figure is
approximate, because the reading is still settling back from each update's 58 mA when the next one comes.

The low sleep current makes the hold-up long. The ~20 µF after the magnet switch holds the board above the
supervisor's ~1.7 V for about a minute at 0.4 µA, so a magnet held on for less than that may not restart
the count (the next minute's update, which the capacitors can't feed, does stop it).

## How it works

- The chip sleeps in LPM3.5, where only the RTC and the 32-byte backup memory keep running. The RTC, clocked
  from the 32.768 kHz crystal, wakes it once a minute. That wake is a reset: `main()` runs again. The count
  lives in backup memory as four BCD digits, since RAM doesn't survive.
- Each wake wakes the panel with a hardware reset, sends it the old and the new count, and starts a partial
  update (about half a second at ~3.5 mA). After every five partial updates comes a full refresh (about
  3.5 s; the panel flashes black and white), which clears the ghosting that partial updates leave. Then the
  panel goes back into deep sleep, where it draws about 1 µA.
- While the panel works, the chip waits in LPM3 for BUSY to fall, with a timer as a timeout, so a missing or
  stuck panel costs a few seconds per update, not a flat battery.
- 99:59 is drawn with a full refresh, then the RTC stops and nothing wakes the chip again.
- Any other reset, such as power returning when the magnet is taken away, starts from 00:00 with a full
  refresh.
- The supply supervisor (SVSH) stays on in LPM3.5, at about 0.24 µA. It resets the chip once the hold-up
  capacitors fall below ~1.8 V after the magnet goes on. Without it, the brownout reset is only guaranteed
  if the supply falls below 0.1 V first, so a magnet held on for tens of seconds and then taken away might
  not restart the count. (The board README's suggestion to turn it off predates this.)
- The panel sleeps in its deep-sleep mode 2, 1 µA typical, which doesn't keep the panel's RAM. Nothing
  needs it to: every update writes both RAMs.

**Drawing without a frame buffer.** The panel's frame is 250 × 122 pixels, 3.8 KB, and the FR2111 has 1 KB
of RAM. `render.c` computes each 16-byte RAM row as it is sent, straight from the digits' geometry, so the
firmware needs no font or picture data at all.

**Telling a wake from a power-up.** As on ocn-LCD: `SYSRSTIV` keeps every reset cause since it was last
emptied, so the code empties it at every start-up and counts a start-up as a wake only when the LPM3.5 wake
is its one cause.

**The panel.** The command sequence follows GxEPD2's driver for this panel (`GxEPD2_213_B74`) and Good
Display's demo code. Both panel RAMs are written on every update (the previous count, then the new one), so a
partial update doesn't depend on anything the panel kept through deep sleep. SPI runs at 4 MHz from an 8 MHz
clock locked to the crystal.

## Source

- `src/main.c`: the count, the wake handling, clocks, pins and LPM3.5.
- `src/epd.c`, `src/epd.h`: the SSD1680 driver: SPI, reset, the update sequence, the BUSY wait and deep sleep.
- `src/render.c`, `src/render.h`: the seven-segment drawing, one RAM row at a time. Plain C, so it also
  builds on the Mac for the preview.
- `tools/preview.c`: `make preview` draws the panel on the Mac as `build/preview.pbm`.
- `tools/ppk.py`: drives a Nordic PPK2 as the supply and measures the current (from ocn-LCD).
- `targets/msp430fr2111.ccxml`: the DSLite target config for a LaunchPad's eZ-FET programmer.

## Toolchain (macOS)

The same as ocn-LCD's:
- **Compiler:** TI/Mitto msp430-gcc 9.3.1.11 in `~/ti/msp430-gcc` (x86_64, under Rosetta 2).
- **Device headers and linker scripts:** TI's msp430-gcc support files in `~/ti/msp430-gcc-support-files/include`.
- **Flasher:** DSLite from Code Composer Studio, at `/Applications/ti/ccs2101/ccs/ccs_base/DebugServer/bin/DSLite`.
- **PPK2 (optional):** `uv venv .venv && uv pip install --python .venv/bin/python ppk2-api numpy`, in this folder.

Other locations work too: set `TI=`, `GCC=`, `SUPPORT=` or `DSLITE=` on the `make` command line.

## Use

Run these from this folder, with the board wired to a LaunchPad as in the board's
[README](../README.md#programming) (cell out).

```
make                              # build
make flash                        # build, program, verify, hard reset
make FLIP=1 flash                 # the picture turned through 180 degrees
make FAST=1 START=9958 flash      # test build: one count a second, from 99:58
make FULL_EVERY=10 flash          # ten partial updates between full refreshes instead of five
make preview DIGITS=1234          # draw 12:34 on the Mac: build/preview.pbm
make DIAG=1 flash                 # log each panel update in backup memory...
make DIAG=1 readlog               # ...and read it back (halts the chip: power-cycle afterwards)
make DIAG=1 LOOP=1 flash          # full refreshes back to back, for metering the booster
make cycle                        # PPK2 power off and on: a clean start, like the magnet coming off
make measure SECONDS=130          # power-cycle, then measure current for 130 s
```

Each variant builds into its own folder under `build/`; use `make -B` after changing `EXTRA`.

- **After flashing, power-cycle the board.** The panel shows 00:00 straight away, but the chip stays in the
  debugger's session after DSLite finishes, and in that state it doesn't wake from LPM3.5. Unplug the
  LaunchPad's USB and plug it back in (or `make cycle` with the PPK2 as the supply).
- **Which way up.** With the board read with the MAGNET DISABLE end on the right, the digits should read
  upright. Nothing on the panel's drawing says which end its first RAM row is, so if they come out upside
  down, reflash with `FLIP=1`. The picture is never mirrored: the RAM order is the one GxEPD2 uses for this
  panel.
- **Without the panel** the firmware still runs: each update waits out its timeouts (about 10 s for the
  power-up refresh, 3 s for a partial update) and carries on.
- **00:00 that never counts, even after a power cycle,** means the crystal (Y1) didn't start: check its
  joints and C9/C10. The firmware gives the crystal about 3 s, then draws 00:00 anyway and sleeps rather than
  spin and flatten the cell.
- **A blank panel.** Flash `DIAG=1`, power-cycle, then `make DIAG=1 readlog`. Bytes 8 onwards say whether
  BUSY rose after the update command and how long it stayed high (see `src/epd.c`); a working panel holds it
  for about 2-3 s on a full refresh. If BUSY never rises, meter C8 (the panel's own VDD): 1.8 V long after
  power-up means the panel is powered but never heard a command, since the firmware's last command puts it
  to sleep, which turns VDD off. On the first board that was a bad joint between U1 and DS2, fixed by
  reflowing again; check DS2 pin 8 (BS1) to ground too, since a floating BS1 selects 3-wire SPI.
- `FAST=1` counts once a second, as far as the panel keeps up: a full refresh takes longer than a second, so
  the count falls behind the clock around them.

## License

The firmware is under the [MIT License](LICENSE); the rest of the repository (the board) is under the
CERN-OHL-P v2. `tools/ppk.py` uses [ppk2-api](https://github.com/IRNAS/ppk2-api-python) (GPL-2.0), which
isn't included here; install it separately as above.
